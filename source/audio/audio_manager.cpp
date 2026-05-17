#include "audio/audio_manager.h"
#include "log.h"
#include <3ds.h>
#include <cstring>

namespace Audio {

AudioManager &AudioManager::getInstance() {
	static AudioManager instance;
	return instance;
}

AudioManager::AudioManager() : currentPlayBuf(0), micBuffer(nullptr), micBufSize(0), capturing(false), lastMicPos(0), ndspReady(false) {
	// Size for ~40ms of 16kHz mono audio (16kHz * 2 bytes * 0.04 = 1280 bytes)
	playbackBufferSize = 16000 * 2 * 0.04;
	for (int i = 0; i < 2; i++) {
		playbackBuffer[i] = (int16_t *)linearAlloc(playbackBufferSize);
		memset(playbackBuffer[i], 0, playbackBufferSize);
		memset(&waveBuf[i], 0, sizeof(ndspWaveBuf));
		waveBuf[i].data_vaddr = playbackBuffer[i];
		waveBuf[i].nsamples = playbackBufferSize / 2;
		waveBuf[i].status = NDSP_WBUF_DONE;
	}
}

AudioManager::~AudioManager() {
	shutdown();
	for (int i = 0; i < 2; i++) {
		if (playbackBuffer[i]) linearFree(playbackBuffer[i]);
	}
}

void AudioManager::init() {
	Result res = ndspInit();
	if (R_FAILED(res)) {
		Logger::log("[Audio] ndspInit failed (0x%08lX) - DSP firmware may not be dumped", res);
		ndspReady = false;
	} else {
		ndspReady = true;
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
		ndspChnSetRate(0, 16000.0f);
		ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
	}
	
	// Pre-allocate MIC buffer (linearAlloc required for shared memory)
	micBufSize = 0x10000; // 64KB
	micBuffer = (u8 *)linearMemAlign(micBufSize, 0x1000);
	if (micBuffer) {
		memset(micBuffer, 0, micBufSize);
		res = micInit(micBuffer, micBufSize);
		if (R_FAILED(res)) {
			Logger::log("[Audio] micInit failed (0x%08lX)", res);
			linearFree(micBuffer);
			micBuffer = nullptr;
		}
	} else {
		Logger::log("[Audio] Failed to allocate MIC buffer!");
	}
}

void AudioManager::shutdown() {
	stopCapture();
	if (ndspReady) ndspExit();
	if (micBuffer) micExit();
	if (micBuffer) {
		linearFree(micBuffer);
		micBuffer = nullptr;
	}
}

void AudioManager::queuePcm(const int16_t *pcm, size_t samples) {
	if (!pcm || samples == 0 || !ndspReady) return;
	
	size_t bytesToCopy = samples * 2;
	if (bytesToCopy > playbackBufferSize) bytesToCopy = playbackBufferSize;

	if (waveBuf[currentPlayBuf].status == NDSP_WBUF_DONE) {
		memcpy(playbackBuffer[currentPlayBuf], pcm, bytesToCopy);
		waveBuf[currentPlayBuf].nsamples = bytesToCopy / 2;
		DSP_FlushDataCache(playbackBuffer[currentPlayBuf], bytesToCopy);
		ndspChnWaveBufAdd(0, &waveBuf[currentPlayBuf]);
		currentPlayBuf = (currentPlayBuf + 1) % 2;
	}
}

void AudioManager::startCapture() {
	if (capturing) return;
	if (!micBuffer) return;
	
	// sharedMemAudioSize should be at most bufferSize - 4
	u32 audioSize = micBufSize - 4;
	MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, audioSize, true);
	capturing = true;
	lastMicPos = 0;
	Logger::log("[Audio] Started MIC capture");
}

void AudioManager::stopCapture() {
	if (!capturing) return;
	MICU_StopSampling();
	capturing = false;
	Logger::log("[Audio] Stopped MIC capture");
}

bool AudioManager::hasNewSamples() const {
	if (!capturing || !micBuffer) return false;
	return micGetLastSampleOffset() != lastMicPos;
}

size_t AudioManager::readSamples(int16_t *buffer, size_t maxSamples) {
	if (!capturing) return 0;
	
	u32 currentPos = micGetLastSampleOffset();
	if (currentPos == lastMicPos) return 0;
	
	u32 bytesAvailable;
	if (currentPos >= lastMicPos) {
		bytesAvailable = currentPos - lastMicPos;
	} else {
		bytesAvailable = micBufSize - lastMicPos + currentPos;
	}
	
	size_t samplesAvailable = bytesAvailable / 2;
	if (samplesAvailable > maxSamples) samplesAvailable = maxSamples;
	
	// Linear copy considering ring buffer wrap
	size_t firstPart = micBufSize - lastMicPos;
	size_t bytesToRead = samplesAvailable * 2;
	
	if (bytesToRead <= firstPart) {
		memcpy(buffer, micBuffer + lastMicPos, bytesToRead);
	} else {
		memcpy(buffer, micBuffer + lastMicPos, firstPart);
		memcpy((u8*)buffer + firstPart, micBuffer, bytesToRead - firstPart);
	}
	
	lastMicPos = (lastMicPos + bytesToRead) % micBufSize;
	return samplesAvailable;
}

} // namespace Audio
