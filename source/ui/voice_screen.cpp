#include "ui/voice_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "discord/voice_client.h"
#include "ui/screen_manager.h"
#include "utils/utf8_utils.h"
#include <3ds.h>
#include <cmath>

namespace UI {

VoiceScreen::VoiceScreen()
    : selectedUserIndex(-1), contextMenuOpen(false), contextMenuSelection(0), holdTimer(0) {}

void VoiceScreen::onEnter() {
	selectedUserIndex = -1;
	contextMenuOpen = false;
	contextMenuSelection = 0;
	holdTimer = 0;
	refreshUserList();
}

void VoiceScreen::refreshUserList() {
	voiceUsers.clear();

	auto &vc = Discord::VoiceClient::getInstance();
	if (!vc.isInChannel()) return;

	auto &dc = Discord::DiscordClient::getInstance();
	std::string guildId = vc.getGuildId();
	std::string channelId = vc.getChannelId();

	auto userIds = dc.getUsersInVoiceChannel(channelId);

	for (const auto &uid : userIds) {
		VoiceUser vu;
		vu.userId = uid;
		vu.isSpeaking = vc.isUserSpeaking(uid);
		vu.isMuted = false;
		vu.isDeafened = false;

		// Get display name from guild member nickname
		Discord::Member member = dc.getMember(guildId, uid);
		if (!member.nickname.empty()) {
			vu.displayName = member.nickname;
		} else {
			// Fallback to user_id (we don't have full User data in member list)
			vu.displayName = uid.substr(0, 8) + "...";
		}

		// We don't have avatar hash in the Member struct, leave empty for fallback
		vu.avatarHash = "";

		// Check voice state (mute/deaf)
		{
			std::lock_guard<std::recursive_mutex> lock(dc.getMutex());
			for (const auto &guild : dc.getGuilds()) {
				if (guild.id == guildId) {
					for (const auto &vs : guild.voiceStates) {
						if (vs.user_id == uid) {
							vu.isMuted = vs.self_mute || vs.mute;
							vu.isDeafened = vs.self_deaf || vs.deaf;
							break;
						}
					}
					break;
				}
			}
		}

		voiceUsers.push_back(vu);
	}
}

void VoiceScreen::update() {
	u32 kDown = hidKeysDown();
	u32 kHeld = hidKeysHeld();

	// Refresh the user list periodically (every ~2s = 120 frames)
	static int refreshCounter = 0;
	refreshCounter++;
	if (refreshCounter >= 120) {
		refreshUserList();
		refreshCounter = 0;
	}

	// If not in a call anymore, go back
	if (!Discord::VoiceClient::getInstance().isInChannel()) {
		ScreenManager::getInstance().returnToPreviousScreen();
		return;
	}

	if (contextMenuOpen) {
		// Navigate context menu on Top Screen
		if (kDown & KEY_UP) {
			contextMenuSelection--;
			if (contextMenuSelection < 0) contextMenuSelection = MAX_CONTEXT_ITEMS - 1;
		}
		if (kDown & KEY_DOWN) {
			contextMenuSelection++;
			if (contextMenuSelection >= MAX_CONTEXT_ITEMS) contextMenuSelection = 0;
		}
		if (kDown & KEY_B) {
			contextMenuOpen = false;
		}
		if (kDown & KEY_A) {
			// Execute action
			switch (contextMenuSelection) {
			case 0: // Mute/Unmute self
			{
				auto &vc = Discord::VoiceClient::getInstance();
				vc.setMuted(!vc.isMuted());
				ScreenManager::getInstance().showToast(
				    vc.isMuted() ? TR("common.muted") : TR("common.unmuted"));
			} break;
			case 1: // Leave call
				Discord::VoiceClient::getInstance().leaveChannel();
				ScreenManager::getInstance().showToast("Left voice channel");
				return;
			case 2: // Cancel
				break;
			}
			contextMenuOpen = false;
		}
	} else {
		// Navigate user list on bottom screen (D-Pad)
		if (kDown & KEY_DOWN) {
			selectedUserIndex++;
			if (selectedUserIndex >= (int)voiceUsers.size()) selectedUserIndex = 0;
		}
		if (kDown & KEY_UP) {
			selectedUserIndex--;
			if (selectedUserIndex < 0) selectedUserIndex = (int)voiceUsers.size() - 1;
		}
		if (kDown & KEY_B) {
			ScreenManager::getInstance().returnToPreviousScreen();
			return;
		}
		
		if (kDown & KEY_X) {
			Discord::VoiceClient::getInstance().leaveChannel();
			ScreenManager::getInstance().showToast("Left voice channel");
			// Non serve returnToPreviousScreen() perché isInChannel() ora è false
			// e ScreenManager tornerà indietro automaticamente nel check all'inizio di update()
			return;
		}

		if (kDown & KEY_Y) {
			auto &vc = Discord::VoiceClient::getInstance();
			vc.setMuted(!vc.isMuted());
			ScreenManager::getInstance().showToast(
			    vc.isMuted() ? TR("common.muted") : TR("common.unmuted"));
		}

		// Touch input - long press detection
		touchPosition touch;
		hidTouchRead(&touch);
		if (kHeld & KEY_TOUCH) {
			holdTimer++;
			if (holdTimer >= HOLD_THRESHOLD) {
				// Detect which user was tapped (if any)
				float startY = 40.0f;
				float itemH = 42.0f;
				int maxVisible = 4;
				for (int i = 0; i < (int)voiceUsers.size() && i < maxVisible; i++) {
					float y = startY + i * itemH;
					if (touch.py >= y && touch.py < y + itemH) {
						selectedUserIndex = i;
						contextMenuOpen = true;
						contextMenuSelection = 0;
						holdTimer = 0;
						break;
					}
				}
			}
		} else {
			// Simple tap to select
			if ((kDown & KEY_TOUCH) && !voiceUsers.empty()) {
				float startY = 40.0f;
				float itemH = 42.0f;
				int maxVisible = 4;
				for (int i = 0; i < (int)voiceUsers.size() && i < maxVisible; i++) {
					float y = startY + i * itemH;
					if (touch.py >= y && touch.py < y + itemH) {
						selectedUserIndex = i;
						break;
					}
				}
			}
			holdTimer = 0;
		}

		// A button to open context menu on selected user
		if ((kDown & KEY_A) && selectedUserIndex >= 0 && selectedUserIndex < (int)voiceUsers.size()) {
			contextMenuOpen = true;
			contextMenuSelection = 0;
		}
	}
}

void VoiceScreen::renderTop(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackground());

