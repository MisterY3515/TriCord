#include "core/updater.h"
#include "core/config.h"
#include "core/log.h"
#include "network/http_client.h"
#include "utils/file_utils.h"
#include "ui/screen_manager.h"
#include <3ds.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <vector>
#include <sstream>

// Helper to install CIA from a local file
static bool installCia(const std::string& path) {
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

bool Updater::isNewerVersion(const std::string& currentVer, const std::string& remoteVer) {
	// Simple string comparison for versions like "v0.0.8" vs "v0.0.9"
	std::string c = currentVer;
	std::string r = remoteVer;
	if (!c.empty() && c[0] == 'v') c = c.substr(1);
	if (!r.empty() && r[0] == 'v') r = r.substr(1);
	// Basic lexicographical check for now (assumes well-formed major.minor.micro)
	return r > c;
}

void Updater::checkForUpdates(bool background) {
	Network::HttpClient client;
	client.setHeader("User-Agent", "TriCord-Updater/1.0");
	client.setVerifySSL(false);

	auto resp = client.get("https://api.github.com/repos/MisterY3515/TriCord-Revanced/releases");
	if (!resp.success || resp.statusCode >= 400) {
		if (!background) {
			UI::ScreenManager::getInstance().showToast("Failed to fetch updates.");
		}
		return;
	}

	rapidjson::Document doc;
	doc.Parse(resp.body.c_str());
	if (doc.HasParseError() || !doc.IsArray() || doc.Size() == 0) {
		if (!background) {
			UI::ScreenManager::getInstance().showToast("Failed to parse updates.");
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
		if (!background) UI::ScreenManager::getInstance().showToast("No suitable releases found.");
		return;
	}

	std::string remoteTag = (*latestRelease)["tag_name"].GetString();
	// Compare with current version - Assuming 0.0.8 based on AppInfo
	std::string currentTag = "0.0.8";

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
			if (background) {
				UI::ScreenManager::getInstance().showToast("Update " + remoteTag + " available! Check Settings.");
			} else {
				// We found an update! Let's download it directly
				UI::ScreenManager::getInstance().showToast("Downloading " + remoteTag + "...");
				performUpdate(downloadUrl, targetAssetName);
			}
		} else {
			if (!background) UI::ScreenManager::getInstance().showToast("Release found, but no matching asset.");
		}
	} else {
		if (!background) UI::ScreenManager::getInstance().showToast("TriCord is up to date.");
	}
}

void Updater::performUpdate(const std::string& downloadUrl, const std::string& assetName) {
	bool is3dsx = envIsHomebrew();
	
	if (is3dsx) {
		std::string path = "sdmc:/3ds/TriCord/TriCord.3dsx";
		if (Utils::File::downloadFile(downloadUrl, path)) {
			UI::ScreenManager::getInstance().showToast("Update complete! Please restart Homebrew Launcher.");
		} else {
			UI::ScreenManager::getInstance().showToast("Update download failed.");
		}
	} else {
		std::string tempPath = "sdmc:/3ds/TriCord/update.cia";
		if (Utils::File::downloadFile(downloadUrl, tempPath)) {
			UI::ScreenManager::getInstance().showToast("Installing CIA...");
			if (installCia(tempPath)) {
				remove(tempPath.c_str());
				UI::ScreenManager::getInstance().showToast("Install complete! Please restart the app.");
			} else {
				UI::ScreenManager::getInstance().showToast("CIA install failed.");
			}
		} else {
			UI::ScreenManager::getInstance().showToast("Update download failed.");
		}
	}
}

extern "C" void triggerManualUpdateCheck() {
	// Call on a background thread so we don't block the UI while HTTP completes
	threadCreate([](void*) {
		Updater::getInstance().checkForUpdates(false);
	}, nullptr, 16 * 1024, 0x1A, -2, false);
}
