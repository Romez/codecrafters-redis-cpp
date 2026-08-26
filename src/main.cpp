#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>
#include <asio/experimental/promise.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/experimental/deferred.hpp>
#include <expected>
#include <variant>
#include <list>

#include "resp.hpp"
#include "parser.hpp"
#include "storage.hpp"

using asio::ip::tcp;
using asio::experimental::promise;
using asio::experimental::channel;

struct Waiter {
    std::optional<Command> cmd;
    channel<void(std::error_code, std::string)> chan;
};

using Waitings = std::unordered_map<
    StorageKey,
    std::list<std::shared_ptr<Waiter>>
>;

constexpr int port = 6379;
constexpr size_t read_buf_size = 128;
constexpr auto default_block_timeout = std::chrono::milliseconds(9999);

std::string stream_entry_values_to_resp_array(const StreamEntry& entry) {
    std::vector<std::string> values;
    for (const auto &[k, v] : entry.values) {
        values.push_back(resp_bulk_string(k));
        values.push_back(resp_bulk_string(v));
    }

    return resp_array(values);
}

std::string format_stream_entries(std::span<const StreamEntry> entries) {
    std::vector<std::string> items;

    for (auto &entry : entries) {
        std::string id = resp_bulk_string(std::format("{}-{}", entry.id.ms, entry.id.seq));
        std::string values = stream_entry_values_to_resp_array(entry);

        std::string item = resp_array(std::vector<std::string>{id, values});

        items.push_back(item);
    }

    return resp_array(items);
}

std::string format_xread_items(const std::vector<std::pair<StorageKey, std::span<StreamEntry>>>& streams) {
    std::vector<std::string> items;
    for (const auto &[streamId, entries] : streams) {
        auto id = resp_bulk_string(streamId);
        auto msg = resp_array(std::vector<std::string>{id, format_stream_entries(entries)});
        items.push_back(msg);
    }

    return resp_array(items);
}

std::string resp_storage_error(StorageError err) {
    switch (err) {
    case StorageError::NotFound:
        return "$-1\r\n";

    case StorageError::WrongType:
        return resp_simple_error("WRONGTYPE Operation against a key holding the wrong kind of value");

    case StorageError::StreamKeyLess:
        return resp_simple_error("The ID specified in XADD is equal or smaller than the target stream top item");

    case StorageError::StreamKeyZero:
        return resp_simple_error("The ID specified in XADD must be greater than 0-0");
    }

    std::unreachable();
}

std::expected<void, std::string> add_waiter(Waitings& waitings, std::shared_ptr<Waiter> waiter, const Command& cmd) {
    assert(waiter->cmd == std::nullopt && "Expected command is empty");

    if (const auto* blpop = std::get_if<BlpopCommand>(&cmd)) {
        for (const auto& k : blpop->listKeys) {
            waiter->cmd = cmd;
            waiter->chan.reset();
            waitings[k].push_back(waiter);
        }
    }
    else if (const auto* xread = std::get_if<XreadCommand>(&cmd)) {
        for (const auto& s : xread->streams) {
            waiter->cmd = cmd;
            waiter->chan.reset();
            auto& k = s.first;
            waitings[k].push_back(waiter);
        }
    }
    else {
        return std::unexpected("Unexpected blocking cmd");
    }

    return {};
}

std::expected<void, std::string> clear_waiter(Waitings& waitings, std::shared_ptr<Waiter> waiter) {
    if (waiter->cmd) {
        if (const auto* blpop = std::get_if<BlpopCommand>(&*waiter->cmd)) {
            for (const auto& k : blpop->listKeys) {
                std::erase(waitings[k], waiter);
                if (waitings[k].empty()) {
                    waitings.erase(k);
                }
            }
            waiter->cmd = std::nullopt;
            return {};
        }
        else if (const auto* xrd = std::get_if<XreadCommand>(&*waiter->cmd)) {
            for (const auto& s : xrd->streams) {
                const auto& k = s.first;
                std::erase(waitings[k], waiter);
                if (waitings[k].empty()) {
                    waitings.erase(k);
                }
            }
            waiter->cmd = std::nullopt;
            return {};
        }
        else {
            return std::unexpected("Unexpected clear blocking cmd");
        }
    }
    return {};
}

