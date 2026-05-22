#include "ui/camera_screen.h"
#include "ui/screen_manager.h"
#include "discord/discord_client.h"
#include "log.h"
#include <cstring>
#include <cstdio>
#include "3dsware/camera.h"
#include "utils/image_utils.h"
#include "3dsware/fs.h"

namespace UI {

CameraScreen::CameraScreen(const std::string &channelId)
    : channelId(channelId), isCapturing(false),
      isUploading(false), captureReady(false), previewTexReady(false) {}

CameraScreen::~CameraScreen() {
	deinitCamera();
}

void CameraScreen::onEnter() {
	if (!Hardware::Camera::getInstance().init()) {
		ScreenManager::getInstance().showToast("Camera init failed");
		ScreenManager::getInstance().returnToPreviousScreen();
		return;
	}
	
	if (C3D_TexInit(&previewTex, 512, 256, GPU_RGB565)) {
		previewTexReady = true;
		C3D_TexSetFilter(&previewTex, GPU_LINEAR, GPU_LINEAR);
	}

	isCapturing = true;
	Logger::log("[Camera] Initialized successfully via 3DSware");
}

void CameraScreen::deinitCamera() {
	isCapturing = false;
	Hardware::Camera::getInstance().shutdown();
	if (previewTexReady) {
		C3D_TexDelete(&previewTex);
		previewTexReady = false;
	}
}

void CameraScreen::uploadPhoto(const std::string &path) {
	isUploading = true;
	ScreenManager::getInstance().showToast("Uploading photo...");

	Discord::DiscordClient::getInstance().uploadFile(
	    channelId, path, "",
	    [this](const Discord::Message &msg, bool success, int errorCode) {
		    isUploading = false;
		    if (success) {
			    ScreenManager::getInstance().showToast("Photo sent!");
			    ScreenManager::getInstance().returnToPreviousScreen();
		    } else {
			    ScreenManager::getInstance().showToast("Upload failed: " + std::to_string(errorCode));
		    }
	    });
}

void CameraScreen::update() {
	if (isUploading) return;

	u32 kDown = hidKeysDown();

	if (kDown & KEY_B) {
		deinitCamera();
		ScreenManager::getInstance().returnToPreviousScreen();
		return;
	}

	if (kDown & KEY_R) {
		Hardware::Camera::getInstance().flipCamera();
		return;
	}

	if (kDown & KEY_A) {
		// Capture and upload
		std::string tmpPath = "sdmc:/3ds/TriCord/camera_photo.jpg";
		Hardware::FS::ensureDirectory("sdmc:/3ds/TriCord");
		
		u16* buffer = Hardware::Camera::getInstance().getInternalBuffer();
		if (Hardware::Camera::getInstance().isReady() && buffer) {
			if (Utils::Image::saveJPG(tmpPath.c_str(), buffer, 320, 240)) {
				deinitCamera();
				uploadPhoto(tmpPath);
			} else {
				ScreenManager::getInstance().showToast("Failed to save photo");
			}
		} else {
			ScreenManager::getInstance().showToast("Capture failed");
		}
		return;
	}

	// Live preview: continuously capture frames
	if (isCapturing) {
		if (Hardware::Camera::getInstance().captureToTexture(&previewTex)) {
			captureReady = true;
		}
	}
}

void CameraScreen::renderTop(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackground());

	if (previewTexReady && captureReady) {
		Tex3DS_SubTexture subtex;
		subtex.width = 320;
		subtex.height = 240;
		subtex.left = 0.0f;
		subtex.top = 1.0f;
		subtex.right = 320.0f / 512.0f;
		subtex.bottom = 1.0f - (240.0f / 256.0f);

		C2D_Image img = {&previewTex, &subtex};

		// The physical image is 320x240. We want to stretch it to 400x240 (or keep aspect ratio and center it).
		// Let's stretch it to 400x240.
		float scaleX = 400.0f / 320.0f;
		float scaleY = 240.0f / 240.0f;
		C2D_DrawImageAt(img, 0.0f, 0.0f, 0.5f, nullptr, scaleX, scaleY);
	} else {
		drawCenteredText(100.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
		                 "Initializing camera...", 400.0f);
	}

	if (isUploading) {
		C2D_DrawRectSolid(0, 0, 0.9f, 400, 240, C2D_Color32(0, 0, 0, 160));
		drawCenteredText(110.0f, 0.95f, 0.7f, 0.7f, ScreenManager::colorAccent(),
		                 "Uploading...", 400.0f);
	}
}

void CameraScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

	drawCenteredText(80.0f, 0.5f, 0.55f, 0.55f, ScreenManager::colorText(),
	                 "Camera", 320.0f);

	drawCenteredText(120.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
	                 "Point the camera and press A", 320.0f);

	drawCenteredText(220.0f, 0.9f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
	                 "\uE000: Capture  R: Flip  \uE001: Back", 320.0f);
}

} // namespace UI
