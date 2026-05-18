#include "audio/audio_manager.h"
#include "log.h"
#include <3ds.h>
#include <cstring>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Audio {

AudioManager &AudioManager::getInstance() {
	static AudioManager instance;
	return instance;
}

AudioManager::AudioManager() : currentPlayBuf(0), micBuffer(nullptr), micBufSize(0), capturing(false), lastMicPos(0), ndspReady(false) {
	// Size for ~120ms of 16kHz mono audio (16kHz * 2 bytes * 0.12 = 3840 bytes)
	// Questo evita troncamenti se i pacchetti sono più grandi di 40ms
	playbackBufferSize = 3840;
	for (int i = 0; i < NUM_WAVE_BUFS; i++) {
		playbackBuffer[i] = (int16_t *)linearAlloc(playbackBufferSize);
		if (playbackBuffer[i]) {
			memset(playbackBuffer[i], 0, playbackBufferSize);
		}
		memset(&waveBuf[i], 0, sizeof(ndspWaveBuf));
		waveBuf[i].data_vaddr = playbackBuffer[i];
		waveBuf[i].nsamples = playbackBufferSize / 2;
		waveBuf[i].status = NDSP_WBUF_DONE;
	}
}

void AudioManager::playSystemSound(SystemSound sound) {
	if (!ndspReady) return;

	// Buffer statici per evitare leak di linearAlloc
	static int16_t *joinBuf = nullptr;
	static int16_t *leaveBuf = nullptr;
	const int duration = 16000 * 0.15; // 150ms

	if (!joinBuf) {
		joinBuf = (int16_t *)linearAlloc(duration * 2);
		if (joinBuf) {
			for (int i = 0; i < duration; i++) {
				float freq = 440.0f + i * 2.0f;
				joinBuf[i] = (int16_t)(8000.0f * sinf(2.0f * M_PI * freq * i / 16000.0f));
			}
			DSP_FlushDataCache(joinBuf, duration * 2);
		}
	}

	if (!leaveBuf) {
		leaveBuf = (int16_t *)linearAlloc(duration * 2);
		if (leaveBuf) {
			for (int i = 0; i < duration; i++) {
				float freq = 880.0f - i * 2.0f;
				leaveBuf[i] = (int16_t)(8000.0f * sinf(2.0f * M_PI * freq * i / 16000.0f));
			}
			DSP_FlushDataCache(leaveBuf, duration * 2);
		}
	}

	int16_t *targetBuf = (sound == SystemSound::JOIN) ? joinBuf : leaveBuf;
	if (!targetBuf) return;

	static ndspWaveBuf sBuf;
	// Se il buffer precedente è ancora in esecuzione, meglio non sovrascrivere bruscamente
	// ma per i suoni di sistema brevi va bene così.
	memset(&sBuf, 0, sizeof(sBuf));
	sBuf.data_vaddr = targetBuf;
	sBuf.nsamples = duration;
	sBuf.status = NDSP_WBUF_DONE;
	
	ndspChnSetInterp(1, NDSP_INTERP_LINEAR);
	ndspChnSetRate(1, 16000.0f);
	ndspChnSetFormat(1, NDSP_FORMAT_MONO_PCM16);
	
	ndspChnWaveBufAdd(1, &sBuf);
}

AudioManager::~AudioManager() {
	shutdown();
	for (int i = 0; i < NUM_WAVE_BUFS; i++) {
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
	if (ndspReady) {
		ndspChnWaveBufClear(0);
		ndspChnWaveBufClear(1);
		ndspExit();
	}
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
		currentPlayBuf = (currentPlayBuf + 1) % NUM_WAVE_BUFS;
	}
}

void AudioManager::startCapture() {
	if (capturing) return;
	if (!micBuffer) return;
	
	// sharedMemAudioSize should be at most bufferSize - 4
	// Use 16360Hz (closest to 16000Hz available on 3DS)
	u32 audioSize = micBufSize - 4;
	MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, audioSize, true);
	capturing = true;
	lastMicPos = 0;
	Logger::log("[Audio] Started MIC capture at 16360Hz");
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
	
	u32 limit = micBufSize - 4;
	u32 bytesAvailable;
	if (currentPos >= lastMicPos) {
		bytesAvailable = currentPos - lastMicPos;
	} else {
		bytesAvailable = limit - lastMicPos + currentPos;
	}
	
	size_t samplesAvailable = bytesAvailable / 2;
	if (samplesAvailable > maxSamples) samplesAvailable = maxSamples;
	
	// Linear copy considering ring buffer wrap
	size_t firstPart = limit - lastMicPos;
	size_t bytesToRead = samplesAvailable * 2;
	
	if (bytesToRead <= firstPart) {
		memcpy(buffer, micBuffer + lastMicPos, bytesToRead);
	} else {
		memcpy(buffer, micBuffer + lastMicPos, firstPart);
		memcpy((u8*)buffer + firstPart, micBuffer, bytesToRead - firstPart);
	}
	
	lastMicPos = (lastMicPos + bytesToRead) % limit;
	return samplesAvailable;
}

} // namespace Audio