void handle_waitings(Storage& storage, Waitings& waitings, const StorageKey& key) {
    while(waitings.contains(key) && storage.values.contains(key)) {
        assert(!waitings[key].empty());

        auto waiter = waitings[key].front();
        waitings[key].pop_back();
        if (waitings[key].empty()) {
            waitings.erase(key);
        }

        assert(waiter->cmd != std::nullopt && "Expected blocking command");

        if (std::holds_alternative<BlpopCommand>(*waiter->cmd)) {
            auto res = lpop(storage, key, 1);
            if (res) {
                std::string msg = resp_array(std::vector<std::string>{
                    resp_bulk_string(key),
                    resp_bulk_string(res->front())
                });
                waiter->chan.try_send(std::error_code{}, msg);
            }
            else {
                waiter->chan.try_send(std::error_code{}, resp_storage_error(res.error()));
            }
        }
        else if (auto* xrd = std::get_if<XreadCommand>(&*waiter->cmd)) {
            auto result = xread(storage, *xrd);
            if (!result) {
                waiter->chan.try_send(std::error_code{}, resp_storage_error(result.error()));
            }
            else if (result->empty()) {
                waiter->chan.try_send(std::error_code{}, "*-1\r\n");
            }
            else {
                waiter->chan.try_send(std::error_code{}, format_xread_items(*result));
            }
        }
        else {
            assert(false && "Unexpected wait operation");
        }
    }
}

std::string handle_ping() {
    return resp_simple_string("PONG");
}

std::string handle_echo(const EchoCommand& cmd) {
    return resp_bulk_string(cmd.msg);
}

std::string handle_get(Storage& storage, const GetCommand& cmd) {
    auto val = dict_get(storage, cmd.key);
    if (val) {
        return resp_bulk_string(*val);
    }
    else {
        return resp_storage_error(val.error());
    }
}

std::string handle_set(Storage& storage, const SetCommand& cmd) {
    dict_set(storage, cmd);
    return resp_simple_string("OK");
}

std::string handle_rpush(Storage& storage, Waitings& waitings, const RpushCommand& cmd) {
    auto result = list_rpush(storage, cmd);
    if (!result) {
        return resp_storage_error(result.error());
    }

    handle_waitings(storage, waitings, cmd.listKey);

    return resp_integer(*result);
}

std::string handle_lpush(Storage& storage, Waitings& waitings, const LpushCommand& cmd) {
    auto result = list_lpush(storage, cmd);
    if (result) {
        handle_waitings(storage, waitings, cmd.listKey);
        return resp_integer(*result);
    }
    else {
        return resp_storage_error(result.error());
    }
}

asio::awaitable<std::string> handle_blpop(
    Storage& storage,
    Waitings& waitings,
    std::shared_ptr<Waiter> waiter,
    const BlpopCommand& cmd
) {
    auto result = blpop(storage, cmd);
    if (result) {
        std::string key = resp_bulk_string(result->first);
        std::string value = resp_bulk_string(result->second);
        co_return resp_array(std::array<std::string, 2>{key, value});
    }
    else if (result.error() == StorageError::NotFound) {
        auto ms = cmd.timeout ? *cmd.timeout : default_block_timeout;
        auto io = co_await asio::this_coro::executor;

        auto timer = asio::steady_timer(io, ms);

        if (auto err = add_waiter(waitings, waiter, cmd); !err) {
            co_return resp_simple_error(err.error());
        }

        auto [order, timer_ec, channel_ec, channel_res] = co_await asio::experimental::make_parallel_group(
            timer.async_wait(asio::deferred),
            waiter->chan.async_receive(asio::deferred)
        ).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);

        if (auto err = clear_waiter(waitings, waiter); !err) {
            co_return resp_simple_error(err.error());
        }

        if (order[0] == 0) { // timer
            co_return "*-1\r\n";
        }
        else if (!channel_ec) {
            co_return channel_res;
        }
        else {
            co_return "*-1\r\n";
        }
    }
    else {
        co_return resp_storage_error(result.error());
    }
}

std::string handle_type(Storage& storage, const TypeCommand& cmd) {
    auto result = key_type(storage, cmd.key);
    if (result) {
        switch (*result) {
        case StorageItemType::String:
            return resp_simple_string("string");

        case StorageItemType::List:
            return resp_simple_string("list");

        case StorageItemType::Stream:
            return resp_simple_string("stream");
        }
    }

    if (result.error() == StorageError::NotFound) {
        return resp_simple_string("none");
    }

    return resp_storage_error(result.error());

}

