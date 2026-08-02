#include "resp.hpp"

std::string resp_simple_string(std::string_view msg) {
    return std::format("+{}\r\n", msg);
}