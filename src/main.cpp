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
using Waitings = std::unordered_map<
    StorageKey,
    std::list<std::shared_ptr<channel<void(std::error_code, std::string)>>>
>;

constexpr int port = 6379;
constexpr size_t read_buf_size = 128;

std::string resp_storage_error(StorageError err) {
    switch (err) {
    case StorageError::NotFound:
        return "$-1\r\n";

    case StorageError::WrongType:
        return resp_simple_error("WRONGTYPE Operation against a key holding the wrong kind of value");

    case StorageError::StreamKeySmallerThanTop:
        return resp_simple_error("The ID specified in XADD is equal or smaller than the target stream top item");

    case StorageError::StreamKeySmallerThanZero:
        return resp_simple_error("The ID specified in XADD must be greater than 0-0");
    }

    std::unreachable();
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

void handle_blpop_watings(Storage& storage, Waitings& waitings, const std::string list_key) {
    while(waitings.contains(list_key) && storage.values.contains(list_key)) {
        auto channel = waitings[list_key].front();
        waitings[list_key].pop_back();

        auto res = lpop(storage, list_key, 1);
        if (!res) {
            channel->try_send(std::error_code{}, resp_storage_error(res.error()));
        }
        if (res) {
            std::string msg = resp_array(std::vector<std::string>{
                resp_bulk_string(list_key),
                resp_bulk_string(res->front())
            });
            auto success = channel->try_send(std::error_code{}, msg);
            std::println("channel: {}", success);
        }
    }
}

std::string handle_rpush(Storage& storage, Waitings& waitings, const RpushCommand& cmd) {
    auto result = list_rpush(storage, cmd);
    if (!result) {
        return resp_storage_error(result.error());
    }

    handle_blpop_watings(storage, waitings, cmd.listKey);

    return resp_integer(*result);
}

std::string handle_lpush(Storage& storage, Waitings& waitings, const LpushCommand& cmd) {
    auto result = list_lpush(storage, cmd);
    if (!result) {
        return resp_storage_error(result.error());
    }

    handle_blpop_watings(storage, waitings, cmd.listKey);

    return resp_integer(*result);
}

asio::awaitable<std::string> handle_blpop(
    Storage& storage,
    Waitings& waitings,
    std::shared_ptr<channel<void(std::error_code, std::string)>> ready_chan,
    const BlpopCommand& cmd) {
    auto result = blpop(storage, cmd);
    if (result) {
        std::string key = resp_bulk_string(result->first);
        std::string value = resp_bulk_string(result->second);
        co_return resp_array(std::array<std::string, 2>{key, value});
    }
    else if (result.error() == StorageError::NotFound) {
        // auto ms = b->timeout ? *b->timeout : std::chrono::milliseconds(9999);
        // auto io = co_await asio::this_coro::executor;

        // auto timer = asio::steady_timer(io, ms);
        // co_await timer.async_wait(asio::use_awaitable);

        // auto ready_chan = std::make_shared<channel<void(std::error_code, std::string)>>(io, 0);

        for (const auto& k : cmd.listKeys) {
            waitings[k].push_back(ready_chan);
        }

        auto msg = co_await ready_chan->async_receive(asio::use_awaitable);

        for (const auto& k : cmd.listKeys) {
            std::erase(waitings[k], ready_chan);
            if (waitings.contains(k) && waitings[k].empty()) {
                waitings.erase(k);
            }
        }

        ready_chan->reset();

        co_return msg;
    }
    else {
        co_return resp_storage_error(result.error());
    }
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

asio::awaitable<void> read_loop(Storage& storage, Waitings& waitings, tcp::socket socket) {
    Parser parser{};

    auto io = co_await asio::this_coro::executor;
    auto ready_chan = std::make_shared<channel<void(std::error_code, std::string)>>(io, 0);

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
                    resp = co_await handle_blpop(storage, waitings, ready_chan, *blp);
                }
                else if (auto* err = std::get_if<InvalidCommand>(&*cmd)) {
                    resp = resp_simple_error(err->msg);
                }
                else {
                    std::unreachable();
                    exit(1);
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

        for (auto& [k, v] : waitings) {
            std::erase(waitings[k], ready_chan);
            if (waitings.contains(k) && waitings[k].empty()) {
                waitings.erase(k);
            }
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
