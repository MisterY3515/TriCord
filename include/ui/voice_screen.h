#ifndef VOICE_SCREEN_H
#define VOICE_SCREEN_H

#include "screen_manager.h"
#include <string>
#include <vector>

namespace UI {

class VoiceScreen : public Screen {
  public:
	VoiceScreen();
	~VoiceScreen() override = default;

	void update() override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;
	void onEnter() override;
	bool hidesMenu() const override { return true; }

  private:
	struct VoiceUser {
		std::string userId;
		std::string displayName;
		std::string avatarHash;
		bool isSpeaking;
		bool isMuted;
		bool isDeafened;
	};

	// Context menu items
	enum class ContextAction {
		NONE,
		MUTE_TOGGLE,
		LEAVE_CALL,
		CANCEL
	};

	std::vector<VoiceUser> voiceUsers;
	int selectedUserIndex;
	bool contextMenuOpen;
	int contextMenuSelection;
	int holdTimer;

	// Refresh user list
	void refreshUserList();

	static constexpr int HOLD_THRESHOLD = 30; // ~0.5s at 60fps
	static constexpr int MAX_CONTEXT_ITEMS = 3;
};

} // namespace UI

#endif // VOICE_SCREEN_H
