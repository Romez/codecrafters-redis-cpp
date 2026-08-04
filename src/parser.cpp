#include "parser.hpp"

void ensure_buf_cap(ReadBuf& buf, size_t need) {
    assert(buf.pos <= buf.end);

    size_t free_bytes = buf.cap - buf.end;
    if (free_bytes >= need) return;

    // compact
    if (buf.pos > 0) {
        size_t len = buf.end - buf.pos;

        std::memmove(buf.data.get(), buf.data.get() + buf.pos, len);

        buf.pos = 0;
        buf.end = len;
    }

    free_bytes = buf.cap - buf.end;
    if (free_bytes < need) {
        // TODO: think how to handle that
        assert(buf.cap <= max_buf_cap);

        size_t next_cap = buf.cap + need;

        auto next_buf = std::make_unique<char[]>(next_cap);

        std::copy(buf.data.get(), buf.data.get() + buf.end, next_buf.get());

        buf.data = std::move(next_buf);
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
    char* cmd_begin = buf.data.get() + buf.pos;

    std::string resp_msg(cmd_begin, cmd_len);

    buf.pos = cmd_end + 2;

    return resp_msg;
}

// TODO: simplify return
std::expected<std::optional<size_t>, std::string> consume_number(ReadBuf& buf) {
    if (auto num_end = find_crlf(buf)) {
        size_t num;

        auto begin = buf.data.get() + buf.pos;
        auto end = buf.data.get() + *num_end;

        auto [ptr, ec] = std::from_chars(begin, end, num);
        if (ec != std::errc{} || ptr != end) {
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
                    buf.expected_str_len = *str_size;
                    buf.state = ParsingState::BulkString;
                }
                else {
                    // TODO: return error and close this client
                    std::println("Failed to parse bulk string size: {}", res.error());
                    exit(1);
                }
            }
        }
        else if (buf.state == ParsingState::BulkString) {
            size_t avail_size = buf.end - buf.pos;
            if (avail_size >= buf.expected_str_len + 2) {
                std::string msg = consume_msg(buf, buf.pos + buf.expected_str_len);
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
            // TODO: test on empty array
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