std::string handle_lrange(Storage& storage, const LrangeCommand& cmd) {
    auto result = list_lrange(storage, cmd);
    if (result) {
        std::vector<std::string> msgs;
        for (const std::string &arg : *result) {
            msgs.push_back(resp_bulk_string(arg));
        }
        return resp_array(msgs);
    }
    else if (result.error() == StorageError::NotFound) {
        return std::string("*0\r\n");
    }
    else {
        return resp_storage_error(result.error());
    }
}

std::string handle_llen(Storage& storage, const LlenCommand& cmd) {
    auto result = list_len(storage, cmd);
    if (result) {
        return resp_integer(*result);
    }
    else if (result.error() == StorageError::NotFound) {
        return resp_integer(0);
    }
    else {
        return resp_storage_error(result.error());
    }
}

std::string handle_lpop(Storage& storage, const LpopCommand& cmd) {
    auto result = list_lpop(storage, cmd);
    if (!result) {
        return resp_storage_error(result.error());
    }
    if (result->size() == 0) {
        return std::string("$-1\r\n");
    }
    else if (cmd.type == LpopType::Multiple) {
        std::vector<std::string> msgs;
        for (const std::string &arg : *result) {
            msgs.push_back(resp_bulk_string(arg));
        }
        return resp_array(msgs);
    }
    else if (cmd.type == LpopType::Single) {
        return resp_bulk_string(result->front());
    }
    else {
        std::unreachable();
    }
}

std::string handle_xadd(
    Storage& storage,
    Waitings& waitings,
    std::shared_ptr<Waiter> waiter,
    const XaddCommand& cmd
) {
    auto result = xadd(storage, cmd);
    if (result) {
        std::string msg = std::format("{}-{}", result->ms, result->seq);
        handle_waitings(storage, waitings, cmd.streamKey);
        return resp_bulk_string(std::move(msg));
    }
    else {
        return resp_storage_error(result.error());
    }
}

std::string handle_xrange(Storage& storage, const XrangeCommand& cmd) {
    auto result = xrange(storage, cmd);
    if (!result) {
        return resp_storage_error(result.error());
    }
    else {
        return format_stream_entries(*result);
    }
}

asio::awaitable<std::string> handle_xread(
    Storage& storage,
    Waitings& waitings,
    std::shared_ptr<Waiter> waiter,
    const XreadCommand& cmd
) {
    auto result = xread(storage, cmd);
    if (!result) {
        co_return resp_storage_error(result.error());
    }
    else if (result->empty()) {
        if (cmd.timeout) {
            auto io = co_await asio::this_coro::executor;
            auto timer = asio::steady_timer(io, cmd.timeout->count() == 0 ? default_block_timeout : *cmd.timeout);

            if (auto err = add_waiter(waitings, waiter, cmd); !err) {
                co_return resp_simple_error(err.error());
            }

            auto [order, timer_ec, channel_ec, channel_res] = co_await asio::experimental::make_parallel_group(
                timer.async_wait(asio::deferred),
                waiter->chan.async_receive(asio::deferred)
            ).async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);

            if (auto err = clear_waiter(waitings, waiter); !err) {
                co_return resp_simple_error(err.error());
            }

            if (order[0] == 0) { // timer
                co_return "*-1\r\n";
            }
            else if (!channel_ec) {
                co_return channel_res;
            }
            else {
                co_return "*-1\r\n";
            }

            assert(false && "Not implemented");
            // add_stream_waiting_op(server, client, cmd);
        }
        else {
            co_return "*-1\r\n";
        }
    }
    else {
        co_return format_xread_items(*result);
    }
}

