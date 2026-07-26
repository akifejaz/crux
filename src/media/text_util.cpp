#include "media/text_util.h"

#include <cstdio>
#include <sstream>

namespace crux::text {

std::string utf8_truncate(std::string s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    s.resize(max_bytes);
    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.back());
        if (c < 0x80) break;                              // ASCII — safe end
        if ((c & 0xC0) == 0xC0) { s.pop_back(); break; }  // leading byte
        s.pop_back();                                     // continuation byte
    }
    return s;
}

std::string hms(double seconds) {
    const int t = static_cast<int>(seconds + 0.5);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  t / 3600, (t % 3600) / 60, t % 60);
    return buf;
}

std::string ftos(double v) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);
    oss << v;
    return oss.str();
}

} // namespace crux::text
