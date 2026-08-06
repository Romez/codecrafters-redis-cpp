#include "parser.hpp"

void ensure_buf_cap(Parser& buf, size_t need) {
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

std::optional<size_t> find_crlf(Parser& buf) {
    for (size_t i = buf.pos + 1; i < buf.end; ++i) {
        if (buf.data[i-1] == '\r' && buf.data[i] == '\n') {
            return i - 1;
        }
    }
    return std::nullopt;
}

std::optional<ParsingState> consume_msg_type(Parser& buf) {
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

std::string consume_msg(Parser& buf, size_t cmd_end) {
    size_t cmd_len = cmd_end - buf.pos;
    char* cmd_begin = buf.data.get() + buf.pos;

    std::string resp_msg(cmd_begin, cmd_len);

    buf.pos = cmd_end + 2;

    return resp_msg;
}

std::expected<size_t, std::string> consume_number(Parser& buf, size_t num_end) {
    size_t num;

    auto begin = buf.data.get() + buf.pos;
    auto end = buf.data.get() + num_end;

    auto [ptr, ec] = std::from_chars(begin, end, num);
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected("Invalid number");
    }

    buf.pos = num_end + 2;
    return num;
}

std::optional<RespMessage> append_to_frames(Parser& buf, RespMessage& msg) {
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

std::optional<Command> process_input(Parser& parser) {
    while (parser.pos < parser.end) {
        if (parser.state == ParsingState::Init) {
            if (auto next_state = consume_msg_type(parser)) {
                parser.state = *next_state;
            }
        }
        else if (parser.state == ParsingState::String) {
            if (auto cmd_end = find_crlf(parser)) {
                std::string msg = consume_msg(parser, *cmd_end);
                parser.state = ParsingState::Init;

                RespMessage respStr = RespString{std::move(msg)};
                if (auto next_msg = append_to_frames(parser, respStr)) {
                    return build_cmd(*next_msg);
                }
            }
            return std::nullopt;
        }
        else if (parser.state == ParsingState::BulkStringSize) {
            if (auto num_end = find_crlf(parser)) {
                if (auto res = consume_number(parser, *num_end)) {
                    parser.expected_str_len = *res;
                    parser.state = ParsingState::BulkString;
                } else {
                    return InvalidCommand{std::format("Failed to parse bulk string size: {}", res.error())};
                }
            }
        }
        else if (parser.state == ParsingState::BulkString) {
            size_t avail_size = parser.end - parser.pos;
            if (avail_size >= parser.expected_str_len + 2) {
                std::string msg = consume_msg(parser, parser.pos + parser.expected_str_len);
                RespMessage respStr = RespString{std::move(msg)};

                parser.state = ParsingState::Init;

                if (auto next_msg = append_to_frames(parser, respStr)) {
                    return build_cmd(*next_msg);
                }
            }
            else {
                return std::nullopt;
            }
        }
        else if (parser.state == ParsingState::ArraySize) {
            if (auto num_end = find_crlf(parser)) {
                if (auto res = consume_number(parser, *num_end)) {
                    RespArray arr;
                    arr.reserve(*res);
                    parser.frames.push_back(std::move(arr));
                    parser.state = ParsingState::Init;
                } else {
                    return InvalidCommand{std::format("Failed to parse array size: {}", res.error())};
                }
            }
        }
    }
    return std::nullopt;
}