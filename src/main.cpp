#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>

#include "resp.hpp"
#include "utils/str.hpp"

using asio::ip::tcp;

constexpr int port = 6379;
constexpr size_t read_buf_size = 8;
constexpr size_t max_buf_cap = 1024;

enum class ParsingState {
    Init,
    String
};

struct ReadBuf {
    char* data;
    size_t cap = 0;
    size_t pos = 0;
    size_t end = 0;

    size_t msg_pos = 0;

    ParsingState state;
};

void ensure_buf_cap(ReadBuf& buf, size_t need) {
    size_t free_bytes = buf.cap - buf.end;
    if (free_bytes >= need) return;

    // std::println("cap: {}, pos: {}, end: {}", buf.cap, buf.pos, buf.end);

    assert(buf.cap <= max_buf_cap);

    // compact
    if (buf.pos > 0) {
        size_t len = buf.end - buf.pos;
        std::memmove(buf.data, buf.data + buf.pos, len);

        buf.pos = 0;
        buf.end = len;
    }

    free_bytes = buf.cap - buf.end;
    if (free_bytes < need) {
        size_t next_cap = buf.cap + need;

        char* next_buf = new char[next_cap];

        std::memcpy(next_buf, buf.data, buf.end);
        
        delete buf.data;

        buf.data = next_buf;
        buf.cap = next_cap;
    }
}

std::optional<size_t> find_crlf(ReadBuf& buf) {
    for (size_t i = buf.pos + 1; i < buf.end; ++i) {
        if (buf.data[i-1] == '\r' && buf.data[i] == '\n') {
            return i - 1;
        }
    }
    return std::nullopt;
}

std::optional<char> consume_until_msg(ReadBuf& buf) {
    while(buf.pos < buf.end) {
        char c = buf.data[buf.pos++];
        if (c == '+') {
            return '+';
        }
    }
    return std::nullopt;
}

std::string consume_msg(ReadBuf& buf, size_t cmd_end) {
    size_t cmd_len = cmd_end - buf.pos;
    char* cmd_begin = buf.data + buf.pos;
    auto resp_msg = to_lower_case(std::string_view(cmd_begin, cmd_len));

    buf.state = ParsingState::Init;

    buf.pos = cmd_end + 2;

    return resp_msg;
}

std::optional<std::string> process_input(ReadBuf& buf) {
    while (buf.pos < buf.end) {
        if (buf.state == ParsingState::Init) {
            if (auto msg_type = consume_until_msg(buf)) {
                if (*msg_type == '+') {
                    buf.state = ParsingState::String;
                }
            }
        }
        else if (buf.state == ParsingState::String) {
            auto cmd_end = find_crlf(buf);

            if (cmd_end) {
                return consume_msg(buf, *cmd_end);
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

asio::awaitable<void> read_loop(tcp::socket socket) {
    ReadBuf buf{};

    size_t i = 0;
    try {
        while(true) {
            ensure_buf_cap(buf, read_buf_size);

            size_t bytes_read = co_await socket.async_read_some(asio::buffer(buf.data + buf.end, read_buf_size), asio::use_awaitable);
            buf.end += bytes_read;

            while(auto res = process_input(buf)) {
                if (*res == "ping") {
                    auto msg = resp_simple_string("PONG");
                    co_await asio::async_write(socket, asio::buffer(msg), asio::use_awaitable);
                }
                else {
                    std::println("Unknown command: {}", *res);
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
    // Flush after every std::cout / std::cerr
    // std::cout << std::unitbuf;
    // std::cerr << std::unitbuf;

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
