#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>
#include <expected>
#include <variant>

#include "resp.hpp"
#include "parser.hpp"
#include "storage.hpp"

using asio::ip::tcp;

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

asio::awaitable<void> read_loop(Storage& storage, tcp::socket socket) {
    Parser parser{};

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
                std::string msg;
                if (std::holds_alternative<PingCommand>(*cmd)) {
                    msg = resp_simple_string("PONG");
                }
                else if (auto* echo = std::get_if<EchoCommand>(&*cmd)) {
                    msg = resp_bulk_string(echo->msg);
                }
                else if (auto* get = std::get_if<GetCommand>(&*cmd)) {
                    auto val = dict_get(storage, get->key);
                    if (val) {
                        msg = resp_bulk_string(*val);
                    }
                    else {
                        msg = resp_storage_error(val.error());
                    }
                }
                else if (auto* set = std::get_if<SetCommand>(&*cmd)) {
                    dict_set(storage, *set);
                    msg = resp_simple_string("OK");
                }
                else if (auto* rpush = std::get_if<RpushCommand>(&*cmd)) {
                    auto result = list_rpush(storage, *rpush);
                    if (result) {
                        msg = resp_integer(*result);
                    }
                    else {
                        msg = resp_storage_error(result.error());
                    }
                }
                else if (auto* lpush = std::get_if<LpushCommand>(&*cmd)) {
                    auto result = list_lpush(storage, *lpush);

                    if (result) {
                        msg = resp_integer(*result);
                    }
                    else {
                        msg = resp_storage_error(result.error());
                    }
                }
                else if (auto* lrange = std::get_if<LrangeCommand>(&*cmd)) {
                    auto result = list_lrange(storage, *lrange);
                    if (result) {
                        std::vector<std::string> msgs;
                        for (const std::string &arg : *result) {
                            msgs.push_back(resp_bulk_string(arg));
                        }
                        msg = resp_array(msgs);
                    }
                    else if (result.error() == StorageError::NotFound) {
                        msg = std::string("*0\r\n");
                    }
                    else {
                        msg = resp_storage_error(result.error());
                    }
                }
                else if (auto* llen = std::get_if<LlenCommand>(&*cmd)) {
                    auto result = list_len(storage, *llen);
                    if (result) {
                        msg = resp_integer(*result);
                    }
                    else if (result.error() == StorageError::NotFound) {
                        msg = resp_integer(0);
                    }
                    else {
                        msg = resp_storage_error(result.error());
                    }
                }
                else if (auto* lpop = std::get_if<LpopCommand>(&*cmd)) {
                    auto result = list_lpop(storage, *lpop);
                    if (!result) {
                        msg = resp_storage_error(result.error());
                    }
                    if (result->size() == 0) {
                        msg = std::string("$-1\r\n");
                    }
                    else if (lpop->type == LpopType::Multiple) {
                        std::vector<std::string> msgs;
                        for (const std::string &arg : *result) {
                            msgs.push_back(resp_bulk_string(arg));
                        }
                        msg = resp_array(msgs);
                    }
                    else if (lpop->type == LpopType::Single) {
                        msg = resp_bulk_string(result->front());
                    }
                    else {
                        std::unreachable();
                    }
                }
                else if (auto* b = std::get_if<BlpopCommand>(&*cmd)) {
                    auto result = blpop(storage, *b);
                    if (result) {
                        std::string key = resp_bulk_string(result->first);
                        std::string value = resp_bulk_string(result->second);
                        msg = resp_array(std::array<std::string, 2>{key, value});
                    }
                    else if (result.error() == StorageError::NotFound) {
                        auto ms = b->timeout ? *b->timeout : std::chrono::milliseconds(9999);
                        auto io = co_await asio::this_coro::executor;

                        auto timer = asio::steady_timer(io, ms);
                        co_await timer.async_wait(asio::use_awaitable);

                        // somebody should close it if value found

                        std::println("Timeout");
                    }
                    else {
                        msg = resp_storage_error(result.error());
                    }
                }
                else if (auto* err = std::get_if<InvalidCommand>(&*cmd)) {
                    msg = resp_simple_error(err->msg);
                }
                else {
                    std::unreachable();
                    exit(1);
                }

                co_await asio::async_write(socket, asio::buffer(msg), asio::use_awaitable);
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
    }
    catch (const std::exception& e) {
        std::println("Read failure: {}", e.what());
    }
    co_return;
}

asio::awaitable<void> accept_loop(tcp::acceptor&& acceptor) {
    auto io = acceptor.get_executor();

    Storage storage{};

    try {
        while(true) {
            auto socket = co_await acceptor.async_accept(io, asio::use_awaitable);
            asio::co_spawn(io, read_loop(storage, std::move(socket)), asio::detached);
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
