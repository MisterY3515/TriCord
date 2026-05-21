#pragma once

#include "discord/discord_client.h"
#include <3ds.h>
#include <ctime>
#include <string>
#include <vector>

namespace UI {
namespace MessageUtils {

void syncClock(const std::string &dateStr);
time_t getUtcNow();
s64 get3DSLocalTimeOffset();
time_t parseISO8601(const std::string &timestamp);
time_t snowflakeToTimestamp(const std::string &snowflake);
std::string formatTimestamp(const std::string &timestamp);
std::string getLocalDateString(const std::string &timestamp);
std::string formatTimeOnly(const std::string &timestamp);
std::string getISOTimestamp(time_t epoch);
std::string getRelativeTime(time_t targetEpoch);

std::vector<std::string> wrapText(const std::string &text, float maxWidth, float scale, bool unicodeOnly = false);
bool isEmojiOnly(const std::string &text, int &count);
std::string getChannelDisplayName(const Discord::Channel &channel);

bool canGroupWithPrevious(const Discord::Message &current, const Discord::Message &previous);

} // namespace MessageUtils
} // namespace UI
