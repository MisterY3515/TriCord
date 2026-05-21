#include "ui/camera_screen.h"
#include "ui/screen_manager.h"
#include "discord/discord_client.h"
#include "log.h"
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

namespace UI {

CameraScreen::CameraScreen(const std::string &channelId)
    : channelId(channelId), cameraInitialized(false), isCapturing(false),
      isUploading(false), captureReady(false), camBuffer(nullptr),
      camBufferSize(0), previewTexReady(false) {}

CameraScreen::~CameraScreen() {
	deinitCamera();
}

void CameraScreen::onEnter() {
	if (!initCamera()) {
		ScreenManager::getInstance().showToast("Camera init failed");
		ScreenManager::getInstance().returnToPreviousScreen();
	}
}

bool CameraScreen::initCamera() {
	Result res = camInit();
	if (R_FAILED(res)) {
		Logger::log("[Camera] camInit failed: 0x%08lX", res);
		return false;
	}

	res = CAMU_SetSize(SELECT_OUT1, SIZE_DS_LCD, CONTEXT_A);
	if (R_FAILED(res)) {
		Logger::log("[Camera] SetSize failed: 0x%08lX", res);
		camExit();
		return false;
	}

	CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15);
	CAMU_SetNoiseFilter(SELECT_OUT1, true);
	CAMU_SetAutoExposure(SELECT_OUT1, true);
	CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);

	CAMU_GetMaxBytes(&camBufferSize, CAM_WIDTH, CAM_HEIGHT);
	camBuffer = (u16 *)linearAlloc(camBufferSize);
	if (!camBuffer) {
		Logger::log("[Camera] Failed to allocate camera buffer");
		camExit();
		return false;
	}

	// Init preview texture (must be power-of-2 for GPU)
	if (C3D_TexInit(&previewTex, 512, 256, GPU_RGB565)) {
		previewTexReady = true;
		C3D_TexSetFilter(&previewTex, GPU_LINEAR, GPU_LINEAR);
	}

	res = CAMU_Activate(SELECT_OUT1);
	if (R_FAILED(res)) {
		Logger::log("[Camera] Activate failed: 0x%08lX", res);
		linearFree(camBuffer);
		camBuffer = nullptr;
		camExit();
		return false;
	}

	cameraInitialized = true;
	isCapturing = true;
	Logger::log("[Camera] Initialized successfully (%dx%d)", CAM_WIDTH, CAM_HEIGHT);
	return true;
}

void CameraScreen::deinitCamera() {
	if (cameraInitialized) {
		CAMU_StopCapture(PORT_CAM1);
		CAMU_Activate(SELECT_NONE);
		camExit();
		cameraInitialized = false;
		isCapturing = false;
	}
	if (camBuffer) {
		linearFree(camBuffer);
		camBuffer = nullptr;
	}
	if (previewTexReady) {
		C3D_TexDelete(&previewTex);
		previewTexReady = false;
	}
}

bool CameraScreen::captureFrame() {
	if (!cameraInitialized || !camBuffer) return false;

	Handle camReceiveEvent = 0;
	Result res = CAMU_SetReceiving(&camReceiveEvent, camBuffer, PORT_CAM1,
	                                camBufferSize, (s16)(camBufferSize & 0xFFFF));
	if (R_FAILED(res)) {
		Logger::log("[Camera] SetReceiving failed: 0x%08lX", res);
		return false;
	}

	res = CAMU_StartCapture(PORT_CAM1);
	if (R_FAILED(res)) {
		svcCloseHandle(camReceiveEvent);
		Logger::log("[Camera] StartCapture failed: 0x%08lX", res);
		return false;
	}

	res = svcWaitSynchronization(camReceiveEvent, 300000000LL); // 300ms timeout
	svcCloseHandle(camReceiveEvent);

	CAMU_StopCapture(PORT_CAM1);

	if (R_FAILED(res)) {
		Logger::log("[Camera] Capture timeout: 0x%08lX", res);
		return false;
	}

	captureReady = true;

	// Update preview texture
	if (previewTexReady) {
		// Copy RGB565 data into the texture, row by row
		// The texture is 512 wide but our image is 320 wide
		u16 *texData = (u16 *)previewTex.data;
		// citro3d textures use morton/swizzled order, so we use GSPGPU_FlushDataCache
		// and a simpler approach: use C3D_TexUpload which handles tiling
		// Actually, for simplicity, we'll use the subtexture approach
		// The GPU texture format requires tiled/morton order. We need to convert.
		for (int y = 0; y < CAM_HEIGHT; y++) {
			for (int x = 0; x < CAM_WIDTH; x++) {
				// Morton interleave for tiled texture
				int blockX = x / 8;
				int blockY = y / 8;
				int localX = x % 8;
				int localY = y % 8;

				// Morton code for 8x8 block
				static const u8 mortonTable[64] = {
				     0,  1,  4,  5, 16, 17, 20, 21,
				     2,  3,  6,  7, 18, 19, 22, 23,
				     8,  9, 12, 13, 24, 25, 28, 29,
				    10, 11, 14, 15, 26, 27, 30, 31,
				    32, 33, 36, 37, 48, 49, 52, 53,
				    34, 35, 38, 39, 50, 51, 54, 55,
				    40, 41, 44, 45, 56, 57, 60, 61,
				    42, 43, 46, 47, 58, 59, 62, 63
				};

				int mortonOffset = mortonTable[localY * 8 + localX];
				int texOffset = (blockY * (512 / 8) + blockX) * 64 + mortonOffset;

				texData[texOffset] = camBuffer[y * CAM_WIDTH + x];
			}
		}
		GSPGPU_FlushDataCache(previewTex.data, previewTex.size);
	}

	return true;
}

