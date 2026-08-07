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
    for (size_t i = 0; i < cmd.args.size(); i += 1) {
        const auto& [key, val] = cmd.args[i];

        StorageString item{ .value = val };

        // int ms = px + ex * 1000;
        // if (ms > 0) {
        //     storage.expires[key] = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        // }
        // else {
        //     storage.expires.erase(key);
        // }

        storage.values[key] = item;
    }
}

std::expected<std::string, StorageError> dict_get(Storage& storage, const StorageKey& key) {
    auto result = lookup_key_as<StorageString>(storage, key);
    if (!result) {
        return std::unexpected(result.error());
    }
    return (*result)->value;
}