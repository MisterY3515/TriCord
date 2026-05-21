#ifndef UPDATER_H
#define UPDATER_H

#include <string>
#include <functional>

class Updater {
public:
	static Updater& getInstance();

	// Checks for updates. If background is true, it only notifies via toast if an update is found.
	// If background is false, it shows a UI prompt asking to download.
	void checkForUpdates(bool background);

	// Performs the actual download and install.
	void performUpdate(const std::string& downloadUrl, const std::string& assetName);
	bool installCia(const std::string& path);

private:
	Updater() = default;
	~Updater() = default;
	
	// Helper to extract version numbers from tags like "v0.0.8" or "0.0.8"
	bool isNewerVersion(const std::string& currentVer, const std::string& remoteVer);
};

// C-linkage helper for the settings menu
extern "C" void triggerManualUpdateCheck();

#endif // UPDATER_H
