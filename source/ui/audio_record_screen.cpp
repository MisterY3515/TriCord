#include "ui/audio_record_screen.h"
#include "ui/screen_manager.h"
#include "discord/discord_client.h"
#include "log.h"
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

#include "3dsware/mic.h"

namespace UI {

AudioRecordScreen::AudioRecordScreen(const std::string &channelId)
    : channelId(channelId), isRecording(false),
      isUploading(false), recordStartTime(0) {}

AudioRecordScreen::~AudioRecordScreen() {
	if (isRecording) stopRecording();
}

void AudioRecordScreen::onEnter() {
	if (!Hardware::Mic::getInstance().isReady()) {
		if (!Hardware::Mic::getInstance().init()) {
			char errMsg[64];
			snprintf(errMsg, sizeof(errMsg), "Mic init failed: 0x%08lX", Hardware::Mic::getInstance().getLastError());
			ScreenManager::getInstance().showToast(errMsg);
			ScreenManager::getInstance().returnToPreviousScreen();
		}
	}
}

void AudioRecordScreen::startRecording() {
	if (isRecording) return;

	recordedAudio.clear();
	recordedAudio.reserve(SAMPLE_RATE * MAX_RECORD_SECONDS);

	if (!Hardware::Mic::getInstance().startStreaming()) {
		Logger::log("[Audio] StartSampling failed");
		ScreenManager::getInstance().showToast("Failed to start recording");
		return;
	}

	isRecording = true;
	recordStartTime = osGetTime();
	Logger::log("[Audio] Recording started");
}

void AudioRecordScreen::stopRecording() {
	if (!isRecording) return;

	Hardware::Mic::getInstance().stopStreaming();
	isRecording = false;

	Logger::log("[Audio] Recording stopped, %zu samples captured", recordedAudio.size());
}

bool AudioRecordScreen::saveWav(const std::string &path) {
	if (recordedAudio.empty()) return false;

	u32 actualBytes = recordedAudio.size() * 2;
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;

	u32 sampleRate = SAMPLE_RATE;
	u16 bitsPerSample = 16;
	u16 channels = 1;
	u32 byteRate = sampleRate * channels * bitsPerSample / 8;
	u16 blockAlign = channels * bitsPerSample / 8;
	u32 fileSize = 36 + actualBytes;

	// RIFF header
	fwrite("RIFF", 1, 4, f);
	fwrite(&fileSize, 4, 1, f);
	fwrite("WAVE", 1, 4, f);

	// fmt chunk
	fwrite("fmt ", 1, 4, f);
	u32 fmtSize = 16;
	fwrite(&fmtSize, 4, 1, f);
	u16 audioFormat = 1; // PCM
	fwrite(&audioFormat, 2, 1, f);
	fwrite(&channels, 2, 1, f);
	fwrite(&sampleRate, 4, 1, f);
	fwrite(&byteRate, 4, 1, f);
	fwrite(&blockAlign, 2, 1, f);
	fwrite(&bitsPerSample, 2, 1, f);

	// data chunk
	fwrite("data", 1, 4, f);
	fwrite(&actualBytes, 4, 1, f);

	// Write PCM data
	fwrite(recordedAudio.data(), 1, actualBytes, f);

	fclose(f);
	Logger::log("[Audio] Saved WAV to %s (%u bytes)", path.c_str(), actualBytes);
	return true;
}

void AudioRecordScreen::uploadAudio(const std::string &path) {
	isUploading = true;
	ScreenManager::getInstance().showToast("Uploading audio...");

	Discord::DiscordClient::getInstance().uploadFile(
	    channelId, path, "",
	    [this](const Discord::Message &msg, bool success, int errorCode) {
		    isUploading = false;
		    if (success) {
			    ScreenManager::getInstance().showToast("Audio sent!");
			    ScreenManager::getInstance().returnToPreviousScreen();
		    } else {
			    ScreenManager::getInstance().showToast("Upload failed: " + std::to_string(errorCode));
		    }
	    });
}

void AudioRecordScreen::update() {
	if (isUploading) return;

	u32 kDown = hidKeysDown();

	if (kDown & KEY_B) {
		if (isRecording) {
			stopRecording();
		}
		ScreenManager::getInstance().returnToPreviousScreen();
		return;
	}

	if (kDown & KEY_A) {
		if (!isRecording) {
			startRecording();
		} else {
			stopRecording();

			std::string tmpPath = "sdmc:/3ds/TriCord/voice_record.wav";
			mkdir("sdmc:/3ds", 0777);
			mkdir("sdmc:/3ds/TriCord", 0777);

			if (saveWav(tmpPath)) {
				uploadAudio(tmpPath);
			} else {
				ScreenManager::getInstance().showToast("Failed to save audio");
			}
		}
	}

	// Auto-stop at max duration
	if (isRecording) {
		int16_t buf[2048];
		size_t samplesRead = Hardware::Mic::getInstance().readSamples(buf, 2048);
		if (samplesRead > 0) {
			recordedAudio.insert(recordedAudio.end(), buf, buf + samplesRead);
		}

		u64 elapsed = osGetTime() - recordStartTime;
		if (elapsed >= MAX_RECORD_SECONDS * 1000) {
			stopRecording();
			ScreenManager::getInstance().showToast("Max duration reached");
		}
	}
}

void AudioRecordScreen::renderTop(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackground());

	drawCenteredText(40.0f, 0.5f, 0.7f, 0.7f, ScreenManager::colorText(),
	                 "Audio Recorder", 400.0f);

	if (isRecording) {
		u64 elapsed = osGetTime() - recordStartTime;
		int seconds = (int)(elapsed / 1000);
		int minutes = seconds / 60;
		seconds %= 60;

		char timeStr[16];
		snprintf(timeStr, sizeof(timeStr), "%02d:%02d", minutes, seconds);

		// Pulsing red circle
		float pulse = 1.0f + 0.3f * sinf((float)elapsed / 500.0f);
		float radius = 15.0f * pulse;
		drawCircle(200.0f, 120.0f, 0.5f, radius, C2D_Color32(255, 60, 60, 255));

		drawCenteredText(155.0f, 0.5f, 0.8f, 0.8f, C2D_Color32(255, 60, 60, 255),
		                 timeStr, 400.0f);

		drawCenteredText(190.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
		                 "Recording...", 400.0f);
	} else if (isUploading) {
		drawCenteredText(120.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorAccent(),
		                 "Uploading...", 400.0f);
	} else {
		drawCircle(200.0f, 120.0f, 0.5f, 20.0f, C2D_Color32(100, 100, 100, 255));

		drawCenteredText(160.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(),
		                 "Press A to start recording", 400.0f);
	}
}

void AudioRecordScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

	drawCenteredText(80.0f, 0.5f, 0.55f, 0.55f, ScreenManager::colorText(),
	                 "Voice Message", 320.0f);

	if (isRecording) {
		drawCenteredText(120.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
		                 "Press A to stop and send", 320.0f);
	} else {
		drawCenteredText(120.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
		                 "Press A to start recording", 320.0f);

		char maxStr[32];
		snprintf(maxStr, sizeof(maxStr), "Max duration: %us", (unsigned int)MAX_RECORD_SECONDS);
		drawCenteredText(145.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
		                 maxStr, 320.0f);
	}

	drawCenteredText(220.0f, 0.9f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
	                 "\uE000: Record/Stop  \uE001: Cancel", 320.0f);
}

} // namespace UI
