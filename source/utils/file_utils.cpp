#include "utils/file_utils.h"
#include "core/log.h"
#include <cstdio>
#include <vector>
#include <curl/curl.h>

namespace Utils {
namespace File {

std::vector<char> readFile(const std::string &path) {
	FILE *fp = fopen(path.c_str(), "r");
	if (!fp) {
		return {};
	}

	fseek(fp, 0, SEEK_END);
	size_t size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	std::vector<char> buffer(size + 1);
	if (size > 0) {
		fread(buffer.data(), 1, size, fp);
	}
	buffer[size] = '\0';
	fclose(fp);
	return buffer;
}

std::vector<unsigned char> readFileBinary(const std::string &path) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		return {};
	}

	fseek(fp, 0, SEEK_END);
	size_t size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	std::vector<unsigned char> buffer(size);
	if (size > 0) {
		fread(buffer.data(), 1, size, fp);
	}
	fclose(fp);
	return buffer;
}

bool writeFile(const std::string &path, const std::vector<unsigned char> &data) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (!fp) {
		return false;
	}
	fwrite(data.data(), 1, data.size(), fp);
	fclose(fp);
	return true;
}

bool writeFile(const std::string &path, const std::string &data) {
	FILE *fp = fopen(path.c_str(), "w");
	if (!fp) {
		return false;
	}
	fputs(data.c_str(), fp);
	fclose(fp);
	return true;
}

static size_t writeDataCallback(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	size_t written = fwrite(ptr, size, nmemb, stream);
	return written;
}

static int xferinfoCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
	auto* callback = static_cast<std::function<void(size_t, size_t)>*>(clientp);
	if (callback && *callback) {
		(*callback)(static_cast<size_t>(dlnow), static_cast<size_t>(dltotal));
	}
	return 0;
}

bool downloadFile(const std::string &url, const std::string &path, std::function<void(size_t, size_t)> progressCallback) {
	CURL *curl = curl_easy_init();
	if (!curl) return false;

	FILE *fp = fopen(path.c_str(), "wb");
	if (!fp) {
		curl_easy_cleanup(curl);
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDataCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // Disable SSL verification for simplicity on 3DS
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "TriCord-Updater/1.0");
	if (progressCallback) {
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfoCallback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressCallback);
	}

	CURLcode res = curl_easy_perform(curl);
	
	long httpCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

	fclose(fp);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || httpCode >= 400) {
		remove(path.c_str());
		Logger::log("[Updater] Download failed: %s (HTTP %ld)", curl_easy_strerror(res), httpCode);
		return false;
	}

	return true;
}

} // namespace File
} // namespace Utils