bool CameraScreen::saveBmp(const std::string &path) {
	if (!captureReady || !camBuffer) return false;

	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;

	int w = CAM_WIDTH;
	int h = CAM_HEIGHT;
	int rowSize = w * 3;
	int padding = (4 - (rowSize % 4)) % 4;
	int imageSize = (rowSize + padding) * h;
	int fileSize = 54 + imageSize;

	// BMP Header
	u8 header[54] = {};
	header[0] = 'B'; header[1] = 'M';
	*(int *)&header[2] = fileSize;
	*(int *)&header[10] = 54;
	*(int *)&header[14] = 40;
	*(int *)&header[18] = w;
	*(int *)&header[22] = h;
	*(short *)&header[26] = 1;
	*(short *)&header[28] = 24;
	*(int *)&header[34] = imageSize;

	fwrite(header, 1, 54, f);

	// BMP is bottom-up
	u8 pad[3] = {0, 0, 0};
	for (int y = h - 1; y >= 0; y--) {
		for (int x = 0; x < w; x++) {
			u16 pixel = camBuffer[y * w + x];
			// RGB565 -> BGR888
			u8 r = ((pixel >> 11) & 0x1F) << 3;
			u8 g = ((pixel >> 5) & 0x3F) << 2;
			u8 b = (pixel & 0x1F) << 3;
			u8 bgr[3] = {b, g, r};
			fwrite(bgr, 1, 3, f);
		}
		if (padding > 0) fwrite(pad, 1, padding, f);
	}

	fclose(f);
	Logger::log("[Camera] Saved BMP to %s", path.c_str());
	return true;
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

	if (kDown & KEY_A) {
		// Capture and upload
		if (captureFrame()) {
			std::string tmpPath = "sdmc:/3ds/TriCord/camera_photo.bmp";
			// Ensure directory exists
			mkdir("sdmc:/3ds", 0777);
			mkdir("sdmc:/3ds/TriCord", 0777);
			if (saveBmp(tmpPath)) {
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
		captureFrame();
	}
}

void CameraScreen::renderTop(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackground());

	if (previewTexReady && captureReady) {
		Tex3DS_SubTexture subtex;
		subtex.width = CAM_WIDTH;
		subtex.height = CAM_HEIGHT;
		subtex.left = 0.0f;
		subtex.top = 1.0f;
		subtex.right = (float)CAM_WIDTH / 512.0f;
		subtex.bottom = 1.0f - ((float)CAM_HEIGHT / 256.0f);

		C2D_Image img = {&previewTex, &subtex};

		// Center on top screen (400x240), scale to fill
		float scaleX = 400.0f / CAM_WIDTH;
		float scaleY = 240.0f / CAM_HEIGHT;
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
	                 "Point the outer camera and press A", 320.0f);

	drawCenteredText(220.0f, 0.9f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
	                 "\uE000: Capture  \uE001: Back", 320.0f);
}

} // namespace UI
