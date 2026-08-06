#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>
#include <expected>

#include "resp.hpp"
#include "parser.hpp"

using asio::ip::tcp;

constexpr int port = 6379;
constexpr size_t read_buf_size = 128;

std::string handle_cmd(Command& cmd) {
    if (std::holds_alternative<PingCommand>(cmd)) {
        return resp_simple_string("PONG");
    }
    else if (auto* err = std::get_if<InvalidCommand>(&cmd)) {
        return resp_simple_error(err->msg);
    }
    std::unreachable();
}

asio::awaitable<void> read_loop(tcp::socket socket) {
    Parser buf{};

    size_t i = 0;
    try {
        while(true) {
            if (auto err = ensure_buf_cap(buf, read_buf_size); !err) {
                auto msg = resp_simple_error(err.error());
                co_await asio::async_write(socket, asio::buffer(msg), asio::use_awaitable);
                break;
            }

            char* buf_begin = buf.data.get() + buf.end;
            size_t bytes_read = co_await socket.async_read_some(asio::buffer(buf_begin, read_buf_size), asio::use_awaitable);
            buf.end += bytes_read;

            while(auto cmd = process_input(buf)) {
                auto msg = handle_cmd(*cmd);
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

    try {
        while(true) {
            auto socket = co_await acceptor.async_accept(io, asio::use_awaitable);
            asio::co_spawn(io, read_loop(std::move(socket)), asio::detached);
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
