#ifndef THEME_MANAGER_SCREEN_H
#define THEME_MANAGER_SCREEN_H

#include "ui/screen_manager.h"
#include <string>
#include <thread>
#include <vector>

namespace UI {

class ThemeManagerScreen : public Screen {
  public:
	ThemeManagerScreen();
	virtual ~ThemeManagerScreen();

	void onEnter() override;
	void onExit() override;
	void update() override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;

  private:
	struct ThemeItem {
		std::string filename;
		std::string displayName;
		std::string author;
		std::string description;
		std::string updateUrl;
	};
	std::vector<ThemeItem> themes;
	int selectedIndex;
	float scrollOffset;
	int repeatTimer = 0;
	u32 lastKey = 0;

	bool isUpdating = false;
	bool shouldRefresh = false;
	std::string themeToReload = "";
	std::thread downloadThread;
	std::string updateStatus;

	enum class DeleteState { NONE, CONFIRMING };
	DeleteState deleteState = DeleteState::NONE;
	int themeToDeleteIndex = -1;

	void refreshThemes();
	void downloadTheme(const std::string &url, const std::string &filename);
};

} // namespace UI

#endif // THEME_MANAGER_SCREEN_H
