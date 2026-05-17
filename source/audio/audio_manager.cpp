#include "audio/audio_manager.h"
#include "log.h"
#include <malloc.h>
#include <string.h>

namespace Audio {

AudioManager &AudioManager::getInstance() {
	static AudioManager instance;
	return instance;
}

AudioManager::AudioManager() : currentPlayBuf(0), micBuffer(nullptr), micBufSize(0), capturing(false), lastMicPos(0) {
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
	ndspInit();
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, 16000.0f);
	ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
	
	// Init MIC
	micInit(micBuffer, micBufSize);
}

void AudioManager::shutdown() {
	stopCapture();
	ndspExit();
	micExit();
	if (micBuffer) {
		linearFree(micBuffer);
		micBuffer = nullptr;
	}
}

void AudioManager::queuePcm(const int16_t *pcm, size_t samples) {
	if (!pcm || samples == 0) return;
	
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
	
	micBufSize = 16000 * 2 * 2; // 2 seconds of 16kHz mono
	micBuffer = (u8 *)linearAlloc(micBufSize);
	memset(micBuffer, 0, micBufSize);
	
	micSetSampleData(micBuffer, micBufSize, 0); // Not needed? wait, micInit sets it
	MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, micBufSize, true);
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
	if (!capturing) return false;
	u32 pos = MICU_GetLastSampleOffset(); // This might not be right...
	// On 3DS we actually read memory offset. 
	// The right way is checking micGetLastSampleOffset();
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
