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

struct InvalidCommand {
    std::string msg;
};

using Command = std::variant<
    PingCommand,
    EchoCommand,
    InvalidCommand
>;

Command build_cmd(RespMessage& msg);

std::string resp_simple_string(std::string_view msg);

std::string resp_simple_error(std::string_view  msg);

std::string resp_bulk_string(std::string_view arg);