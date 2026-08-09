#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>
#include <expected>

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

std::string handle_cmd(Storage& storage, Command& cmd) {
    if (std::holds_alternative<PingCommand>(cmd)) {
        return resp_simple_string("PONG");
    }
    else if (auto* echo = std::get_if<EchoCommand>(&cmd)) {
        return resp_bulk_string(echo->msg);
    }
    else if (auto* get = std::get_if<GetCommand>(&cmd)) {
        auto val = dict_get(storage, get->key);
        if (val) return resp_bulk_string(*val);
        else return resp_storage_error(val.error());
    }
    else if (auto* set = std::get_if<SetCommand>(&cmd)) {
        dict_set(storage, *set);
        return resp_simple_string("OK");
    }
    else if (auto* rpush = std::get_if<RpushCommand>(&cmd)) {
        auto result = list_rpush(storage, *rpush);
        if (result) {
            return resp_integer(*result);
        }
        else {
            return resp_storage_error(result.error());
        }
    }
    else if (auto* lrange = std::get_if<LrangeCommand>(&cmd)) {
        auto result = list_lrange(storage, *lrange);

        if (result) {
            std::vector<std::string> msgs;
            for (const std::string &arg : *result) {
                msgs.push_back(resp_bulk_string(arg));
            }
            return resp_array(msgs);
        }
        else if (result.error() == StorageError::NotFound) {
            return "*0\r\n";
        }
        else {
            return resp_storage_error(result.error());
        }
    }
    else if (auto* err = std::get_if<InvalidCommand>(&cmd)) {
        return resp_simple_error(err->msg);
    }
    std::unreachable();
    exit(1);
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
                auto msg = handle_cmd(storage, *cmd);
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
