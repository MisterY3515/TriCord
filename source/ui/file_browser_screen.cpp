#include "ui/file_browser_screen.h"
#include "ui/screen_manager.h"
#include "core/i18n.h"
#include "discord/discord_client.h"
#include "log.h"
#include <3ds.h>
#include <dirent.h>
#include <algorithm>
#include <sys/stat.h>

namespace UI {

FileBrowserScreen::FileBrowserScreen(const std::string &channelId)
    : channelId(channelId), currentPath("sdmc:/"), selectedIndex(0), scrollY(0.0f), maxScroll(0.0f), isUploading(false) {}

void FileBrowserScreen::onEnter() {
	loadDirectory(currentPath);
}

void FileBrowserScreen::loadDirectory(const std::string &path) {
	entries.clear();
	currentPath = path;
	selectedIndex = 0;
	scrollY = 0.0f;

	if (currentPath != "sdmc:/" && currentPath != "sdmc:") {
		entries.push_back({"..", true, 0});
	}

	DIR *dir = opendir(path.c_str());
	if (dir) {
		struct dirent *ent;
		while ((ent = readdir(dir)) != nullptr) {
			std::string name = ent->d_name;
			if (name == "." || name == "..") continue;

			std::string fullPath = path;
			if (fullPath.back() != '/') fullPath += '/';
			fullPath += name;
			struct stat st;
			bool isDir = false;
			size_t size = 0;
			
			if (stat(fullPath.c_str(), &st) == 0) {
				isDir = S_ISDIR(st.st_mode);
				size = st.st_size;
			}
			entries.push_back({name, isDir, size});
		}
		closedir(dir);
	}

	std::sort(entries.begin() + (entries.empty() || entries[0].name != ".." ? 0 : 1), entries.end(),
	          [](const FileEntry &a, const FileEntry &b) {
		          if (a.isDir != b.isDir) return a.isDir > b.isDir;
		          return a.name < b.name;
	          });

	maxScroll = std::max(0.0f, (float)entries.size() * 25.0f - 200.0f);
}

void FileBrowserScreen::update() {
	if (isUploading) return;

	u32 kDown = hidKeysDown();

	if (kDown & KEY_B) {
		ScreenManager::getInstance().returnToPreviousScreen();
		return;
	}

	if (!entries.empty()) {
		if (kDown & KEY_DOWN) {
			selectedIndex++;
			if (selectedIndex >= (int)entries.size()) selectedIndex = entries.size() - 1;
		}
		if (kDown & KEY_UP) {
			selectedIndex--;
			if (selectedIndex < 0) selectedIndex = 0;
		}

		if (kDown & KEY_A) {
			const auto &entry = entries[selectedIndex];
			if (entry.isDir) {
				if (entry.name == "..") {
					size_t slash = currentPath.find_last_of('/');
					if (slash != std::string::npos && slash > 4) {
						loadDirectory(currentPath.substr(0, slash));
					} else {
						loadDirectory("sdmc:/");
					}
				} else {
					std::string nextPath = currentPath;
					if (nextPath.back() != '/') nextPath += "/";
					nextPath += entry.name;
					loadDirectory(nextPath);
				}
			} else {
				uploadSelectedFile();
			}
		}
	}

	float itemY = selectedIndex * 25.0f;
	if (itemY < scrollY) scrollY = itemY;
	if (itemY + 25.0f > scrollY + 200.0f) scrollY = itemY + 25.0f - 200.0f;
}

void FileBrowserScreen::uploadSelectedFile() {
	if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;
	const auto &entry = entries[selectedIndex];
	if (entry.isDir) return;

	std::string fullPath = currentPath;
	if (fullPath.back() != '/') fullPath += "/";
	fullPath += entry.name;

	isUploading = true;
	ScreenManager::getInstance().showToast("Uploading " + entry.name + "...");

	Discord::DiscordClient::getInstance().uploadFile(
	    channelId, fullPath, "Sent via TriCord",
	    [this](const Discord::Message &msg, bool success, int errorCode) {
		    isUploading = false;
		    if (success) {
			    ScreenManager::getInstance().runOnMainThread([]() {
				    ScreenManager::getInstance().showToast("Upload complete!");
				    ScreenManager::getInstance().returnToPreviousScreen();
			    });
		    } else {
			    std::string errMsg = "Upload failed: " + std::to_string(errorCode);
			    ScreenManager::getInstance().runOnMainThread([errMsg]() {
				    ScreenManager::getInstance().showToast(errMsg);
			    });
		    }
	    });
}

void FileBrowserScreen::renderTop(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackground());

	drawText(10.0f, 10.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorText(), "File Browser");
	drawText(10.0f, 35.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), currentPath);

	if (isUploading) {
		drawCenteredText(120.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorAccent(), "Uploading...", 400.0f);
	}
}

void FileBrowserScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

	float y = 10.0f - scrollY;

	for (size_t i = 0; i < entries.size(); i++) {
		if (y > 240.0f || y < -25.0f) {
			y += 25.0f;
			continue;
		}

		const auto &entry = entries[i];
		u32 color = ScreenManager::colorText();
		if ((int)i == selectedIndex) {
			C2D_DrawRectSolid(5.0f, y, 0.5f, 310.0f, 22.0f, ScreenManager::colorSelection());
			color = ScreenManager::colorWhite();
		}

		std::string icon = entry.isDir ? "\uE074" : "\uE073"; // Folder / File
		drawText(10.0f, y + 2.0f, 0.6f, 0.45f, 0.45f, color, icon + " " + entry.name);

		if (!entry.isDir && entry.size > 0) {
			std::string sizeStr = std::to_string(entry.size / 1024) + " KB";
			drawText(260.0f, y + 4.0f, 0.6f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), sizeStr);
		}

		y += 25.0f;
	}

	drawText(10.0f, 220.0f, 0.9f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), "\uE000: Open  \uE001: Back");
}

} // namespace UI
