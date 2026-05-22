#include "core/updater.h"
#include "core/config.h"
#include "core/log.h"
#include "network/http_client.h"
#include "utils/file_utils.h"
#include "ui/screen_manager.h"
#include "ui/update_screen.h"
#include <3ds.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <vector>
#include <sstream>
#include <cctype>
#include "core/i18n.h"
#include "utils/string_utils.h"

// Helper to install CIA from a local file
bool Updater::installCia(const std::string& path) {
	Handle fileHandle;
	FS_Archive sdmcArchive = 0;
	FS_Path archPath = fsMakePath(PATH_EMPTY, "");
	Result res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, archPath);
	if (R_FAILED(res)) return false;

	// Use path without "sdmc:/" for FSUSER_OpenFile
	std::string relativePath = path;
	if (relativePath.find("sdmc:/") == 0) {
		relativePath = relativePath.substr(6);
	}
	// ensure starts with '/'
	if (relativePath.empty() || relativePath[0] != '/') {
		relativePath = "/" + relativePath;
	}

	std::vector<uint16_t> utf16Path(relativePath.length() + 1);
	for (size_t i = 0; i < relativePath.length(); ++i) utf16Path[i] = relativePath[i];
	utf16Path[relativePath.length()] = 0;

	FS_Path filePath = fsMakePath(PATH_UTF16, utf16Path.data());
	res = FSUSER_OpenFile(&fileHandle, sdmcArchive, filePath, FS_OPEN_READ, 0);
	if (R_FAILED(res)) {
		FSUSER_CloseArchive(sdmcArchive);
		return false;
	}

	amInit();
	Handle ciaHandle;
	res = AM_StartCiaInstall(MEDIATYPE_SD, &ciaHandle);
	if (R_FAILED(res)) {
		FSFILE_Close(fileHandle);
		FSUSER_CloseArchive(sdmcArchive);
		amExit();
		return false;
	}

	u32 bytesRead = 0;
	u32 bytesWritten = 0;
	u64 offset = 0;
	std::vector<u8> buffer(128 * 1024);

	bool success = true;
	while (true) {
		res = FSFILE_Read(fileHandle, &bytesRead, offset, buffer.data(), buffer.size());
		if (R_FAILED(res) || bytesRead == 0) break;

		res = FSFILE_Write(ciaHandle, &bytesWritten, offset, buffer.data(), bytesRead, FS_WRITE_FLUSH);
		if (R_FAILED(res) || bytesWritten != bytesRead) {
			success = false;
			break;
		}
		offset += bytesRead;
	}

	FSFILE_Close(fileHandle);
	FSUSER_CloseArchive(sdmcArchive);

	if (success) {
		res = AM_FinishCiaInstall(ciaHandle);
		if (R_FAILED(res)) success = false;
	} else {
		AM_CancelCIAInstall(ciaHandle);
	}

	amExit();
	return success;
}

Updater& Updater::getInstance() {
	static Updater instance;
	return instance;
}

struct Version {
	int major = 0;
	int minor = 0;
	int patch = 0;
	std::string preReleaseType;
	int preReleaseNum = 0;
	bool isPreRelease = false;
};

static Version parseVersion(const std::string& vstr) {
	Version v;
	std::string s = vstr;
	if (!s.empty() && s[0] == 'v') s = s.substr(1);
	if (s.empty()) return v;

	// Safe parsing without exceptions
	const char* p = s.c_str();
	char* end = nullptr;

	v.major = (int)strtol(p, &end, 10);
	if (end == p) return v; // no digits
	p = end;

	if (*p == '.') {
		p++;
		v.minor = (int)strtol(p, &end, 10);
		if (end == p) return v;
		p = end;
		if (*p == '.') {
			p++;
			v.patch = (int)strtol(p, &end, 10);
			if (end == p) return v;
			p = end;
		}
	}

	if (*p != '\0') {
		v.isPreRelease = true;
		std::string suffix = p;
		if (!suffix.empty() && suffix[0] == '-') suffix = suffix.substr(1);

		size_t digitPos = 0;
		while (digitPos < suffix.length() && !isdigit(suffix[digitPos])) {
			digitPos++;
		}
		v.preReleaseType = suffix.substr(0, digitPos);
		if (digitPos < suffix.length()) {
			v.preReleaseNum = (int)strtol(suffix.c_str() + digitPos, nullptr, 10);
		}
	}
	return v;
}

