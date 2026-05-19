#include "log.h"
#include <3ds.h>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr const char *kLogDir = "sdmc:/3ds/TriCord";
constexpr const char *kSessionLogFile = "sdmc:/3ds/TriCord/session.log";
constexpr const char *kPreviousSessionLogFile = "sdmc:/3ds/TriCord/session-prev.log";
constexpr const char *kPersistentLogFile = "sdmc:/3ds/TriCord/tricord.log";
constexpr const char *kCrashReportFile = "sdmc:/3ds/TriCord/crash_report.txt";
constexpr size_t kMaxLogLines = 2000;
constexpr size_t kMaxLogLineLength = 2048;
constexpr size_t kMaxCrashContextLength = 512;

std::deque<std::string> logBuffer;
std::mutex logMutex;
FILE *sessionLogFile = nullptr;
bool loggerInitialized = false;
bool fileLoggingEnabled = false;
bool crashReportWritten = false;
char currentCrashContext[kMaxCrashContextLength] = "startup";
char lastLogLine[kMaxLogLineLength] = "";

void ensureLogDirectory() { mkdir(kLogDir, 0700); }

void rotateSessionLogs() {
	remove(kPreviousSessionLogFile);
	rename(kSessionLogFile, kPreviousSessionLogFile);
}

void formatPrefix(char *buffer, size_t bufferSize) {
	const uint64_t uptimeMs = osGetTime();
	const time_t now = time(nullptr);
	struct tm timeInfo;
	bool hasLocalTime = localtime_r(&now, &timeInfo) != nullptr;

	if (hasLocalTime) {
		snprintf(buffer, bufferSize, "[%04d-%02d-%02d %02d:%02d:%02d.%03llu][%10llu ms] ", timeInfo.tm_year + 1900,
		         timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec,
		         static_cast<unsigned long long>(uptimeMs % 1000), static_cast<unsigned long long>(uptimeMs));
		return;
	}

	snprintf(buffer, bufferSize, "[%10llu ms] ", static_cast<unsigned long long>(uptimeMs));
}

void appendToFile(FILE *file, const char *line) {
	if (!file || !line) {
		return;
	}

	fputs(line, file);
	fputc('\n', file);
	fflush(file);
}

void writeCrashReportUnlocked(const char *reason) {
	if (crashReportWritten) {
		return;
	}
	crashReportWritten = true;

	int fd = open(kCrashReportFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		return;
	}

	char report[4096];
	const uint64_t uptimeMs = osGetTime();
	const int written = snprintf(
	    report, sizeof(report),
	    "TriCord crash report\n"
	    "reason: %s\n"
	    "uptime_ms: %llu\n"
	    "crash_context: %s\n"
	    "last_log_line: %s\n"
	    "session_log: %s\n"
	    "previous_session_log: %s\n"
	    "persistent_log: %s\n",
	    reason ? reason : "unknown", static_cast<unsigned long long>(uptimeMs), currentCrashContext, lastLogLine,
	    kSessionLogFile, kPreviousSessionLogFile, kPersistentLogFile);
	if (written > 0) {
		write(fd, report, static_cast<size_t>(written));
	}
	close(fd);
}

void writeLifecycleMarker(const char *label) {
	if (!label) {
		return;
	}

	char prefix[128];
	char line[512];
	formatPrefix(prefix, sizeof(prefix));
	snprintf(line, sizeof(line), "%s=== %s ===", prefix, label);
	appendToFile(sessionLogFile, line);
	if (fileLoggingEnabled) {
		FILE *persistentLog = fopen(kPersistentLogFile, "a");
		if (persistentLog) {
			appendToFile(persistentLog, line);
			fclose(persistentLog);
		}
	}
	strncpy(lastLogLine, line, sizeof(lastLogLine) - 1);
	lastLogLine[sizeof(lastLogLine) - 1] = '\0';
}

void crashSignalHandler(int signalNumber) {
	const char *reason = "signal";
	switch (signalNumber) {
	case SIGABRT:
		reason = "SIGABRT";
		break;
	case SIGSEGV:
		reason = "SIGSEGV";
		break;
	case SIGILL:
		reason = "SIGILL";
		break;
	case SIGFPE:
		reason = "SIGFPE";
		break;
#ifdef SIGBUS
	case SIGBUS:
		reason = "SIGBUS";
		break;
#endif
	case SIGTERM:
		reason = "SIGTERM";
		break;
	default:
		break;
	}

	writeCrashReportUnlocked(reason);
	signal(signalNumber, SIG_DFL);
	raise(signalNumber);
}