asio::awaitable<void> read_loop(Storage& storage, Waitings& waitings, tcp::socket socket) {
    Parser parser{};

    auto io = co_await asio::this_coro::executor;

    auto waiter = std::make_shared<Waiter>(
        std::nullopt,
        channel<void(std::error_code, std::string)>(io, 0)
    );

    try {
        while(true) {
            if (auto err = ensure_buf_cap(parser, read_buf_size); !err) {
                auto msg = resp_simple_error(err.error());
                co_await asio::async_write(socket, asio::buffer(msg), asio::use_awaitable);
                break;
            }

            char* buf_begin = parser.data.get() + parser.end;
            size_t bytes_read = co_await socket.async_read_some(asio::buffer(buf_begin, read_buf_size), asio::use_awaitable);
            parser.end += bytes_read;

            while(auto cmd = process_input(parser)) {
                std::string resp;
                if (std::holds_alternative<PingCommand>(*cmd)) {
                    resp = handle_ping();
                }
                else if (auto* echo = std::get_if<EchoCommand>(&*cmd)) {
                    resp = handle_echo(*echo);
                }
                else if (auto* get = std::get_if<GetCommand>(&*cmd)) {
                    resp = handle_get(storage, *get);
                }
                else if (auto* set = std::get_if<SetCommand>(&*cmd)) {
                    resp = handle_set(storage, *set);
                }
                else if (auto* rpush = std::get_if<RpushCommand>(&*cmd)) {
                    resp = handle_rpush(storage, waitings, *rpush);
                }
                else if (auto* lpush = std::get_if<LpushCommand>(&*cmd)) {
                    resp = handle_lpush(storage, waitings, *lpush);
                }
                else if (auto* lrange = std::get_if<LrangeCommand>(&*cmd)) {
                    resp = handle_lrange(storage, *lrange);
                }
                else if (auto* llen = std::get_if<LlenCommand>(&*cmd)) {
                    resp = handle_llen(storage, *llen);
                }
                else if (auto* lpop = std::get_if<LpopCommand>(&*cmd)) {
                    resp = handle_lpop(storage, *lpop);
                }
                else if (auto* blp = std::get_if<BlpopCommand>(&*cmd)) {
                    resp = co_await handle_blpop(storage, waitings, waiter, *blp);
                }
                else if (auto* type = std::get_if<TypeCommand>(&*cmd)) {
                    resp = handle_type(storage, *type);
                }
                else if (auto* xadd = std::get_if<XaddCommand>(&*cmd)) {
                    resp = handle_xadd(storage, waitings, waiter, *xadd);
                }
                else if (auto* xrange = std::get_if<XrangeCommand>(&*cmd)) {
                    resp = handle_xrange(storage, *xrange);
                }
                else if (auto* xread = std::get_if<XreadCommand>(&*cmd)) {
                    resp = co_await handle_xread(storage, waitings, waiter, *xread);
                }
                else if (auto* err = std::get_if<InvalidCommand>(&*cmd)) {
                    resp = resp_simple_error(err->msg);
                }
                else {
                    resp = resp_simple_error("Unsupported command");
                }

                co_await asio::async_write(socket, asio::buffer(resp), asio::use_awaitable);
            }
        }
    }
    catch (const asio::system_error& e) {
        if (e.code() == asio::error::eof) {
            std::println("Client disconnected");
        }
        else {
            std::println("Read socket failure: {}", e.code().message());
        }

        if (auto err = clear_waiter(waitings, waiter); !err) {
            std::println("Error: {}", err.error());
        }
    }
    catch (const std::exception& e) {
        std::println("Read failure: {}", e.what());
    }
    co_return;
}

asio::awaitable<void> accept_loop(tcp::acceptor&& acceptor) {
    auto io = acceptor.get_executor();

    Storage storage{};
    Waitings waitings;

    try {
        while(true) {
            auto socket = co_await acceptor.async_accept(io, asio::use_awaitable);
            asio::co_spawn(io, read_loop(storage, waitings, std::move(socket)), asio::detached);
        }
    }
    catch (const asio::system_error& e) {
        std::println("Accept error: {}", e.code().message());
    }
    catch (const std::exception& e) {
      std::println("Unexpected exception: {}", e.what());
    }

    co_return;
}

int main() {
    asio::io_context io;
    auto acceptor = tcp::acceptor(io, tcp::endpoint(tcp::v4(), port));

    asio::co_spawn(io, accept_loop(std::move(acceptor)), asio::detached);

    std::println("Server on port: {}", port);

    try {
        io.run();
    }
    catch (std::exception &e) {
        std::println("Server failure: {}", e.what());
        exit(1);
    }

    return 0;
}
