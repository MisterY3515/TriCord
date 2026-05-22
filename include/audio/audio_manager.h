#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <3ds.h>
#include <cstdint>
#include <vector>

namespace Audio {

enum class SystemSound { JOIN, LEAVE, MUTE, UNMUTE };

class AudioManager {
  public:
	static AudioManager &getInstance();

	void init();
	void shutdown();

	// Playback (NDSP) — 48kHz mono per il flusso voice Discord
	void queuePcm(const int16_t *pcm, size_t samples);
	
	// Suoni di sistema
	void playSystemSound(SystemSound sound);

	// Capture (MIC) — 16360Hz nativi del 3DS, poi risincronizzati a 48kHz dal backend voice
	bool startCapture();
	void stopCapture();
	bool hasNewSamples() const;
	size_t readSamples(int16_t *buffer, size_t maxSamples);

  private:
	AudioManager();
	~AudioManager();

	AudioManager(const AudioManager &) = delete;
	AudioManager &operator=(const AudioManager &) = delete;

	// NDSP multi-buffer playback per evitare drop (12 buffer = ~240ms di audio)
	static const int NUM_WAVE_BUFS = 12;
	ndspWaveBuf waveBuf[NUM_WAVE_BUFS];
	int16_t *playbackBuffer[NUM_WAVE_BUFS];
	int currentPlayBuf;
	size_t playbackBufferSize; // in bytes

	// MIC capture ora delegato a Hardware::Mic in 3DSware
	bool ndspReady;
};

} // namespace Audio

#endif // AUDIO_MANAGER_H