void registerCrashHandlers() {
	signal(SIGABRT, crashSignalHandler);
	signal(SIGSEGV, crashSignalHandler);
	signal(SIGILL, crashSignalHandler);
	signal(SIGFPE, crashSignalHandler);
	signal(SIGTERM, crashSignalHandler);
#ifdef SIGBUS
	signal(SIGBUS, crashSignalHandler);
#endif
}
} // namespace

namespace Logger {

void init() {
	std::lock_guard<std::mutex> lock(logMutex);
	if (loggerInitialized) {
		return;
	}

	ensureLogDirectory();
	rotateSessionLogs();
	remove(kCrashReportFile);

	sessionLogFile = fopen(kSessionLogFile, "w");
	if (sessionLogFile) {
		setvbuf(sessionLogFile, nullptr, _IOLBF, 0);
	}

	loggerInitialized = true;
	registerCrashHandlers();
	writeLifecycleMarker("TriCord Log Started");
}

void shutdown() {
	std::lock_guard<std::mutex> lock(logMutex);
	if (!loggerInitialized) {
		return;
	}

	writeLifecycleMarker("TriCord Log Closed");
	if (sessionLogFile) {
		fflush(sessionLogFile);
		fclose(sessionLogFile);
		sessionLogFile = nullptr;
	}
	loggerInitialized = false;
}

void log(const char *fmt, ...) {
	char message[kMaxLogLineLength];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	char prefix[128];
	formatPrefix(prefix, sizeof(prefix));

	char line[kMaxLogLineLength + 160];
	snprintf(line, sizeof(line), "%s%s", prefix, message);

	std::lock_guard<std::mutex> lock(logMutex);
	std::string logLine(line);
	logBuffer.push_back(logLine);
	if (logBuffer.size() > kMaxLogLines) {
		logBuffer.pop_front();
	}

	strncpy(lastLogLine, line, sizeof(lastLogLine) - 1);
	lastLogLine[sizeof(lastLogLine) - 1] = '\0';

	printf("%s\n", line);
	svcOutputDebugString(line, strlen(line));

	if (sessionLogFile) {
		appendToFile(sessionLogFile, line);
	}

	if (fileLoggingEnabled) {
		FILE *persistentLog = fopen(kPersistentLogFile, "a");
		if (persistentLog) {
			appendToFile(persistentLog, line);
			fclose(persistentLog);
		}
	}
}

void flush() {
	std::lock_guard<std::mutex> lock(logMutex);
	if (sessionLogFile) {
		fflush(sessionLogFile);
	}
}

void setCrashContext(const char *fmt, ...) {
	if (!fmt) {
		return;
	}

	char context[kMaxCrashContextLength];
	va_list args;
	va_start(args, fmt);
	vsnprintf(context, sizeof(context), fmt, args);
	va_end(args);

	std::lock_guard<std::mutex> lock(logMutex);
	strncpy(currentCrashContext, context, sizeof(currentCrashContext) - 1);
	currentCrashContext[sizeof(currentCrashContext) - 1] = '\0';
}

std::vector<std::string> getRecentLogs() {
	std::lock_guard<std::mutex> lock(logMutex);
	return std::vector<std::string>(logBuffer.begin(), logBuffer.end());
}

void setFileLoggingEnabled(bool enabled) {
	std::lock_guard<std::mutex> lock(logMutex);
	fileLoggingEnabled = enabled;
	if (enabled) {
		FILE *persistentLog = fopen(kPersistentLogFile, "a");
		if (persistentLog) {
			char prefix[128];
			char line[512];
			formatPrefix(prefix, sizeof(prefix));
			snprintf(line, sizeof(line), "%s=== Persistent archive logging enabled ===", prefix);
			appendToFile(persistentLog, line);
			fclose(persistentLog);
		}
	}
}

bool isFileLoggingEnabled() {
	std::lock_guard<std::mutex> lock(logMutex);
	return fileLoggingEnabled;
}

} // namespace Logger
