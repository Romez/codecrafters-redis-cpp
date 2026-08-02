#pragma once

#include <iostream>

inline std::string to_lower_case(std::string_view s) {
    std::string result{s};
    for (size_t i = 0; i < s.length(); ++i) {
        if (0x41 <= s[i] && s[i] <= 0x5a) {
            result[i] += 0x20;
        }
    }
    return result;
}