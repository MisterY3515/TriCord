#pragma once

#include "ui/screen_manager.h"
#include <string>
#include <vector>

namespace UI {

class FileBrowserScreen : public Screen {
  public:
	FileBrowserScreen(const std::string &channelId);

	void onEnter() override;
	void update() override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;

  private:
	void loadDirectory(const std::string &path);
	void uploadSelectedFile();

	std::string currentPath;
	std::string channelId;

	struct FileEntry {
		std::string name;
		bool isDir;
		size_t size;
	};

	std::vector<FileEntry> entries;
	int selectedIndex;
	float scrollY;
	float maxScroll;
	bool isUploading;
};

} // namespace UI
