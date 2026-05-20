#ifndef AVATAR_CACHE_H
#define AVATAR_CACHE_H

#include "utils/image_utils.h"
#include <citro2d.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Discord {

struct AvatarInfo {
	C3D_Tex *tex = nullptr;
	std::string url;
	bool loading = false;
	uint64_t lastAccess = 0;
};

class AvatarCache {
  public:
	static AvatarCache &getInstance();

	void init();
	void shutdown();
	void update();
	void clear();
	void evictOldestIfNeeded();
	bool isBusy() const;

	C3D_Tex *getAvatar(const std::string &userId, const std::string &avatarHash, const std::string &discriminator);
	C3D_Tex *getGuildIcon(const std::string &guildId, const std::string &iconHash);
	C3D_Tex *getChannelIcon(const std::string &channelId, const std::string &iconHash);

	void prefetchAvatar(const std::string &userId, const std::string &avatarHash, const std::string &discriminator);
	void prefetchGuildIcon(const std::string &guildId, const std::string &iconHash);
	void prefetchChannelIcon(const std::string &channelId, const std::string &iconHash);

  private:
	AvatarCache() {}
	~AvatarCache() { clear(); }

	std::map<std::string, AvatarInfo> cache;
	std::recursive_mutex cacheMutex;
	uint64_t accessCounter = 0;
	const size_t MAX_CACHE_SIZE = 100;

	struct PendingAvatar {
		std::string id;
		Utils::Image::TiledData tiled;
	};
	std::vector<PendingAvatar> pendingAvatars;
};

} // namespace Discord

#endif // AVATAR_CACHE_H
