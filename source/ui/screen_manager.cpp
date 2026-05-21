#include "ui/screen_manager.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "discord/voice_client.h"
#include "log.h"
#include "ui/about_screen.h"
#include "ui/disclaimer_screen.h"
#include "ui/dm_screen.h"
#include "ui/emoji_manager.h"
#include "ui/forum_screen.h"
#include "ui/image_manager.h"
#include "ui/login_screen.h"
#include "ui/message_screen.h"
#include "ui/server_list_screen.h"
#include "ui/settings_screen.h"
#include "ui/text_measure_cache.h"
#include "ui/theme_manager_screen.h"
#include "ui/voice_screen.h"
#include "ui/modal_screen.h"
#include "utils/message_utils.h"
#include "utils/utf8_utils.h"
#include <cmath>

namespace UI {

static C2D_TextBuf textBuf = nullptr;
static C2D_TextBuf debugTextBuf = nullptr;
static C2D_TextBuf layoutTextBuf = nullptr;

Screen::Screen() : exitRequested(false) {}

ScreenManager &ScreenManager::getInstance() {
	static ScreenManager instance;
	return instance;
}

void ScreenManager::init() {
	Config::getInstance().loadTheme();
	topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	if (!textBuf) {
		textBuf = C2D_TextBufNew(32768);
	}
	if (!debugTextBuf) {
		debugTextBuf = C2D_TextBufNew(16384);
	}
	if (!layoutTextBuf) {
		layoutTextBuf = C2D_TextBufNew(32768);
	}

	debugOverlayEnabled = false;

	Logger::log("[UI] Screen manager initialized");

	if (!Config::getInstance().isDisclaimerAccepted()) {
		setScreen(ScreenType::DISCLAIMER);
	} else if (Discord::DiscordClient::getInstance().getState() == Discord::ConnectionState::READY) {
		setScreen(ScreenType::GUILD_LIST);
	} else {
		setScreen(ScreenType::LOGIN);
	}
}

void ScreenManager::shutdown() {
	if (currentScreen) {
		currentScreen->onExit();
		currentScreen.reset();
	}

	if (textBuf) {
		C2D_TextBufDelete(textBuf);
		textBuf = nullptr;
	}
	if (debugTextBuf) {
		C2D_TextBufDelete(debugTextBuf);
		debugTextBuf = nullptr;
	}
	if (layoutTextBuf) {
		C2D_TextBufDelete(layoutTextBuf);
		layoutTextBuf = nullptr;
	}

	Logger::log("[UI] Screen manager shutdown");
}

void ScreenManager::setScreen(ScreenType type) {
	if (currentScreen) {
		currentScreen->onExit();
	}

	if (type == ScreenType::LOGIN || type == ScreenType::GUILD_LIST || type == ScreenType::ADD_ACCOUNT ||
	    type == ScreenType::DM_LIST || type == ScreenType::DISCLAIMER) {
		screenHistory.clear();
	}

	currentType = type;

	if (type == ScreenType::LOGIN || type == ScreenType::ADD_ACCOUNT || type == ScreenType::DISCLAIMER) {
		hamburgerMenu.reset();
	}

	switch (type) {
	case ScreenType::LOGIN:
		expandedFolders.clear();

		currentScreen = std::make_unique<LoginScreen>();
		break;
	case ScreenType::GUILD_LIST:
		currentScreen = std::make_unique<ServerListScreen>();
		break;
	case ScreenType::MESSAGES: {
		auto &client = Discord::DiscordClient::getInstance();
		std::string channelId = client.getSelectedChannelId();
		std::string channelName = TR("common.channel");
		for (const auto &g : client.getGuilds()) {
			for (const auto &ch : g.channels) {
				if (ch.id == channelId) {
					channelName = ch.name;
					goto found;
				}
			}
		}
		for (const auto &ch : client.getPrivateChannels()) {
			if (ch.id == channelId) {
				channelName = ch.name;
				if (channelName.empty() && ch.type == 1 && !ch.recipients.empty()) {
					channelName = ch.recipients[0].global_name;
					if (channelName.empty()) {
						channelName = ch.recipients[0].username;
					}
				}
				break;
			}
		}
	found:
		currentScreen = std::make_unique<MessageScreen>(channelId, channelName);
		break;
	}
	case ScreenType::ADD_ACCOUNT:
		currentScreen = std::make_unique<LoginScreen>();
		break;
	case ScreenType::FORUM_CHANNEL: {
		auto &client = Discord::DiscordClient::getInstance();
		std::string channelId = client.getSelectedChannelId();
		std::string channelName = TR("common.forum");
		for (const auto &g : client.getGuilds()) {
			for (const auto &ch : g.channels) {
				if (ch.id == channelId) {
					channelName = ch.name;
					break;
				}
			}
		}
		currentScreen = std::make_unique<ForumScreen>(channelId, channelName);
		break;
	}
	case ScreenType::SETTINGS:
		currentScreen = std::make_unique<SettingsScreen>();
		break;
	case ScreenType::DM_LIST:
		currentScreen = std::make_unique<DmScreen>();
		break;
	case ScreenType::ABOUT:
		currentScreen = std::make_unique<AboutScreen>();
		break;
	case ScreenType::DISCLAIMER:
		currentScreen = std::make_unique<DisclaimerScreen>();
		break;
	case ScreenType::THEME_MANAGER:
		currentScreen = std::make_unique<ThemeManagerScreen>();
		break;
	case ScreenType::VOICE_CALL:
		currentScreen = std::make_unique<VoiceScreen>();
		break;
	}

	if (currentScreen) {
		currentScreen->onEnter();
	}
}

void ScreenManager::pushScreen(ScreenType type) {
	if (currentType != type) {
		screenHistory.push_back(currentType);
	}
	setScreen(type);
}

void ScreenManager::returnToPreviousScreen() {
	if (screenHistory.empty()) {
		if (currentType != ScreenType::GUILD_LIST && currentType != ScreenType::LOGIN) {
			setScreen(ScreenType::GUILD_LIST);
		}
		return;
	}

	ScreenType prev = screenHistory.back();
	screenHistory.pop_back();

	setScreen(prev);
}

void ScreenManager::pushCustomScreen(std::unique_ptr<Screen> screen) {
	if (currentScreen) {
		currentScreen->onExit();
	}
	screenHistory.push_back(currentType);
	currentScreen = std::move(screen);
	if (currentScreen) {
		currentScreen->onEnter();
	}
}

void ScreenManager::pop() {
	returnToPreviousScreen();
}

void ScreenManager::showModal(const std::string& title, const std::string& desc,
                              const std::vector<std::string>& buttons, std::function<void(int)> onButton) {
	pushCustomScreen(std::make_unique<ModalScreen>(title, desc, buttons, onButton));
}

void ScreenManager::update() {
	ImageManager::getInstance().update();
	EmojiManager::getInstance().update();
	Discord::AvatarCache::getInstance().update();

	hamburgerMenu.update();

	if (layoutTextBuf) {
		C2D_TextBufClear(layoutTextBuf);
	}

	u32 kDown = hidKeysDown();
	u32 kHeld = hidKeysHeld();

	if ((kHeld & KEY_SELECT) && (kHeld & KEY_START) && (kDown & KEY_B)) {
		appExitRequested = true;
		return;
	}

	if ((kDown & KEY_START) && Discord::VoiceClient::getInstance().isInChannel()) {
		static u64 lastStartPress = 0;
		u64 now = osGetTime();
		if (now - lastStartPress > 500) { // 500ms debounce
			lastStartPress = now;
			if (currentType == ScreenType::VOICE_CALL) {
				returnToPreviousScreen();
			} else {
				pushScreen(ScreenType::VOICE_CALL);
			}
		}
	}

	bool shouldBlockScreen = !hamburgerMenu.isClosed();

	if (!isMenuHidden()) {
		touchPosition touch;
		hidTouchRead(&touch);
		if (kDown & KEY_TOUCH) {
			if (touch.px < 40 && touch.py < 40) {
				if (shouldShowBackArrow()) {
					UI::SettingsScreen *ss = (UI::SettingsScreen *)currentScreen.get();
					ss->saveAndExit();
				} else {
					hamburgerMenu.toggle();
				}
				shouldBlockScreen = true;
			}
		} else if (kDown & KEY_SELECT) {
			hamburgerMenu.toggle();
			shouldBlockScreen = !hamburgerMenu.isClosed();
		}
	}

	if (!shouldBlockScreen) {
		if (currentScreen) {
			currentScreen->update();
		}
	}

	if ((kHeld & KEY_L) && (kDown & KEY_R)) {
		toggleDebugOverlay();
		Logger::log("Debug overlay toggled: %s", debugOverlayEnabled ? "ON" : "OFF");
	} else if ((kHeld & KEY_L) && (kDown & KEY_B)) {
		auto &vc = Discord::VoiceClient::getInstance();
		if (vc.isInChannel()) {
			vc.leaveChannel();
			showToast("Left voice channel");
		}
	} else if (kDown & KEY_X) { // Changed back to KEY_X as requested
		auto &vc = Discord::VoiceClient::getInstance();
		if (vc.isInChannel() && currentType != ScreenType::VOICE_CALL) {
			vc.setMuted(!vc.isMuted());
			showToast(vc.isMuted() ? TR("common.muted") : TR("common.unmuted"));
		}
	}

	// Touch controls for Voice Overlay - Only if not in Voice Screen
	if ((kDown & KEY_TOUCH) && Discord::VoiceClient::getInstance().isInChannel() && currentType != ScreenType::VOICE_CALL) {
		touchPosition touch;
		hidTouchRead(&touch);
		if (touch.py >= BOTTOM_SCREEN_HEIGHT - 22.0f) {
			if (touch.px < BOTTOM_SCREEN_WIDTH / 2) {
				auto &vc = Discord::VoiceClient::getInstance();
				vc.setMuted(!vc.isMuted());
				showToast(vc.isMuted() ? TR("common.muted") : TR("common.unmuted"));
			} else {
				auto &vc = Discord::VoiceClient::getInstance();
				vc.leaveChannel();
				showToast("Left voice channel");
			}
			// Block touch from reaching the current screen
			return;
		}
	}

	if (toastTimer > 0) {
		toastTimer--;
	}
}

void ScreenManager::render() {
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

	if (textBuf) {
		C2D_TextBufClear(textBuf);
	}
	if (debugTextBuf) {
		C2D_TextBufClear(debugTextBuf);
	}

	C2D_TargetClear(topTarget, colorBackground());
	C2D_SceneBegin(topTarget);

	if (currentScreen) {
		currentScreen->renderTop(topTarget);
	}

	if (!isMenuHidden()) {
		hamburgerMenu.render();
	}

	if (debugOverlayEnabled) {
		renderDebugOverlay();
	}

	C2D_TargetClear(bottomTarget, colorBackground());
	C2D_SceneBegin(bottomTarget);

	if (currentScreen) {
		currentScreen->renderBottom(bottomTarget);
	}

	if (!isMenuHidden()) {
		drawHamburgerButton();
	}

	renderConnectionIndicator();

	if (toastTimer > 0) {
		drawToast();
	}

	if (Discord::VoiceClient::getInstance().isInChannel() && currentType != ScreenType::VOICE_CALL) {
		renderVoiceOverlay();
	}

	C3D_FrameEnd(0);
}

void ScreenManager::renderVoiceOverlay() {
	auto &vc = Discord::VoiceClient::getInstance();
	
	// Draw a persistent bar at the very bottom
	float barH = 22.0f;
	float barY = BOTTOM_SCREEN_HEIGHT - barH;
	
	// Split in two buttons
	u32 muteColor = vc.isMuted() ? C2D_Color32(200, 60, 60, 255) : colorAccent();
	u32 leaveColor = C2D_Color32(200, 60, 60, 255);
	
	float halfW = BOTTOM_SCREEN_WIDTH / 2.0f;
	
	// Mute Button (Left)
	C2D_DrawRectSolid(0.0f, barY, 0.9f, halfW, barH, muteColor);
	C2D_DrawRectSolid(0.0f, barY, 0.91f, halfW, 1.0f, C2D_Color32(255, 255, 255, 100)); // highlight
	
	std::string muteStr = vc.isMuted() ? "\uE002 Unmute" : "\uE002 Mute"; // \uE002 is X
	C2D_Text mText;
	C2D_TextParse(&mText, textBuf, muteStr.c_str());
	C2D_TextOptimize(&mText);
	float mw, mh;
	C2D_TextGetDimensions(&mText, 0.45f, 0.45f, &mw, &mh);
	C2D_DrawText(&mText, C2D_WithColor, (halfW - mw) / 2.0f, barY + 3.0f, 0.95f, 0.45f, 0.45f, C2D_Color32(255, 255, 255, 255));
	
	// Leave Button (Right)
	C2D_DrawRectSolid(halfW, barY, 0.9f, halfW, barH, leaveColor);
	C2D_DrawRectSolid(halfW, barY, 0.91f, halfW, 1.0f, C2D_Color32(255, 255, 255, 100)); // highlight
	
	std::string leaveStr = "\uE004+\uE001 Leave Call"; // \uE004 is L, \uE001 is B
	C2D_Text lText;
	C2D_TextParse(&lText, textBuf, leaveStr.c_str());
	C2D_TextOptimize(&lText);
	float lw, lh;
	C2D_TextGetDimensions(&lText, 0.45f, 0.45f, &lw, &lh);
	C2D_DrawText(&lText, C2D_WithColor, halfW + (halfW - lw) / 2.0f, barY + 3.0f, 0.95f, 0.45f, 0.45f, C2D_Color32(255, 255, 255, 255));
	
	// Separator
	C2D_DrawRectSolid(halfW, barY, 0.92f, 1.0f, barH, C2D_Color32(0, 0, 0, 100));
}

void ScreenManager::renderConnectionIndicator() {
	auto state = Discord::DiscordClient::getInstance().getState();
	
	bool isConnected = (state == Discord::ConnectionState::READY);
	bool isConnecting = (state == Discord::ConnectionState::CONNECTING || 
	                     state == Discord::ConnectionState::CONNECTED_WS || 
	                     state == Discord::ConnectionState::IDENTIFYING || 
	                     state == Discord::ConnectionState::AUTHENTICATING ||
	                     state == Discord::ConnectionState::RECONNECTING);
	bool isError = (state == Discord::ConnectionState::DISCONNECTED_ERROR);
	
	// Draw connection bars on top-right of bottom screen
	float startX = 297.0f;
	float startY = 20.0f;
	float barWidth = 3.0f;
	float spacing = 2.0f;
	
	u32 activeColor = C2D_Color32(0, 255, 0, 255);
	if (isConnecting) activeColor = C2D_Color32(255, 200, 0, 255);
	if (isError) activeColor = C2D_Color32(255, 0, 0, 255);
	
	u32 inactiveColor = C2D_Color32(100, 100, 100, 255);
	
	for (int i = 0; i < 3; i++) {
		float barHeight = 6.0f + (i * 4.0f);
		float bx = startX + (i * (barWidth + spacing));
		float by = startY - barHeight;
		
		bool fill = false;
		if (isConnected) fill = true;
		else if (isConnecting && i < 2) fill = true; // Two bars for connecting
		else if (!isError && i == 0) fill = true;    // One bar if just idle
		
		C2D_DrawRectSolid(bx, by, 0.9f, barWidth, barHeight, fill ? activeColor : inactiveColor);
	}
	
	// Full screen error overlay if disconnected/error
	if (isError) {
		C2D_DrawRectSolid(0, 0, 0.95f, 320, 240, C2D_Color32(0, 0, 0, 230)); // Dark overlay
		auto& i18n = Core::I18n::getInstance();
		std::string errText = i18n.get("connection.no_network");
		
		C2D_TextBuf buf = C2D_TextBufNew(256);
		C2D_Text text;
		C2D_TextParse(&text, buf, errText.c_str());
		C2D_TextOptimize(&text);
		C2D_DrawText(&text, C2D_WithColor | C2D_AlignCenter, 160.0f, 110.0f, 0.96f, 0.6f, 0.6f, C2D_Color32(255, 50, 50, 255));
		C2D_TextBufDelete(buf);
	}
}

void ScreenManager::toggleDebugOverlay() { debugOverlayEnabled = !debugOverlayEnabled; }

void ScreenManager::renderDebugOverlay() {
	std::vector<std::string> logs = Logger::getRecentLogs();
	float y = 5.0f;
	float lineHeight = 10.0f;

	for (const auto &line : logs) {
		if (y + lineHeight > 240) {
			break;
		}

		C2D_Text text;
		C2D_TextParse(&text, debugTextBuf, line.c_str());
		C2D_TextOptimize(&text);
		C2D_DrawText(&text, C2D_WithColor, 5.0f, y, 1.0f, 0.4f, 0.4f, C2D_Color32(0, 255, 0, 255));

		y += lineHeight;
	}
}

void ScreenManager::drawHamburgerButton() {
	if (shouldShowBackArrow()) {
		C3D_Tex *backTex = ImageManager::getInstance().getLocalImage("romfs:/discord-icons/arrow-large-left.png", true);
		if (backTex) {
			ImageManager::ImageInfo info =
			    ImageManager::getInstance().getImageInfo("romfs:/discord-icons/arrow-large-left.png");
			Tex3DS_SubTexture sub;
			sub.width = (u16)info.originalW;
			sub.height = (u16)info.originalH;
			sub.left = 0.0f;
			sub.top = 0.0f;
			sub.right = (float)info.originalW / backTex->width;
			sub.bottom = (float)info.originalH / backTex->height;
			C2D_Image img = {backTex, &sub};
			float iconSize = 20.0f;
			float scale = iconSize / info.originalW;
			C2D_DrawImageAtRotated(img, 18, 18, 1.0f, -M_PI / 2.0f, nullptr, scale, scale);
			return;
		}
	}

	u32 color = colorText();
	float x = 12.0f;
	float y = 11.0f;
	float w = 18.0f;
	float h = 2.0f;
	float gap = 5.0f;
	float z = 1.0f;
	float r = 1.0f;

	drawRoundedRect(x, y, z, w, h, r, color);
	drawRoundedRect(x, y + gap, z, w, h, r, color);
	drawRoundedRect(x, y + gap * 2, z, w, h, r, color);
}

void ScreenManager::showToast(const std::string &message) {
	toastMessage = message;
	toastTimer = 120;
}

bool ScreenManager::isMenuHidden() const {
	auto &client = Discord::DiscordClient::getInstance();
	bool isConnecting = (client.getState() == Discord::ConnectionState::CONNECTING ||
	                     client.getState() == Discord::ConnectionState::AUTHENTICATING);

	return (currentType == ScreenType::LOGIN) || (currentType == ScreenType::DISCLAIMER) ||
	       (currentType == ScreenType::ADD_ACCOUNT && isConnecting) || (currentScreen && currentScreen->hidesMenu());
}

bool ScreenManager::shouldShowBackArrow() const {
	if (currentType != ScreenType::SETTINGS) {
		return false;
	}
	if (screenHistory.empty()) {
		return false;
	}

	ScreenType prev = screenHistory.back();
	return (prev == ScreenType::LOGIN || prev == ScreenType::DISCLAIMER);
}

void ScreenManager::drawToast() {
	float w = measureText(toastMessage, 0.5f, 0.5f) + 24.0f;
	float h = 32.0f;
	float x = (320.0f - w) / 2.0f;
	float y = 180.0f;
	float z = 0.95f;

	u32 bg = C2D_Color32(40, 40, 45, 235);
	drawRoundedRect(x, y, z, w, h, 8.0f, bg);
	float progressW = (w - 8.0f) * (toastTimer / 120.0f);
	drawRoundedRect(x + 4, y + h - 2.0f, z + 0.01f, progressW, 1.5f, 0.75f, colorSelection());

	C2D_SceneBegin(bottomTarget);
	drawCenteredText(y + 9.0f, z + 0.02f, 0.5f, 0.5f, colorWhite(), toastMessage, 320.0f);
}

void drawText(float x, float y, float z, float scaleX, float scaleY, u32 color, const std::string &rawText) {
	std::string text = Utils::Utf8::sanitizeText(rawText);

	if (!textBuf) {
		return;
	}

	C2D_Text c2dText;
	C2D_TextParse(&c2dText, textBuf, text.c_str());
	C2D_TextOptimize(&c2dText);
	C2D_DrawText(&c2dText, C2D_WithColor, x, y, z, scaleX, scaleY, color);
}

void drawCenteredText(float y, float z, float scaleX, float scaleY, u32 color, const std::string &rawText,
                      float screenWidth) {
	std::string text = Utils::Utf8::sanitizeText(rawText);

	if (!textBuf) {
		return;
	}

	C2D_Text c2dText;
	C2D_TextParse(&c2dText, textBuf, text.c_str());
	C2D_TextOptimize(&c2dText);

	float width, height;
	C2D_TextGetDimensions(&c2dText, scaleX, scaleY, &width, &height);

	float x = (screenWidth - width) / 2.0f;
	C2D_DrawText(&c2dText, C2D_WithColor, x, y, z, scaleX, scaleY, color);
}

float measureTextDirect(const std::string &rawText, float scaleX, float scaleY) {
	std::string text = Utils::Utf8::sanitizeText(rawText);

	if (!layoutTextBuf || text.empty()) {
		return 0.0f;
	}

	C2D_Text c2dText;
	C2D_TextParse(&c2dText, layoutTextBuf, text.c_str());

	float width, height;
	C2D_TextGetDimensions(&c2dText, scaleX, scaleY, &width, &height);

	C2D_TextBufClear(layoutTextBuf);
	return width;
}

float measureText(const std::string &text, float scaleX, float scaleY) {
	return UI::TextMeasureCache::getInstance().measureText(text, scaleX, scaleY);
}

void drawRoundedRect(float x, float y, float z, float w, float h, float radius, u32 color) {
	if (radius <= 0) {
		C2D_DrawRectSolid(x, y, z, w, h, color);
		return;
	}

	if (radius > w / 2) {
		radius = w / 2;
	}
	if (radius > h / 2) {
		radius = h / 2;
	}

	C2D_DrawRectSolid(x + radius, y, z, w - 2 * radius, h, color);
	C2D_DrawRectSolid(x, y + radius, z, radius, h - 2 * radius, color);
	C2D_DrawRectSolid(x + w - radius, y + radius, z, radius, h - 2 * radius, color);

	auto drawCorner = [&](float cx, float cy, float startAngle) {
		const int segments = 8;
		const float step = (M_PI / 2.0f) / segments;
		for (int i = 0; i < segments; i++) {
			float a1 = startAngle + i * step;
			float a2 = startAngle + (i + 1) * step;
			C2D_DrawTriangle(cx, cy, color, cx + radius * cos(a1), cy + radius * sin(a1), color, cx + radius * cos(a2),
			                 cy + radius * sin(a2), color, z);
		}
	};

	drawCorner(x + radius, y + radius, M_PI);
	drawCorner(x + w - radius, y + radius, 3 * M_PI / 2);
	drawCorner(x + w - radius, y + h - radius, 0);
	drawCorner(x + radius, y + h - radius, M_PI / 2);
}

void drawCircle(float x, float y, float z, float radius, u32 color) { C2D_DrawCircleSolid(x, y, z, radius, color); }

void drawRichText(float x, float y, float z, float scaleX, float scaleY, u32 color, const std::string &text) {
	if (!textBuf || text.empty()) {
		return;
	}

	size_t cursor = 0;
	float currentX = x;

	while (cursor < text.length()) {
		if (text[cursor] == '<') {
			size_t start = cursor;
			if (start + 6 < text.length()) {
				bool isAnimated = (text[start + 1] == 'a');
				if (text[start + 1] == ':' || isAnimated) {
					size_t secondColon = text.find(':', start + (isAnimated ? 3 : 2));
					if (secondColon != std::string::npos) {
						size_t closeBracket = text.find('>', secondColon);
						if (closeBracket != std::string::npos) {
							std::string id = text.substr(secondColon + 1, closeBracket - secondColon - 1);
							EmojiManager::EmojiInfo info = EmojiManager::getInstance().getEmojiInfo(id);
							float emojiSize = 28.0f * scaleY;

							if (info.tex) {
								float uMax = (float)info.originalW / info.tex->width;
								float vMax = (float)info.originalH / info.tex->height;

								Tex3DS_SubTexture subtex = {(u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax,
								                            1.0f - vMax};

								const C2D_Image img = {info.tex, &subtex};
								C2D_DrawImageAt(img, currentX, y + 1.0f, z, nullptr, emojiSize / info.originalW,
								                emojiSize / info.originalH);
							} else {
								EmojiManager::getInstance().prefetchEmoji(id);

								std::string name = text.substr(start + (isAnimated ? 3 : 2),
								                               secondColon - (start + (isAnimated ? 3 : 2)));
								std::string fallback = ":" + name + ":";
								drawText(currentX, y, z, scaleX, scaleY, color, fallback);

								currentX += measureText(fallback, scaleX, scaleY) - (emojiSize + (2.0f * scaleX));
							}

							currentX += emojiSize + (2.0f * scaleX);
							cursor = closeBracket + 1;
							continue;
						}
					}
				}
			}
		}

		size_t tempCursor = cursor;
		uint32_t codepoint = Utils::Utf8::decodeNext(text, tempCursor);

		if (Utils::Utf8::isEmoji(codepoint)) {
			size_t seqCursor = cursor;
			std::string sequence = Utils::Utf8::getEmojiSequence(text, seqCursor);
			std::string hex = Utils::Utf8::utf8ToHex(sequence);
			EmojiManager::EmojiInfo info = EmojiManager::getInstance().getTwemojiInfo(hex);
			float emojiSize = 28.0f * scaleY;

			if (info.tex) {
				float uMax = (float)info.originalW / info.tex->width;
				float vMax = (float)info.originalH / info.tex->height;
				Tex3DS_SubTexture subtex = {(u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax, 1.0f - vMax};
				const C2D_Image img = {info.tex, &subtex};
				C2D_DrawImageAt(img, currentX, y + 1.0f, z, nullptr, emojiSize / info.originalW,
				                emojiSize / info.originalH);
				currentX += emojiSize + (2.0f * scaleX);
				cursor = seqCursor;
				continue;
			} else {
				drawText(currentX, y, z, scaleX, scaleY, color, sequence);
				currentX += measureText(sequence, scaleX, scaleY);
				cursor = seqCursor;
				continue;
			}
		}

		size_t end = cursor;
		while (end < text.length()) {
			if (text[end] == '<') {
				if (end + 6 < text.length()) {
					bool isAnimated = (text[end + 1] == 'a');
					if (text[end + 1] == ':' || isAnimated) {
						size_t secondColon = text.find(':', end + (isAnimated ? 3 : 2));
						if (secondColon != std::string::npos) {
							size_t closeBracket = text.find('>', secondColon);
							if (closeBracket != std::string::npos) {
								break;
							}
						}
					}
				}
			}

			size_t nextC = end;
			uint32_t cp = Utils::Utf8::decodeNext(text, nextC);
			if (Utils::Utf8::isEmoji(cp)) {
				break;
			}
			end = nextC;
		}

		if (end > cursor) {
			std::string segment = text.substr(cursor, end - cursor);
			drawText(currentX, y, z, scaleX, scaleY, color, segment);
			currentX += measureText(segment, scaleX, scaleY);
			cursor = end;
		} else if (cursor < text.length()) {
			size_t nextC = cursor;
			Utils::Utf8::decodeNext(text, nextC);
			std::string segment = text.substr(cursor, nextC - cursor);
			drawText(currentX, y, z, scaleX, scaleY, color, segment);
			currentX += measureText(segment, scaleX, scaleY);
			cursor = nextC;
		}
	}
}

void drawCenteredRichText(float y, float z, float scaleX, float scaleY, u32 color, const std::string &rawText,
                          float screenWidth) {
	float width = measureRichText(rawText, scaleX, scaleY);
	float x = (screenWidth - width) / 2.0f;
	drawRichText(x, y, z, scaleX, scaleY, color, rawText);
}

float measureRichTextImpl(const std::string &text, float scaleX, float scaleY, bool unicodeOnly) {
	if (!layoutTextBuf || text.empty()) {
		return 0.0f;
	}

	size_t cursor = 0;
	float currentX = 0;

	while (cursor < text.length()) {
		if (!unicodeOnly && text[cursor] == '<') {
			size_t start = cursor;
			if (start + 6 < text.length()) {
				bool isAnimated = (text[start + 1] == 'a');
				if (text[start + 1] == ':' || isAnimated) {
					size_t secondColon = text.find(':', start + (isAnimated ? 3 : 2));
					if (secondColon != std::string::npos) {
						size_t closeBracket = text.find('>', secondColon);
						if (closeBracket != std::string::npos) {
							float emojiSize = 28.0f * scaleY;
							currentX += emojiSize + (2.0f * scaleX);
							cursor = closeBracket + 1;
							continue;
						}
					}
				}
			}
		}

		size_t tempCursor = cursor;
		uint32_t codepoint = Utils::Utf8::decodeNext(text, tempCursor);

		if (Utils::Utf8::isEmoji(codepoint)) {
			size_t seqCursor = cursor;
			std::string sequence = Utils::Utf8::getEmojiSequence(text, seqCursor);
			std::string hex = Utils::Utf8::utf8ToHex(sequence);
			EmojiManager::EmojiInfo info = EmojiManager::getInstance().getTwemojiInfo(hex);
			float emojiSize = 28.0f * scaleY;
			if (info.tex) {
				currentX += emojiSize + (2.0f * scaleX);
			} else {
				currentX += measureText(sequence, scaleX, scaleY);
			}
			cursor = seqCursor;
		} else {
			size_t end = cursor;
			while (end < text.length()) {
				if (!unicodeOnly && text[end] == '<') {
					if (end + 6 < text.length()) {
						bool isAnimated = (text[end + 1] == 'a');
						if (text[end + 1] == ':' || isAnimated) {
							size_t secondColon = text.find(':', end + (isAnimated ? 3 : 2));
							if (secondColon != std::string::npos) {
								size_t closeBracket = text.find('>', secondColon);
								if (closeBracket != std::string::npos) {
									break;
								}
							}
						}
					}
				}

				size_t nextC = end;
				uint32_t cp = Utils::Utf8::decodeNext(text, nextC);
				if (Utils::Utf8::isEmoji(cp)) {
					break;
				}
				end = nextC;
			}

			if (end > cursor) {
				std::string segment = text.substr(cursor, end - cursor);
				currentX += measureText(segment, scaleX, scaleY);
				cursor = end;
			} else if (cursor < text.length()) {
				size_t nextC = cursor;
				Utils::Utf8::decodeNext(text, nextC);
				std::string segment = text.substr(cursor, nextC - cursor);
				currentX += measureText(segment, scaleX, scaleY);
				cursor = nextC;
			}
		}
	}

	return currentX;
}

float measureRichText(const std::string &rawText, float scaleX, float scaleY) {
	return measureRichTextImpl(rawText, scaleX, scaleY, false);
}

std::string getTruncatedText(const std::string &text, float maxWidth, float scaleX, float scaleY) {
	if (measureText(text, scaleX, scaleY) <= maxWidth) {
		return text;
	}

	std::vector<size_t> offsets;
	for (size_t i = 0; i < text.length();) {
		offsets.push_back(i);
		unsigned char c = (unsigned char)text[i];
		if (c < 0x80) {
			i += 1;
		} else if ((c & 0xE0) == 0xC0) {
			i += 2;
		} else if ((c & 0xF0) == 0xE0) {
			i += 3;
		} else if ((c & 0xF4) == 0xF0) {
			i += 4;
		} else {
			i += 1;
		}
	}

	int low = 0;
	int high = (int)offsets.size() - 1;
	int best = 0;

	while (low <= high) {
		int mid = low + (high - low) / 2;
		std::string test = text.substr(0, offsets[mid]) + "...";
		if (measureText(test, scaleX, scaleY) <= maxWidth) {
			best = mid;
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}

	return text.substr(0, offsets[best]) + "...";
}

std::string getTruncatedRichText(const std::string &rawText, float maxWidth, float scaleX, float scaleY) {
	if (measureRichText(rawText, scaleX, scaleY) <= maxWidth) {
		return rawText;
	}

	std::vector<size_t> offsets;
	for (size_t i = 0; i < rawText.length();) {
		offsets.push_back(i);
		unsigned char c = (unsigned char)rawText[i];
		if (c < 0x80) {
			i += 1;
		} else if ((c & 0xE0) == 0xC0) {
			i += 2;
		} else if ((c & 0xF0) == 0xE0) {
			i += 3;
		} else if ((c & 0xF4) == 0xF0) {
			i += 4;
		} else {
			i += 1;
		}
	}

	int low = 0;
	int high = (int)offsets.size() - 1;
	int best = 0;

	while (low <= high) {
		int mid = low + (high - low) / 2;
		std::string test = rawText.substr(0, offsets[mid]) + "...";
		if (measureRichText(test, scaleX, scaleY) <= maxWidth) {
			best = mid;
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}

	return rawText.substr(0, offsets[best]) + "...";
}

void drawRichTextUnicodeOnly(float x, float y, float z, float scaleX, float scaleY, u32 color,
                             const std::string &rawText) {
	std::string text = Utils::Utf8::sanitizeText(rawText);

	if (!textBuf || text.empty()) {
		return;
	}

	size_t cursor = 0;
	float currentX = x;

	while (cursor < text.length()) {
		size_t tempCharCursor = cursor;
		uint32_t firstCp = Utils::Utf8::decodeNext(text, tempCharCursor);

		if (Utils::Utf8::isEmoji(firstCp)) {
			size_t seqCursor = cursor;
			std::string sequence = Utils::Utf8::getEmojiSequence(text, seqCursor);
			std::string hex = Utils::Utf8::utf8ToHex(sequence);
			EmojiManager::EmojiInfo info = EmojiManager::getInstance().getTwemojiInfo(hex);
			float emojiSize = 28.0f * scaleY;

			if (info.tex) {
				Tex3DS_SubTexture subtex;
				subtex.width = (u16)info.originalW;
				subtex.height = (u16)info.originalH;
				subtex.left = 0.0f;
				subtex.top = 0.0f;
				subtex.right = (float)info.originalW / info.tex->width;
				subtex.bottom = (float)info.originalH / info.tex->height;

				const C2D_Image img = {info.tex, &subtex};
				C2D_DrawImageAt(img, currentX, y + 1.0f, z, nullptr, emojiSize / info.originalW,
				                emojiSize / info.originalH);
				currentX += emojiSize + (0.0f * scaleX);
			} else {
				std::string clean = Utils::Utf8::sanitizeText(sequence);
				drawText(currentX, y, z, scaleX, scaleY, color, clean);
				currentX += measureText(clean, scaleX, scaleY);
			}
			cursor = seqCursor;
		} else {
			size_t end = cursor;
			while (end < text.length()) {
				size_t nextC = end;
				uint32_t cp = Utils::Utf8::decodeNext(text, nextC);
				if (Utils::Utf8::isEmoji(cp)) {
					break;
				}
				end = nextC;
			}

			if (end > cursor) {
				std::string segment = text.substr(cursor, end - cursor);
				drawText(currentX, y, z, scaleX, scaleY, color, segment);
				currentX += measureText(segment, scaleX, scaleY);
				cursor = end;
			} else if (cursor < text.length()) {
				size_t nextC = cursor;
				Utils::Utf8::decodeNext(text, nextC);
				std::string segment = text.substr(cursor, nextC - cursor);
				drawText(currentX, y, z, scaleX, scaleY, color, segment);
				currentX += measureText(segment, scaleX, scaleY);
				cursor = nextC;
			}
		}
	}
}

float measureRichTextUnicodeOnly(const std::string &rawText, float scaleX, float scaleY) {
	return measureRichTextImpl(rawText, scaleX, scaleY, true);
}

u32 ScreenManager::colorBackground() { return Config::getInstance().getTheme().bg; }
u32 ScreenManager::colorBackgroundDark() { return Config::getInstance().getTheme().bg_dark; }
u32 ScreenManager::colorBackgroundLight() { return Config::getInstance().getTheme().bg_light; }
u32 ScreenManager::colorAccent() { return Config::getInstance().getTheme().accent; }
u32 ScreenManager::colorText() { return Config::getInstance().getTheme().text; }
u32 ScreenManager::colorTextMuted() { return Config::getInstance().getTheme().text_muted; }
u32 ScreenManager::colorSuccess() { return Config::getInstance().getTheme().success; }
u32 ScreenManager::colorError() { return Config::getInstance().getTheme().error; }

u32 ScreenManager::colorInput() { return Config::getInstance().getTheme().input_bg; }
u32 ScreenManager::colorLink() { return Config::getInstance().getTheme().link; }

u32 ScreenManager::colorSeparator() { return Config::getInstance().getTheme().separator; }

u32 ScreenManager::colorHeaderGlass() {
	u32 bg = colorBackgroundDark();
	u8 r = (bg >> 0) & 0xFF;
	u8 g = (bg >> 8) & 0xFF;
	u8 b = (bg >> 16) & 0xFF;
	return C2D_Color32(r, g, b, 230);
}

u32 ScreenManager::colorHeaderBorder() { return Config::getInstance().getTheme().header_border; }

u32 ScreenManager::colorSelection() { return Config::getInstance().getTheme().selection; }

u32 ScreenManager::colorOverlay() { return Config::getInstance().getTheme().overlay; }

u32 ScreenManager::colorWhite() { return Config::getInstance().getTheme().pure_white; }

u32 ScreenManager::colorEmbed() { return Config::getInstance().getTheme().embed_bg; }

u32 ScreenManager::colorEmbedMedia() { return Config::getInstance().getTheme().embed_media_bg; }

u32 ScreenManager::colorReaction() { return Config::getInstance().getTheme().reaction_bg; }

u32 ScreenManager::colorReactionMe() { return Config::getInstance().getTheme().reaction_me_bg; }

void ScreenManager::resetSelection() {
	lastServerIndex = 0;
	lastServerScroll = 0;
	lastChannelIndex.clear();
	lastChannelScroll.clear();
	lastForumIndex.clear();
	lastForumScroll.clear();
	selectedGuildId = "";
	expandedFolders.clear();
}

void ScreenManager::clearCaches() {
	Discord::AvatarCache::getInstance().clear();
	ImageManager::getInstance().clear();
}

void drawOverlay(float z) { C2D_DrawRectSolid(0.0f, 0.0f, z, 400.0f, 240.0f, ScreenManager::colorOverlay()); }

void drawPopupBackground(float x, float y, float w, float h, float z, float radius) {
	drawRoundedRect(x, y, z, w, h, radius, ScreenManager::colorBackground());
}

void drawPopupMenuItem(float x, float y, float w, float h, float z, bool isSelected, u32 selectionColor) {
	if (isSelected) {
		drawRoundedRect(x, y, z, w, h, 6.0f, selectionColor);
	}
}

} // namespace UI
