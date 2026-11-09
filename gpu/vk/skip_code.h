/* RX_VK_SKIP_CODE: whether a program has a stage of the given native code (vk_draw.cpp). */
#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

namespace mkwvk::skip_code {

/// Whether `codes`, a program's stage code keys as program_code_keys writes them (" <stage>:<16 hex digits>" each),
/// holds a key that starts with one of `list`'s comma-separated hex prefixes (in either case). Empty prefixes match
/// nothing, so an empty list leaves every program in.
inline bool matches(std::string_view codes, std::string_view list) {
    for (size_t q = 0; q <= list.size();) {
        size_t e = list.find(',', q);
        if (e == std::string_view::npos) e = list.size();
        const std::string_view prefix = list.substr(q, e - q);
        if (!prefix.empty())
            for (size_t at = codes.find(':'); at != std::string_view::npos; at = codes.find(':', at + 1)) {
                const std::string_view key = codes.substr(at + 1);
                size_t i = 0;
                while (i < prefix.size() && i < key.size() &&
                       std::tolower((unsigned char)key[i]) == std::tolower((unsigned char)prefix[i]))
                    i++;
                if (i == prefix.size()) return true;
            }
        q = e + 1;
    }
    return false;
}

}  // namespace mkwvk::skip_code
