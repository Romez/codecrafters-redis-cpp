#include "resp.hpp"

std::string resp_simple_string(std::string_view msg) {
    return std::format("+{}\r\n", msg);
}

std::string resp_simple_error(std::string_view  msg) {
    return std::format("-ERR {}\r\n", msg);
}

std::string resp_bulk_string(std::string_view arg) {
    return std::format("${}\r\n{}\r\n", arg.size(), arg);
}

std::string resp_blob_error(std::string_view  msg) {
    return std::format("!{}\r\n<{}\r\n", msg.size(), msg);
}

std::string resp_integer(int64_t val) {
    return std::format(":{}\r\n", val);
}