#pragma once

#include <chrono>
#include <unordered_map>
#include <list>
#include <ranges>
#include <asio.hpp>
#include <expected>
#include <variant>
#include <span>
#include "resp.hpp"

using StorageString = std::string;

using StorageList = std::list<std::string>;

using StorageKey = std::string;

using EntryId = std::pair<uint64_t, uint64_t>;

struct StreamEntry {
    EntryId id;
    std::unordered_map<std::string, std::string> values;
};

inline bool operator<(const StreamEntry& a, const StreamEntry& b) {
    return a.id < b.id;
}

using StorageStream = std::vector<StreamEntry>;

using StorageValue = std::variant<
    StorageString,
    StorageList,
    StorageStream
>;

enum class StorageError {
    StreamKeySmallerThanZero,
    StreamKeySmallerThanTop,
    WrongType,
    NotFound,
};

enum class StorageItemType {
    String,
    List,
    Stream,
};

struct Storage {
    std::unordered_map<StorageKey, StorageValue> values;
    std::unordered_map<StorageKey, std::chrono::steady_clock::time_point> expires;
};

void dict_set(Storage& storage, SetCommand& cmd);

std::expected<std::string, StorageError> dict_get(Storage& storage, const StorageKey& key);

std::expected<size_t, StorageError> list_rpush(Storage& storage, const RpushCommand& cmd);

std::expected<size_t, StorageError> list_lpush(Storage& storage, const LpushCommand& cmd);

std::expected<std::vector<std::string>, StorageError> list_lrange(Storage& storage, const LrangeCommand& cmd);

std::expected<size_t, StorageError> list_len(Storage& storage, const LlenCommand& cmd);

std::expected<std::vector<std::string>, StorageError> list_lpop(Storage& storage, const LpopCommand& cmd);

std::expected<std::pair<std::string, std::string>, StorageError> blpop(Storage& storage, const BlpopCommand& cmd)