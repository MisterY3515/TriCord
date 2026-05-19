#ifndef LOG_H
#define LOG_H

#include <string>
#include <vector>

namespace Logger {
void init();
void shutdown();
void log(const char *fmt, ...);
void flush();
void setCrashContext(const char *fmt, ...);
std::vector<std::string> getRecentLogs();
void setFileLoggingEnabled(bool enabled);
bool isFileLoggingEnabled();

} // namespace Logger

#endif // LOG_H
