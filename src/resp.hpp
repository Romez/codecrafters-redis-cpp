#pragma once

#include <format>
#include <variant>
#include <vector>
#include <string>
#include <string_view>
#include <expected>
#include <span>

#include "utils/str.hpp"

using RespString = std::string;

struct RespArray;

using RespMessage = std::variant<
    RespString,
    RespArray
>;

struct RespArray : std::vector<RespMessage> {
    using std::vector<RespMessage>::vector;
};

struct PingCommand {};

struct EchoCommand {
    std::string msg;
};

struct SetCommand {
    std::vector<std::pair<std::string, std::string>> args;
    // int ex = 0;
    // int px = 0;
};

struct GetCommand {
    std::string key;
};

struct InvalidCommand {
    std::string msg;
};

using Command = std::variant<
    PingCommand,
    EchoCommand,
    InvalidCommand
>;

std::string resp_simple_string(std::string_view msg);

std::string resp_simple_error(std::string_view  msg);

std::string resp_bulk_string(std::string_view arg);