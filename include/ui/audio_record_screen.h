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
	void startRecording();
	void stopRecording();
	bool saveWav(const std::string &path);
	void uploadAudio(const std::string &path);

	std::string channelId;

	bool isRecording;
	bool isUploading;

	static constexpr u32 SAMPLE_RATE = 32730;
	static constexpr u32 MAX_RECORD_SECONDS = 60;

	std::vector<int16_t> recordedAudio;
	u64 recordStartTime;
};

} // namespace UI
