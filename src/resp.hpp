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

struct BlpopCommand {
    std::vector<std::string> listKeys;
    std::optional<std::chrono::milliseconds> timeout;
};

struct TypeCommand {
    std::string key;
};

struct RespStreamMsSeqId {
    uint64_t ms;
    uint64_t seq;
};

using RespStreamMsId = uint64_t;

enum class RespStreamSpecialId {
    Auto,
    Min,
    Max
};

using RespStreamId = std::variant<
    RespStreamMsSeqId,
    RespStreamMsId,
    RespStreamSpecialId
>;

struct XaddCommand {
    std::string streamKey;
    RespStreamId id;
    std::vector<std::pair<std::string, std::string>> kvPairs;
};

struct XrangeCommand {
    std::string streamKey;
    RespStreamId start;
    RespStreamId stop;
};

struct XreadCommand {
    std::vector<std::pair<std::string, RespStreamId>> streams;
    std::optional<long> timoutMs;
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
    LrangeCommand,
    LlenCommand,
    LpopCommand,
    BlpopCommand,
    TypeCommand,
    XaddCommand,
    XrangeCommand,
    XreadCommand,
    InvalidCommand
>;

std::string resp_simple_string(std::string_view msg);

std::string resp_simple_error(std::string_view  msg);

std::string resp_bulk_string(std::string_view arg);

std::string resp_blob_error(std::string_view  msg);

std::string resp_integer(int64_t val);

std::string resp_array(std::span<const std::string> args);
