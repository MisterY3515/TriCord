#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#include "ui/screen_manager.h"
#include <functional>
#include <string>
#include <vector>

namespace UI {

enum class SettingItemType { INTEGER, STEPPER, TOGGLE, ACTION, SECTION_HEADER };

struct SettingItem {
	std::string label;
	std::string description;
	SettingItemType type;
	int value;
	int min;
	int max;
	std::function<std::string(int)> valueFormatter;
	std::function<void(int)> onUpdate;
	std::function<void()> action;
	bool isDeveloper = false;
};

class SettingsScreen : public Screen {
  public:
	SettingsScreen();
	virtual ~SettingsScreen();

	void onEnter() override;
	void onExit() override;
	void update() override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;
	void saveAndExit();

  private:
	std::vector<SettingItem> allItems;
	std::vector<SettingItem *> visibleItems;

	int selectedIndex;
	float scrollOffset;
	int repeatTimer;
	u32 lastKey;

	std::string searchQuery;
	bool isDeveloperMode;

	bool popupActive;
	int popupSelectedIndex;
	SettingItem *activePopupItem;
	float popupScrollOffset;

	bool scheduleRefresh = false;

	void refreshVisibleItems();
};

} // namespace UI

#endif // SETTINGS_SCREEN_H
