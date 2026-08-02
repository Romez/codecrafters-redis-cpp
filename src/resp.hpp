#pragma once

#include <format>
#include <variant>
#include <vector>
#include <string>
#include <string_view>

using RespString = std::string;

struct RespArray;

using RespMessage = std::variant<
    RespString,
    RespArray
>;

struct RespArray : std::vector<RespMessage> {
    using std::vector<RespMessage>::vector;
};

std::string resp_simple_string(std::string_view msg);