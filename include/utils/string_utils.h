#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <string>
#include <vector>
#include <cstdio>

namespace Utils {
namespace String {

std::string trim(const std::string &str);

// Variadic format function
template<typename... Args>
std::string format(const std::string& format, Args... args) {
	int size_s = snprintf(nullptr, 0, format.c_str(), args...) + 1; // Extra space for '\0'
	if (size_s <= 0) { return ""; }
	auto size = static_cast<size_t>(size_s);
	std::vector<char> buf(size);
	snprintf(buf.data(), size, format.c_str(), args...);
	return std::string(buf.data(), buf.data() + size - 1); // We don't want the '\0' inside
}

} // namespace String
} // namespace Utils

#endif // STRING_UTILS_H
