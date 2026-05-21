#pragma once

#include "ui/screen_manager.h"
#include <3ds.h>
#include <string>
#include <vector>

namespace UI {

class AudioRecordScreen : public Screen {
  public:
	AudioRecordScreen(const std::string &channelId);
	~AudioRecordScreen();

	void onEnter() override;
	void update() override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;

  private:
	bool initMic();
	void deinitMic();
	void startRecording();
	void stopRecording();
	bool saveWav(const std::string &path);
	void uploadAudio(const std::string &path);

	std::string channelId;

	bool micInitialized;
	bool isRecording;
	bool isUploading;

	static constexpr u32 SAMPLE_RATE = 16000;
	static constexpr u32 MAX_RECORD_SECONDS = 60;
	static constexpr u32 BUFFER_SIZE = SAMPLE_RATE * MAX_RECORD_SECONDS * 2; // 16-bit PCM

	u8 *micBuffer;
	u32 recordedBytes;
	u64 recordStartTime;
};

} // namespace UI
