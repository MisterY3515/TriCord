#include "ui/update_screen.h"
#include "ui/screen_manager.h"
#include "core/updater.h"
#include "core/i18n.h"
#include "utils/file_utils.h"
#include "utils/string_utils.h"
#include <citro2d.h>

namespace UI {

UpdateScreen::UpdateScreen(const std::string& downloadUrl, const std::string& assetName)
    : downloadUrl(downloadUrl), assetName(assetName), currentBytes(0), totalBytes(0), 
      updateComplete(false), isSuccess(false), updateThread(nullptr) {
	
	auto& i18n = Core::I18n::getInstance();
	statusText = Core::I18n::format(i18n.get("updater.downloading"), assetName);
	
	updateThread = threadCreate([](void* arg) {
		UpdateScreen* screen = static_cast<UpdateScreen*>(arg);
		screen->performUpdate();
	}, this, 16 * 1024, 0x1A, -2, false);
}

UpdateScreen::~UpdateScreen() {
	if (updateThread) {
		threadJoin(updateThread, U64_MAX);
		threadFree(updateThread);
	}
}

void UpdateScreen::update() {
	if (updateComplete && (hidKeysDown() & KEY_B)) {
		ScreenManager::getInstance().pop();
	}
}

void UpdateScreen::renderTop(C3D_RenderTarget *target) {
	C2D_DrawRectSolid(0, 0, 0, 400, 240, C2D_Color32(30, 30, 30, 255));
	
	auto& i18n = Core::I18n::getInstance();
	
	float textY = 100.0f;
	
	std::lock_guard<std::mutex> lock(progressMutex);
	
	C2D_TextBuf buf = C2D_TextBufNew(256);
	C2D_Text text;
	C2D_TextParse(&text, buf, statusText.c_str());
	C2D_TextOptimize(&text);
	C2D_DrawText(&text, C2D_WithColor | C2D_AlignCenter, 200.0f, textY, 0.5f, 0.5f, 0.5f, C2D_Color32(255, 255, 255, 255));
	C2D_TextBufDelete(buf);
	
	textY += 30.0f;
	
	// Draw Progress Bar
	if (totalBytes > 0 && !updateComplete) {
		float progress = static_cast<float>(currentBytes) / static_cast<float>(totalBytes);
		
		// Outline
		C2D_DrawRectSolid(50, textY, 0, 300, 20, C2D_Color32(100, 100, 100, 255));
		// Background
		C2D_DrawRectSolid(52, textY + 2, 0, 296, 16, C2D_Color32(0, 0, 0, 255));
		// Fill
		C2D_DrawRectSolid(52, textY + 2, 0, 296 * progress, 16, C2D_Color32(0, 255, 0, 255));
		
		textY += 30.0f;
		
		char dlNow[32], dlTotal[32];
		snprintf(dlNow, sizeof(dlNow), "%zu KB", currentBytes / 1024);
		snprintf(dlTotal, sizeof(dlTotal), "%zu KB", totalBytes / 1024);
		std::string progressStr = Core::I18n::format(i18n.get("updater.progress"), dlNow, dlTotal);
			
		C2D_TextBuf progBuf = C2D_TextBufNew(256);
		C2D_Text progText;
		C2D_TextParse(&progText, progBuf, progressStr.c_str());
		C2D_TextOptimize(&progText);
		C2D_DrawText(&progText, C2D_WithColor | C2D_AlignCenter, 200.0f, textY, 0.5f, 0.4f, 0.4f, C2D_Color32(200, 200, 200, 255));
		C2D_TextBufDelete(progBuf);
	}
	
	if (updateComplete) {
		textY += 30.0f;
		C2D_TextBuf exitBuf = C2D_TextBufNew(256);
		C2D_Text exitText;
		C2D_TextParse(&exitText, exitBuf, i18n.get("exit.cancel").c_str());
		C2D_TextOptimize(&exitText);
		C2D_DrawText(&exitText, C2D_WithColor | C2D_AlignCenter, 200.0f, 200.0f, 0.5f, 0.5f, 0.5f, C2D_Color32(255, 255, 255, 255));
		C2D_TextBufDelete(exitBuf);
	}
}

void UpdateScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_DrawRectSolid(0, 0, 0, 320, 240, C2D_Color32(30, 30, 30, 255));
}

void UpdateScreen::performUpdate() {
	auto& i18n = Core::I18n::getInstance();
	bool is3dsx = envIsHomebrew();
	
	auto progressCb = [this](size_t dlnow, size_t dltotal) {
		std::lock_guard<std::mutex> lock(progressMutex);
		currentBytes = dlnow;
		totalBytes = dltotal;
	};
	
	if (is3dsx) {
		std::string path = "sdmc:/3ds/TriCord/TriCord.3dsx";
		if (Utils::File::downloadFile(downloadUrl, path, progressCb)) {
			std::lock_guard<std::mutex> lock(progressMutex);
			statusText = i18n.get("updater.complete_3dsx");
			isSuccess = true;
		} else {
			std::lock_guard<std::mutex> lock(progressMutex);
			statusText = i18n.get("updater.failed_download");
		}
	} else {
		std::string tempPath = "sdmc:/3ds/TriCord/update.cia";
		if (Utils::File::downloadFile(downloadUrl, tempPath, progressCb)) {
			{
				std::lock_guard<std::mutex> lock(progressMutex);
				statusText = i18n.get("updater.installing_cia");
			}
			
			if (Updater::getInstance().installCia(tempPath)) {
				remove(tempPath.c_str());
				std::lock_guard<std::mutex> lock(progressMutex);
				statusText = i18n.get("updater.complete_cia");
				isSuccess = true;
			} else {
				std::lock_guard<std::mutex> lock(progressMutex);
				statusText = i18n.get("updater.failed_cia");
			}
		} else {
			std::lock_guard<std::mutex> lock(progressMutex);
			statusText = i18n.get("updater.failed_download");
		}
	}
	
	std::lock_guard<std::mutex> lock(progressMutex);
	updateComplete = true;
}

} // namespace UI
