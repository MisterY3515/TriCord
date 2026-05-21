#include "utils/color_utils.h"
#include <cstdio>

namespace Utils {
namespace Color {

u32 hexToColor(const std::string &hex) {
	if (hex.empty() || hex[0] != '#') {
		return 0;
	}
	std::string s = hex.substr(1);
	if (s.length() < 6) {
		return 0;
	}

	unsigned int r = 0, g = 0, b = 0, a = 255;
	if (s.length() >= 8) {
		sscanf(s.c_str(), "%2x%2x%2x%2x", &r, &g, &b, &a);
	} else {
		sscanf(s.c_str(), "%2x%2x%2x", &r, &g, &b);
	}
	return C2D_Color32(r, g, b, a);
}

std::string colorToHex(u32 color) {
	char buf[10];
	u8 r = color & 0xFF;
	u8 g = (color >> 8) & 0xFF;
	u8 b = (color >> 16) & 0xFF;
	u8 a = (color >> 24) & 0xFF;
	sprintf(buf, "#%02X%02X%02X%02X", r, g, b, a);
	return std::string(buf);
}

} // namespace Color
} // namespace Utils
