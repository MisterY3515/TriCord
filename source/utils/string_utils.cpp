#include "utils/string_utils.h"

namespace Utils {
namespace String {

std::string trim(const std::string &str) {
	size_t first = str.find_first_not_of(" \n\r\t");
	if (first == std::string::npos) {
		return "";
	}
	size_t last = str.find_last_not_of(" \n\r\t");
	return str.substr(first, (last - first + 1));
}

} // namespace String
} // namespace Utils
