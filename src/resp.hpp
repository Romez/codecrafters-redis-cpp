#pragma once

#include <format>
#include <variant>
#include <vector>
#include <string>
#include <string_view>
#include <expected>
#include <span>
#include <chrono>

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
    std::string key;
    std::string val;
    std::optional<std::chrono::microseconds> ttl;
};

struct GetCommand {
    std::string key;
};

struct RpushCommand {
    std::string listKey;
    std::vector<std::string> args;
};

struct LpushCommand {
    std::string listKey;
    std::vector<std::string> args;
};

struct InvalidCommand {
    std::string msg;
};

using Command = std::variant<
    PingCommand,
    EchoCommand,
    GetCommand,
    SetCommand,
    RpushCommand,
    LpushCommand,
    InvalidCommand
>;

std::string resp_simple_string(std::string_view msg);

std::string resp_simple_error(std::string_view  msg);

std::string resp_bulk_string(std::string_view arg);

std::string resp_blob_error(std::string_view  msg);

std::string resp_integer(int64_t val);