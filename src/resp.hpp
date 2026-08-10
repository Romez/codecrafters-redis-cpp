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
    std::optional<std::chrono::milliseconds> ttl;
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

struct LrangeCommand {
    std::string listKey;
    int start;
    int stop;
};

struct LlenCommand {
    std::string listKey;
};

enum class LpopType {
    Single,
    Multiple,
};

struct LpopCommand {
    std::string listKey;
    size_t len;
    LpopType type;
};

// struct BlpopCommand {
//     std::vector<std::string> listKeys;
//     std::optional<std::chrono::milliseconds> timeout;
// };

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
    LrangeCommand,
    LlenCommand,
    LpopCommand,
    InvalidCommand
>;

std::string resp_simple_string(std::string_view msg);

std::string resp_simple_error(std::string_view  msg);

std::string resp_bulk_string(std::string_view arg);

std::string resp_blob_error(std::string_view  msg);

std::string resp_integer(int64_t val);

std::string resp_array(std::span<const std::string> args);