bool operator<(const Version& a, const Version& b) {
	if (a.major != b.major) return a.major < b.major;
	if (a.minor != b.minor) return a.minor < b.minor;
	if (a.patch != b.patch) return a.patch < b.patch;
	
	if (a.isPreRelease && !b.isPreRelease) return true;
	if (!a.isPreRelease && b.isPreRelease) return false;
	
	if (a.isPreRelease && b.isPreRelease) {
		if (a.preReleaseType != b.preReleaseType) return a.preReleaseType < b.preReleaseType;
		return a.preReleaseNum < b.preReleaseNum;
	}
	return false;
}

bool Updater::isNewerVersion(const std::string& currentVer, const std::string& remoteVer) {
	Version c = parseVersion(currentVer);
	Version r = parseVersion(remoteVer);
	return c < r;
}

void Updater::checkForUpdates(bool background) {
	auto& i18n = Core::I18n::getInstance();
	Network::HttpClient client;
	client.setHeader("User-Agent", "TriCord-Updater/1.0");
	client.setVerifySSL(false);

	auto resp = client.get("https://api.github.com/repos/MisterY3515/TriCord-Revanced/releases");
	if (!resp.success || resp.statusCode >= 400) {
		if (!background) {
			UI::ScreenManager::getInstance().showToast(i18n.get("updater.failed_fetch"));
		}
		return;
	}

	rapidjson::Document doc;
	doc.Parse(resp.body.c_str());
	if (doc.HasParseError() || !doc.IsArray() || doc.Size() == 0) {
		if (!background) {
			UI::ScreenManager::getInstance().showToast(i18n.get("updater.failed_parse"));
		}
		return;
	}

	const bool wantsPreReleases = Config::getInstance().isReceivePreReleasesEnabled();
	const rapidjson::Value* latestRelease = nullptr;

	for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
		const auto& release = doc[i];
		bool isPreRelease = release.HasMember("prerelease") && release["prerelease"].GetBool();
		if (isPreRelease && !wantsPreReleases) {
			continue;
		}
		latestRelease = &release;
		break;
	}


	if (!latestRelease) {
		if (!background) UI::ScreenManager::getInstance().showToast(i18n.get("updater.no_releases"));
		return;
	}

	std::string remoteTag = (*latestRelease)["tag_name"].GetString();
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#ifdef APP_VERSION_PRERELEASE
	std::string currentTag = TOSTRING(APP_VERSION_MAJOR) "." TOSTRING(APP_VERSION_MINOR) "." TOSTRING(APP_VERSION_MICRO) TOSTRING(APP_VERSION_PRERELEASE);
#else
	std::string currentTag = TOSTRING(APP_VERSION_MAJOR) "." TOSTRING(APP_VERSION_MINOR) "." TOSTRING(APP_VERSION_MICRO);
#endif

	if (isNewerVersion(currentTag, remoteTag)) {
		// Find correct asset
		bool is3dsx = envIsHomebrew();
		std::string targetAssetName = is3dsx ? "TriCord.3dsx" : "TriCord.cia";
		std::string downloadUrl = "";

		if (latestRelease->HasMember("assets") && (*latestRelease)["assets"].IsArray()) {
			const auto& assets = (*latestRelease)["assets"];
			for (rapidjson::SizeType i = 0; i < assets.Size(); ++i) {
				const auto& asset = assets[i];
				std::string name = asset["name"].GetString();
				if (name == targetAssetName) {
					downloadUrl = asset["browser_download_url"].GetString();
					break;
				}
			}
		}

		if (!downloadUrl.empty()) {
			// Show a prompt to download
			std::string title = i18n.get("updater.title");
			std::string desc = Core::I18n::format(i18n.get("updater.desc"), remoteTag);
			
			UI::ScreenManager::getInstance().showModal(title, desc, 
				{i18n.get("common.cancel"), i18n.get("common.ok")},
				[downloadUrl, targetAssetName](int buttonIndex) {
					if (buttonIndex == 1) { // OK
						Updater::getInstance().performUpdate(downloadUrl, targetAssetName);
					}
				});
		} else {
			if (!background) UI::ScreenManager::getInstance().showToast(i18n.get("updater.no_asset"));
		}
	} else {
		if (!background) UI::ScreenManager::getInstance().showToast(i18n.get("updater.up_to_date"));
	}
}

void Updater::performUpdate(const std::string& downloadUrl, const std::string& assetName) {
	UI::ScreenManager::getInstance().pushCustomScreen(std::make_unique<UI::UpdateScreen>(downloadUrl, assetName));
}

extern "C" void triggerManualUpdateCheck() {
	// Call on a background thread so we don't block the UI while HTTP completes
	Thread updateThread = threadCreate([](void*) {
		Updater::getInstance().checkForUpdates(false);
	}, nullptr, 16 * 1024, 0x1A, -2, false);
	if (updateThread) threadDetach(updateThread);
}
