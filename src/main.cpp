#include <iostream>
#include <print>
#include <array>
#include <span>
#include <cstring>
#include <asio.hpp>
#include <expected>
#include <charconv>

#include "resp.hpp"
#include "utils/str.hpp"

using asio::ip::tcp;

constexpr int port = 6379;
constexpr size_t read_buf_size = 8;
constexpr size_t max_buf_cap = 1024;

enum class ParsingState {
    Init,
    String,
    BulkStringSize,
    BulkString,
    ArraySize
};

struct ReadBuf {
    char* data;
    size_t cap = 0;
    size_t pos = 0;
    size_t end = 0;

    size_t expected_msg_size = 0;

    ParsingState state;

    std::vector<RespArray> frames;
};

void ensure_buf_cap(ReadBuf& buf, size_t need) {
    size_t free_bytes = buf.cap - buf.end;
    if (free_bytes >= need) return;

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

std::optional<ParsingState> consume_msg_type(ReadBuf& buf) {
    while(buf.pos < buf.end) {
        switch (buf.data[buf.pos++]) {
          case '+': return ParsingState::String;
          case '$': return ParsingState::BulkStringSize;
          case '*': return ParsingState::ArraySize;
          default: continue;
        }
    }
    return std::nullopt;
}

std::string consume_msg(ReadBuf& buf, size_t cmd_end) {
    size_t cmd_len = cmd_end - buf.pos;
    char* cmd_begin = buf.data + buf.pos;

    std::string resp_msg(cmd_begin, cmd_len);

    buf.pos = cmd_end + 2;

    return resp_msg;
}

std::expected<std::optional<size_t>, std::string> consume_number(ReadBuf& buf) {
    if (auto num_end = find_crlf(buf)) {
        size_t num;

        auto [ptr, ec] = std::from_chars(buf.data + buf.pos, buf.data + *num_end, num);
        if (ec != std::errc{}) {
            return std::unexpected("Invalid number");
        }

        buf.pos = *num_end + 2;
        return num;
    }
    return std::nullopt;
}

std::optional<RespMessage> append_to_frames(ReadBuf& buf, RespMessage& msg) {
    while (true) {
        if (buf.frames.empty()) {
            return msg;
        }

        auto& arr = buf.frames.back();
        arr.push_back(msg);

        if (arr.size() == arr.capacity()) {
            msg = arr;
            buf.frames.pop_back();
        }
        else {
            return std::nullopt;
        }
    }
}

std::optional<RespMessage> process_input(ReadBuf& buf) {
    while (buf.pos < buf.end) {
        if (buf.state == ParsingState::Init) {
            if (auto next_state = consume_msg_type(buf)) {
                buf.state = *next_state;
            }
        }
        else if (buf.state == ParsingState::String) {
            if (auto cmd_end = find_crlf(buf)) {
                std::string msg = consume_msg(buf, *cmd_end);
                buf.state = ParsingState::Init;
                return RespString{std::move(msg)};
            }
            return std::nullopt;
        }
        else if (buf.state == ParsingState::BulkStringSize) {
            if (auto res = consume_number(buf)) {
                if (auto str_size = *res) {
                    buf.expected_msg_size = *str_size;
                    buf.state = ParsingState::BulkString;
                }
                else {
                    std::println("Failed to parse bulk string size: {}", res.error());
                    exit(1);
                }
            }
        }
        else if (buf.state == ParsingState::BulkString) {
            size_t avail_size = buf.end - buf.pos;
            if (buf.expected_msg_size <= avail_size) {
                std::string msg = consume_msg(buf, buf.pos + buf.expected_msg_size);
                RespMessage respStr = RespString{std::move(msg)};

                buf.state = ParsingState::Init;

                if (auto next_msg = append_to_frames(buf, respStr)) {
                    return *next_msg;
                }
            }
            else {
                return std::nullopt;
            }
        }
        else if (buf.state == ParsingState::ArraySize) {
            if (auto res = consume_number(buf)) {
                if (auto arr_size = *res) {
                    RespArray arr;
                    arr.reserve(*arr_size);
                    buf.frames.push_back(std::move(arr));
                    buf.state = ParsingState::Init;
                }
            }
            else {
                std::println("Failed to parse array size: {}", res.error());
                exit(1);
            }
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

            while(auto resp_msg = process_input(buf)) {
                if (auto* resp_str = std::get_if<RespString>(&(*resp_msg))) {
                    std::println("RESP STR: {}", *resp_str);
                    assert(false && "Unexpected resp string");
                }
                else if (auto* resp_arr = std::get_if<RespArray>(&(*resp_msg))) {
                    if (auto* resp_str = std::get_if<RespString>(&resp_arr->front())) {
                        std::string cmd = to_lower_case(*resp_str);
                        if (cmd == "ping") {
                            auto msg = resp_simple_string("PONG");
                            co_await asio::async_write(socket, asio::buffer(msg), asio::use_awaitable);
                        }
                        else {
                            std::println("Unknown command: {}", cmd);
                            exit(1);
                        }
                    }
                    else {
                        assert(false && "Unexpected resp msg type");
                    }
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
