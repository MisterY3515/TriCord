#include "audio/audio_manager.h"
#include "log.h"
#include <3ds.h>
#include <cstring>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Audio {

namespace {
constexpr float kVoicePlaybackRate = 48000.0f;
constexpr size_t kPlaybackBufferMs = 120;
constexpr size_t kBytesPerMonoSample = sizeof(int16_t);
} // namespace

AudioManager &AudioManager::getInstance() {
	static AudioManager instance;
	return instance;
}

AudioManager::AudioManager()
    : currentPlayBuf(0), micBuffer(nullptr), micBufSize(0), capturing(false), lastMicPos(0), ndspReady(false),
      micReady(false) {
	// Size for ~40ms of 48kHz mono audio (48kHz * 2 bytes * 0.04 = 3840 bytes)
	playbackBufferSize = 48000 * 2 * 0.04;
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
	static int16_t *muteBuf = nullptr;
	static int16_t *unmuteBuf = nullptr;
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

	if (!muteBuf) {
		muteBuf = (int16_t *)linearAlloc(duration * 2);
		if (muteBuf) {
			for (int i = 0; i < duration; i++) {
				float freq = 660.0f; // Tono costante medio
				float vol = (i < duration / 2) ? 6000.0f : 0.0f; // Beep corto
				muteBuf[i] = (int16_t)(vol * sinf(2.0f * M_PI * freq * i / 16000.0f));
			}
			DSP_FlushDataCache(muteBuf, duration * 2);
		}
	}

	if (!unmuteBuf) {
		unmuteBuf = (int16_t *)linearAlloc(duration * 2);
		if (unmuteBuf) {
			for (int i = 0; i < duration; i++) {
				float freq = 990.0f; // Tono costante alto
				float vol = (i < duration / 2) ? 6000.0f : 0.0f; // Beep corto
				unmuteBuf[i] = (int16_t)(vol * sinf(2.0f * M_PI * freq * i / 16000.0f));
			}
			DSP_FlushDataCache(unmuteBuf, duration * 2);
		}
	}

	int16_t *targetBuf = nullptr;
	switch(sound) {
		case SystemSound::JOIN: targetBuf = joinBuf; break;
		case SystemSound::LEAVE: targetBuf = leaveBuf; break;
		case SystemSound::MUTE: targetBuf = muteBuf; break;
		case SystemSound::UNMUTE: targetBuf = unmuteBuf; break;
	}
	
	if (!targetBuf) return;

	static ndspWaveBuf soundBufs[4];
	static bool soundBufsInitialized = false;
	static int nextSoundBuf = 0;
	if (!soundBufsInitialized) {
		for (auto &buf : soundBufs) {
			memset(&buf, 0, sizeof(buf));
			buf.status = NDSP_WBUF_DONE;
		}
		soundBufsInitialized = true;
	}

	ndspWaveBuf *targetWaveBuf = nullptr;
	for (int i = 0; i < 4; i++) {
		ndspWaveBuf &candidate = soundBufs[(nextSoundBuf + i) % 4];
		if (candidate.status == NDSP_WBUF_DONE) {
			targetWaveBuf = &candidate;
			nextSoundBuf = (nextSoundBuf + i + 1) % 4;
			break;
		}
	}

	if (!targetWaveBuf) {
		Logger::log("[Audio] Skipping system sound because all NDSP sound buffers are busy");
		return;
	}

	memset(targetWaveBuf, 0, sizeof(*targetWaveBuf));
	targetWaveBuf->data_vaddr = targetBuf;
	targetWaveBuf->nsamples = duration;
	targetWaveBuf->status = NDSP_WBUF_DONE;
	
	ndspChnSetInterp(1, NDSP_INTERP_LINEAR);
	ndspChnSetRate(1, 16000.0f);
	ndspChnSetFormat(1, NDSP_FORMAT_MONO_PCM16);
	
	ndspChnWaveBufAdd(1, targetWaveBuf);
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
		ndspChnSetRate(0, kVoicePlaybackRate);
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
			micReady = false;
		} else {
			micReady = true;
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
		ndspReady = false;
	}
	if (micBuffer) micExit();
	if (micBuffer) {
		linearFree(micBuffer);
		micBuffer = nullptr;
	}
	micReady = false;
}

void AudioManager::queuePcm(const int16_t *pcm, size_t samples) {
	if (!pcm || samples == 0 || !ndspReady) return;
	if (!playbackBuffer[currentPlayBuf]) return;
	
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

bool AudioManager::startCapture() {
	if (capturing) return true;
	if (!micBuffer || !micReady) {
		Logger::log("[Audio] Cannot start MIC capture: microphone is not initialized");
		return false;
	}
	
	// sharedMemAudioSize should be at most bufferSize - 4
	// Use 32730Hz (max available on 3DS) to get closer to 48kHz requirements
	u32 audioSize = micBufSize - 4;
	Result res = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_32730, 0, audioSize, true);
	if (R_FAILED(res)) {
		Logger::log("[Audio] MICU_StartSampling failed (0x%08lX)", res);
		capturing = false;
		return false;
	}
	capturing = true;
	lastMicPos = 0;
	Logger::log("[Audio] Started MIC capture at 32730Hz");
	return true;
}

void AudioManager::stopCapture() {
	if (!capturing) return;
	Result res = MICU_StopSampling();
	if (R_FAILED(res)) {
		Logger::log("[Audio] MICU_StopSampling failed (0x%08lX)", res);
	}
	capturing = false;
	Logger::log("[Audio] Stopped MIC capture");
}

bool AudioManager::hasNewSamples() const {
	if (!capturing || !micBuffer || !micReady) return false;
	const u32 limit = micBufSize >= 4 ? micBufSize - 4 : 0;
	if (limit == 0 || lastMicPos >= limit) return false;
	const u32 currentPos = micGetLastSampleOffset();
	if (currentPos >= limit) {
		return false;
	}
	return currentPos != lastMicPos;
}

size_t AudioManager::readSamples(int16_t *buffer, size_t maxSamples) {
	if (!buffer || maxSamples == 0 || !capturing || !micBuffer || !micReady) return 0;
	
	u32 limit = micBufSize >= 4 ? micBufSize - 4 : 0;
	if (limit == 0) return 0;
	if (lastMicPos >= limit) {
		Logger::log("[Audio] MIC read position out of range (%lu >= %lu), resetting", (unsigned long)lastMicPos,
		            (unsigned long)limit);
		lastMicPos = 0;
	}

	u32 currentPos = micGetLastSampleOffset();
	if (currentPos >= limit) {
		Logger::log("[Audio] MIC sample offset out of range (%lu >= %lu), dropping frame", (unsigned long)currentPos,
		            (unsigned long)limit);
		return 0;
	}
	if (currentPos == lastMicPos) return 0;
	
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
