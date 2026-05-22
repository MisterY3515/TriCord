#pragma once

#include "ui/screen_manager.h"
#include <3ds.h>
#include <string>

namespace UI {

class CameraScreen : public Screen {
  public:
	CameraScreen(const std::string &channelId);
	~CameraScreen();

	void onEnter() override;
	void update() override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;

  private:
	bool initCamera();
	void deinitCamera();
	bool captureFrame();
	bool saveBmp(const std::string &path);
	void uploadPhoto(const std::string &path);

	std::string channelId;

	bool isCapturing;
	bool isUploading;
	bool captureReady;

	C3D_Tex previewTex;
	bool previewTexReady;
};

} // namespace UI
