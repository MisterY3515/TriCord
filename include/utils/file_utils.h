#pragma once
#include <string>
#include <vector>

#include <functional>

namespace Utils {
namespace File {

std::vector<char> readFile(const std::string &path);
std::vector<unsigned char> readFileBinary(const std::string &path);
bool writeFile(const std::string &path, const std::vector<unsigned char> &data);
bool writeFile(const std::string &path, const std::string &data);
bool downloadFile(const std::string &url, const std::string &path, std::function<void(size_t, size_t)> progressCallback = nullptr);

} // namespace File
} // namespace Utils
