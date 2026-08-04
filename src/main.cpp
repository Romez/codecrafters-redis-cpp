#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>
#include <expected>

#include "resp.hpp"
#include "parser.hpp"
#include "utils/str.hpp"

using asio::ip::tcp;

constexpr int port = 6379;
constexpr size_t read_buf_size = 128;

asio::awaitable<void> read_loop(tcp::socket socket) {
    Parser buf{};

    size_t i = 0;
    try {
        while(true) {
            ensure_buf_cap(buf, read_buf_size);

            char* buf_begin = buf.data.get() + buf.end;
            size_t bytes_read = co_await socket.async_read_some(asio::buffer(buf_begin, read_buf_size), asio::use_awaitable);
            buf.end += bytes_read;

            while(auto resp_msg = process_input(buf)) {
                if (auto* resp_arr = std::get_if<RespArray>(&(*resp_msg))) {
                    // TODO: validate arr size > 0
                    if (auto* resp_str = std::get_if<RespString>(&resp_arr->front())) {
                        std::string cmd = to_lower_case(*resp_str);
                        if (cmd == "ping") {
                            auto msg = resp_simple_string("PONG");
                            co_await asio::async_write(socket, asio::buffer(msg), asio::use_awaitable);
                        }
                        else {
                            std::println("Unknown command: |{}|", cmd);
                            co_return;
                        }
                    }
                    else {
                        std::println("Unexpected resp msg type");
                        co_return;
                    }
                }
                else {
                    std::println("Unexpected client message format. Array expected.");
                    co_return;
                }
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