	float headerH = 26.0f;
	C2D_DrawRectSolid(0, 0, 0.9f, 400.0f, headerH, ScreenManager::colorHeaderGlass());
	C2D_DrawRectSolid(0, headerH - 1.0f, 0.91f, 400.0f, 1.0f, ScreenManager::colorHeaderBorder());
	drawText(10.0f, 5.0f, 0.92f, 0.55f, 0.55f, ScreenManager::colorText(), "\uE046 Voice Chat");

	if (contextMenuOpen && selectedUserIndex >= 0 && selectedUserIndex < (int)voiceUsers.size()) {
		// Draw context menu for the selected user
		const auto &user = voiceUsers[selectedUserIndex];

		float popupX = 60.0f;
		float popupY = 50.0f;
		float popupW = 280.0f;
		float popupH = 140.0f;

		drawPopupBackground(popupX, popupY, popupW, popupH, 0.8f);

		// User name
		drawText(popupX + 15.0f, popupY + 10.0f, 0.85f, 0.55f, 0.55f, ScreenManager::colorAccent(),
		         user.displayName);

		// Menu items
		float itemY = popupY + 35.0f;
		float itemH = 30.0f;

		auto &vc = Discord::VoiceClient::getInstance();

		const char *labels[MAX_CONTEXT_ITEMS] = {
		    vc.isMuted() ? "Unmute" : "Mute",
		    "Leave Call",
		    "Cancel"};

		for (int i = 0; i < MAX_CONTEXT_ITEMS; i++) {
			bool selected = (i == contextMenuSelection);
			u32 bgColor = selected ? ScreenManager::colorSelection() : C2D_Color32(0, 0, 0, 0);
			C2D_DrawRectSolid(popupX + 10.0f, itemY, 0.85f, popupW - 20.0f, itemH - 2.0f, bgColor);

			u32 textColor = (i == 1) ? ScreenManager::colorError() : ScreenManager::colorText();
			if (selected) textColor = ScreenManager::colorWhite();

			drawText(popupX + 20.0f, itemY + 6.0f, 0.86f, 0.5f, 0.5f, textColor, labels[i]);
			itemY += itemH;
		}

		// Hint at bottom
		drawText(popupX + 15.0f, popupY + popupH - 22.0f, 0.86f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
		         "\uE079\uE07A: Navigate  \uE000: Confirm  \uE001: Cancel");
	} else {
		// Draw channel info
		auto &vc = Discord::VoiceClient::getInstance();
		if (vc.isInChannel()) {
			std::string statusText = vc.isConnected() ? "Connected" : "Connecting...";
			drawText(10.0f, 35.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorSuccess(), statusText);

			std::string countStr = std::to_string(voiceUsers.size()) + " user(s) in channel";
			drawText(10.0f, 55.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(), countStr);

			// Hint
			drawText(10.0f, 200.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
			         "Hold touch or press \uE000 on a user for options");
		}
	}
}

void VoiceScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackground());

	auto &vc = Discord::VoiceClient::getInstance();
	if (!vc.isInChannel()) {
		drawCenteredText(100.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(), TR("voice.no_users"), 320.0f);
		return;
	}

	// Bottom Bar
	float barH = 22.0f;
	float barY = BOTTOM_SCREEN_HEIGHT - barH;
	
	// Mute/Unmute (X)
	u32 muteColor = vc.isMuted() ? C2D_Color32(200, 60, 60, 255) : ScreenManager::colorAccent();
	C2D_DrawRectSolid(0, barY, 0.9f, 160, barH, muteColor);
	std::string muteStr = vc.isMuted() ? "\uE002 " + TR("voice.unmuted") : "\uE002 " + TR("voice.muted");
	float mw = measureText(muteStr, 0.45f, 0.45f);
	drawText((160.0f - mw) / 2.0f, barY + 3.0f, 0.95f, 0.45f, 0.45f, ScreenManager::colorWhite(), muteStr);

	// Leave (Y)
	C2D_DrawRectSolid(160, barY, 0.9f, 160, barH, C2D_Color32(200, 60, 60, 255));
	std::string leaveStr = "\uE003 " + TR("voice.leave");
	float lw = measureText(leaveStr, 0.45f, 0.45f);
	drawText(160.0f + (160.0f - lw) / 2.0f, barY + 3.0f, 0.95f, 0.45f, 0.45f, ScreenManager::colorWhite(), leaveStr);

	// Header
	float headerH = 30.0f;
	C2D_DrawRectSolid(0, 0, 0.9f, 320.0f, headerH, ScreenManager::colorHeaderGlass());
	C2D_DrawRectSolid(0, headerH - 1.0f, 0.91f, 320.0f, 1.0f, ScreenManager::colorHeaderBorder());
	drawText(10.0f, 7.0f, 0.92f, 0.5f, 0.5f, ScreenManager::colorText(), "\uE046 Voice Participants");

	if (voiceUsers.empty()) {
		drawCenteredText(100.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(), "No users in channel", 320.0f);
		return;
	}

	float startY = 40.0f;
	float itemH = 42.0f;
	int maxVisible = 4; // O3DS safe: max 4 users visible at a time

	for (int i = 0; i < (int)voiceUsers.size() && i < maxVisible; i++) {
		const auto &user = voiceUsers[i];
		float y = startY + i * itemH;
		bool isSelected = (i == selectedUserIndex);

		// Selection highlight
		if (isSelected) {
			C2D_DrawRectSolid(0, y, 0.5f, 320.0f, itemH - 2.0f, ScreenManager::colorSelection());
		}

		// Speaking indicator (green circle around avatar area)
		float avatarX = 12.0f;
		float avatarY = y + 5.0f;
		float avatarSize = 30.0f;

		if (user.isSpeaking) {
			drawCircle(avatarX + avatarSize / 2.0f, avatarY + avatarSize / 2.0f, 0.6f,
			           avatarSize / 2.0f + 3.0f, C2D_Color32(87, 242, 135, 200));
		}

		// Avatar via AvatarCache
		C3D_Tex *tex = nullptr;
		if (!user.avatarHash.empty()) {
			tex = Discord::AvatarCache::getInstance().getAvatar(user.userId, user.avatarHash, "");
		}

		if (tex) {
			Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
			C2D_Image img = {tex, &subtex};
			C2D_DrawImageAt(img, avatarX, avatarY, 0.75f, nullptr, avatarSize / tex->width, avatarSize / tex->height);
		} else {
			// Fallback: colored rect with initial
			u32 avatarColor = isSelected ? ScreenManager::colorAccent() : ScreenManager::colorBackgroundLight();
			drawRoundedRect(avatarX, avatarY, 0.7f, avatarSize, avatarSize, 4.0f, avatarColor);
			std::string initial = Utils::Utf8::getFirstChar(user.displayName.empty() ? "?" : user.displayName);
			drawText(avatarX + 9.0f, avatarY + 6.0f, 0.8f, 0.5f, 0.5f, ScreenManager::colorWhite(), initial);
		}

		// Username
		float textX = avatarX + avatarSize + 10.0f;
		drawText(textX, y + 8.0f, 0.7f, 0.5f, 0.5f, ScreenManager::colorText(), user.displayName);

		// Status icons
		float statusX = 280.0f;
		if (user.isDeafened) {
			drawText(statusX, y + 12.0f, 0.7f, 0.4f, 0.4f, ScreenManager::colorError(), "D");
			statusX -= 18.0f;
		}
		if (user.isMuted) {
			drawText(statusX, y + 12.0f, 0.7f, 0.4f, 0.4f, ScreenManager::colorError(), "M");
		}

		// Separator
		C2D_DrawRectSolid(10.0f, y + itemH - 2.0f, 0.5f, 300.0f, 1.0f, ScreenManager::colorSeparator());
	}

	// Bottom hint bar
	drawText(10.0f, 220 - 18.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
	         "\uE001: Back  \uE002: Leave  \uE003: Mute/Unmute");
}

} // namespace UI
