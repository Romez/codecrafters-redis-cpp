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

void dict_set(Storage& storage, const SetCommand& cmd) {
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

std::expected<size_t, StorageError> list_len(Storage& storage, const LlenCommand& cmd) {
    auto list = lookup_key_as<StorageList>(storage, cmd.listKey);
    if (!list) {
        return std::unexpected(list.error());
    }

    return (*list)->size();
}

std::expected<std::vector<std::string>, StorageError> lpop(Storage& storage, const std::string& key, size_t len) {
    auto listValue = lookup_key_as<StorageList>(storage, key);
    if (!listValue) {
        return std::unexpected(listValue.error());
    }

    StorageList& list = **listValue;

    size_t itemsSize = std::min(list.size(), len);
    std::vector<std::string> items;
    for (size_t i = 0; i < itemsSize; ++i) {
        items.push_back(list.front());
        list.pop_front();
    }

    if (list.size() == 0) {
        storage.values.erase(key);
    }

    return items;
}

std::expected<std::vector<std::string>, StorageError> list_lpop(Storage& storage, const LpopCommand& cmd) {
    return lpop(storage, cmd.listKey, cmd.len);
}

std::expected<std::pair<std::string, std::string>, StorageError> blpop(Storage& storage, const BlpopCommand& cmd) {
    for (size_t i = 0; i < cmd.listKeys.size(); ++i) {
        const std::string &listKey = cmd.listKeys[i];

        auto result = lpop(storage, listKey, 1);
        if (!result) {
            if (result.error() == StorageError::NotFound) {
                continue;
            } else {
                return std::unexpected(result.error());
            }
        }

        return std::pair<std::string, std::string>{listKey, (*result)[0]};
    }
    return std::unexpected(StorageError::NotFound);
}

std::expected<StorageItemType, StorageError> key_type(Storage& storage, const StorageKey& key) {
    auto value = lookup_key(storage, key);
    if (!value) {
        return std::unexpected(StorageError::NotFound);
    }

    if (std::holds_alternative<StorageList>(*value)) {
        return StorageItemType::List;
    }
    else if (std::holds_alternative<StorageString>(*value)) {
        return StorageItemType::String;
    }
    else if (std::holds_alternative<StorageStream>(*value)) {
        return StorageItemType::Stream;
    }
    else {
        assert(false && "Unexpected storage type");
    }
}

EntryId get_last_stream_id(StorageStream& stream) {
    uint64_t prevMs = 0;
    uint64_t prevSeq = 0;

    if (stream.size() > 0) {
        StreamEntry& lastEntry = stream.back();
        prevMs = lastEntry.id.first;
        prevSeq = lastEntry.id.second;
    }

    return {prevMs, prevSeq};
}

std::expected<EntryId, StorageError> make_stream_key(StorageStream& stream, const RespStreamId& respId) {
    if (auto* id = std::get_if<RespStreamMsSeqId>(&respId)) {
        uint64_t nextMs = id->ms;
        uint64_t nextSeq = id->seq;

        if (nextMs == 0 && nextSeq == 0) {
            return std::unexpected(StorageError::StreamKeySmallerThanZero);
        }

        auto [prevMs, prevSeq] = get_last_stream_id(stream);

        if (nextMs < prevMs || (nextMs == prevMs && nextSeq <= prevSeq)) {
            return std::unexpected(StorageError::StreamKeySmallerThanTop);
        }

        return EntryId{nextMs, nextSeq};
    }

    if (auto* id = std::get_if<RespStreamMsId>(&respId)) {
        uint64_t nextMs = *id;

        auto [prevMs, prevSeq] = get_last_stream_id(stream);

        if (nextMs < prevMs) {
            return std::unexpected(StorageError::StreamKeySmallerThanTop);
        }

        if (nextMs == prevMs) {
            return EntryId{nextMs, prevSeq + 1};
        }

        return EntryId{nextMs, 0};
    }

    if (auto* id = std::get_if<RespStreamSpecialId>(&respId)) {
        if (*id == RespStreamSpecialId::Auto) {
            uint64_t nextMs = static_cast<uint64_t>(duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count());

            if (stream.size() == 0) {
                return EntryId{nextMs, 0};
            }

            auto [prevMs, prevSeq] = get_last_stream_id(stream);

            uint64_t nextSeq = prevMs == nextMs ? prevSeq + 1 : 0;

            return EntryId{nextMs, nextSeq};
        }

        assert(false && "Unhandled special key");
    }

    assert(false && "Unexpected resp stream id");
}

std::expected<EntryId, StorageError> xadd(Storage& storage, const XaddCommand& cmd) {
    auto streamValue = lookup_or_create_key_as<StorageStream>(storage, cmd.streamKey);
    if (!streamValue) {
        return std::unexpected(streamValue.error());
    }

    StorageStream& stream = **streamValue;

    auto id = make_stream_key(stream, cmd.id);
    if (!id) {
        return std::unexpected(id.error());
    }

    StreamEntry entry = {.id = *id};

    for (const auto& item : cmd.kvPairs) {
        entry.values[item.first] = item.second;
    }

    stream.push_back(entry);

    return *id;
}
