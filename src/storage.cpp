#include "storage.hpp"

StorageValue* lookup_key(Storage& storage, const StorageKey& key) {
    auto it = storage.values.find(key);
    if (it == storage.values.end()) {
        return nullptr;
    }

    auto expIt = storage.expires.find(key);
    if (expIt != storage.expires.end()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= expIt->second) {
            storage.values.erase(key);
            storage.expires.erase(key);
            return nullptr;
        }
    }

    return &it->second;
}

template <typename T>
std::expected<T*, StorageError> lookup_key_as(Storage& storage, const StorageKey& key) {
    StorageValue* value = lookup_key(storage, key);
    if (!value) {
        return std::unexpected(StorageError::NotFound);
    }
    if (auto* ptr = std::get_if<T>(value)) {
        return ptr;
    }

    return std::unexpected(StorageError::WrongType);
}

template <typename T>
std::expected<T*, StorageError> lookup_or_create_key_as(Storage& storage, const StorageKey& key) {
    StorageValue* value = lookup_key(storage, key);
    if (!value) {
        auto result = storage.values.try_emplace(key, T{});
        auto it = result.first;
        storage.expires.erase(key);
        value = &(it->second);
    }
    if (auto* ptr = std::get_if<T>(value)) {
        return ptr;
    }

    return std::unexpected(StorageError::WrongType);
}

void dict_set(Storage& storage, SetCommand& cmd) {
    storage.values[cmd.key] = cmd.val;

    if (cmd.ttl) {
        storage.expires[cmd.key] = std::chrono::steady_clock::now() + *cmd.ttl;
    }
}

std::expected<StorageString, StorageError> dict_get(Storage& storage, const StorageKey& key) {
    auto result = lookup_key_as<StorageString>(storage, key);
    if (!result) {
        return std::unexpected(result.error());
    }
    return **result;
}

std::expected<size_t, StorageError> list_rpush(Storage& storage, const RpushCommand& cmd) {
    auto result = lookup_or_create_key_as<StorageList>(storage, cmd.listKey);
    if (!result) {
        return std::unexpected(result.error());
    }

    for (size_t i = 0; i < cmd.args.size(); ++i) {
        (*result)->push_back(cmd.args[i]);
    }

    return (*result)->size();
}

std::expected<size_t, StorageError> list_lpush(Storage& storage, const LpushCommand& cmd) {
    auto result = lookup_or_create_key_as<StorageList>(storage, cmd.listKey);
    if (!result) {
        return std::unexpected(result.error());
    }

    for (size_t i = 0; i < cmd.args.size(); ++i) {
        (*result)->push_front(cmd.args[i]);
    }

    return (*result)->size();
}

int normalize_neg_lrange_index(int index, size_t listSize) {
    return std::max((int)listSize + index, 0);
}

std::expected<std::vector<std::string>, StorageError> list_lrange(Storage& storage, const LrangeCommand& cmd) {
    auto listValue = lookup_key_as<StorageList>(storage, cmd.listKey);
    if (!listValue) {
        return std::unexpected(listValue.error());
    }

    StorageList& list = **listValue;

    int start = cmd.start;
    int stop = cmd.stop;

    if (start < 0) {
        start = normalize_neg_lrange_index(start, list.size());
    }

    if (stop < 0) {
        stop = normalize_neg_lrange_index(stop, list.size());
    }
    else {
        stop = stop >= list.size() ? list.size() - 1 : stop;
    }

    std::vector<std::string> result;

    if (start >= list.size()) {
        return result;
    }

    if (start > stop) {
        return result;
    }

    for (std::string s : list | std::views::drop(start) | std::views::take(stop - start + 1)) {
        result.push_back(s);
    }

    return result;
}