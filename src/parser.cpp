#include "parser.hpp"

std::expected<void, std::string> ensure_buf_cap(Parser& buf, size_t need) {
    assert(buf.pos <= buf.end);

    size_t free_bytes = buf.cap - buf.end;
    if (free_bytes >= need) return {};

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
        if (buf.cap >= max_buf_cap) {
            return std::unexpected("Exceeded client buffer size");
        }

        size_t next_cap = buf.cap + need;

        auto next_buf = std::make_unique<char[]>(next_cap);

        std::copy(buf.data.get(), buf.data.get() + buf.end, next_buf.get());

        buf.data = std::move(next_buf);
        buf.cap = next_cap;
    }
    return {};
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

std::expected<size_t, std::string> parse_num(const char* begin, const char* end) {
    size_t num;
    auto [ptr, ec] = std::from_chars(begin, end, num);
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected("Invalid number");
    }
    return num;
}

std::expected<size_t, std::string> consume_number(Parser& parser, size_t num_end) {
    auto begin = parser.data.get() + parser.pos;
    auto end = parser.data.get() + num_end;

    auto num = parse_num(begin, end);
    if (num) {
        parser.pos = num_end + 2;
    }
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

Command build_echo(std::span<RespMessage> args) {
    if (args.size() != 1) {
        return InvalidCommand{"wrong number of arguments for 'echo' command"};
    }

    if (auto* msg = std::get_if<RespString>(&args.front())) {
        return EchoCommand{*msg};
    }

    return InvalidCommand{"Invalid resp command"};
}

Command build_get(std::span<RespMessage> args) {
    if (args.size() != 1) {
        return InvalidCommand{"wrong number of arguments for 'get' command"};
    }

    if (auto* msg = std::get_if<RespString>(&args.front())) {
        return GetCommand{*msg};
    }

    return InvalidCommand{"Invalid resp command"};
}

Command build_set(std::span<RespMessage> args) {
    if (args.size() < 2) {
        return InvalidCommand{"Invalid 'set' command"};
    }

    for (auto arg : args) {
        if (!std::holds_alternative<RespString>(arg)) {
            return InvalidCommand{"Invalid 'set' arg type"};
        }
    }

    SetCommand set{
        .key = std::get<RespString>(args[0]),
        .val = std::get<RespString>(args[1])
    };

    for(size_t i = 2; i < args.size();) {
        const auto& key = to_lower_case(std::get<RespString>(args[i]));

        if (key == "ex" || key == "px") {
            if (i + 1 >= args.size()) {
                return InvalidCommand{"ttl value missing"};
            }
            const auto& value = std::get<RespString>(args[i+1]);
            auto val = parse_num(value.data(), value.data() + value.size());
            // TODO: add ttl validation max size
            if (val) {
                set.ttl = std::chrono::milliseconds(to_lower_case(key) == "ex" ? *val * 1000 : *val);
                i += 2;
            } else {
                return InvalidCommand{val.error()};
            }
        } else {
            return InvalidCommand{"Unexpected 'set' option"};
        }
    }

    return set;
}

Command build_rpush(std::span<RespMessage> args) {
    if (args.size() < 2) {
        return InvalidCommand{"wrong number of arguments for 'rpush' command"};
    }

    std::string listKey;

    if (auto* key = std::get_if<RespString>(&args[0])) {
        listKey = *key;
    }
    else {
        return InvalidCommand{"wrong 'rpush' key type"};
    }

    std::vector<std::string> values;

    for (size_t i = 1; i < args.size(); ++i) {
        if (auto* val = std::get_if<RespString>(&args[i])) {
            values.push_back(*val);
        }
        else {
            return InvalidCommand{"wrong 'rpush' arg type"};
        }
    }

    return RpushCommand{ listKey, std::move(values) };
}

Command build_lpush(std::span<RespMessage> args) {
    if (args.size() < 2) {
        return InvalidCommand{"wrong number of arguments for 'lpush' command"};
    }

    std::string listKey;
    if (auto* key = std::get_if<RespString>(&args[0])) {
        listKey = *key;
    }
    else {
        return InvalidCommand("wrong 'lpush' list key type");
    }

    std::vector<std::string> values;

    for (size_t i = 1; i < args.size(); ++i) {
        if (auto* val = std::get_if<RespString>(&args[i])) {
            values.push_back(*val);
        }
        else {
            return InvalidCommand{"wrong 'lpush' arg type"};
        }
    }

    return LpushCommand{ listKey, std::move(values) };
}

Command build_lrange(std::span<RespMessage> args) {
    if (args.size() != 3) {
        return InvalidCommand{"wrong number of arguments for 'lrange' command"};
    }

    std::string listKey;
    std::string startArg;
    std::string stopArg;

    if (auto* key = std::get_if<RespString>(&args[0])) {
        listKey = *key;
    }
    else {
        return InvalidCommand{"wrong 'lrange' key type"};
    }

    if (auto* arg = std::get_if<RespString>(&args[1])) {
        startArg = *arg;
    }
    else {
        return InvalidCommand{"wrong 'lrange' start arg type"};
    }

    if (auto* arg = std::get_if<RespString>(&args[2])) {
        stopArg = *arg;
    }
    else {
        return InvalidCommand{"wrong 'lrange' stop arg type"};
    }

    int start = 0;
    auto result = std::from_chars(startArg.data(), startArg.data() + startArg.size(), start);
    if (result.ec != std::errc{}) {
        return InvalidCommand{"value is not an integer or out of range"};
    }

    int stop = 0;
    result = std::from_chars(stopArg.data(), stopArg.data() + stopArg.size(), stop);
    if (result.ec != std::errc{}) {
        return InvalidCommand{"value is not an integer or out of range"};
    }

    return LrangeCommand{
        .listKey = listKey,
        .start = start,
        .stop = stop,
    };
}

Command build_llen(std::span<RespMessage> args) {
    if (args.size() != 1) {
        return InvalidCommand{"wrong number of arguments for 'llen' command"};
    }

    if (auto* key = std::get_if<RespString>(&args[0])) {
        return LlenCommand{ *key };
    }
    else {
        return InvalidCommand{"wrong 'llen' key type"};
    }
}

Command build_lpop(std::span<RespMessage> args) {
    if (args.size() == 1) {
        std::string listKey;
        if (auto* key = std::get_if<RespString>(&args[0])) {
            listKey = *key;
        }
        else {
            return InvalidCommand{"wrong 'lpop' key type"};
        }

        return LpopCommand{ listKey, 1, LpopType::Single };
    }
    else if (args.size() == 2) {
        std::string listKey;
        if (auto* key = std::get_if<RespString>(&args[0])) {
            listKey = *key;
        }
        else {
            return InvalidCommand{"wrong 'lpop' key type"};
        }

        std::string lenKey;
        if (auto* len = std::get_if<RespString>(&args[1])) {
            lenKey = *len;
        }
        else {
            return InvalidCommand{"wrong 'lpop' len type"};
        }

        size_t len = 0;
        auto result = std::from_chars(lenKey.data(), lenKey.data() + lenKey.size(), len);
        if (result.ec != std::errc{}) {
            return InvalidCommand{"value is not an integer or out of range"};
        }

        return LpopCommand{ listKey, len, LpopType::Multiple };
    }
    else {
        return InvalidCommand{"wrong number of arguments for 'lpop' command"};
    }
}

Command build_blpop(std::span<RespMessage> args) {
    if (args.size() < 2) {
        return InvalidCommand{"wrong number of arguments for 'blpop' command"};
    }

    for (auto arg : args) {
        if (!std::holds_alternative<RespString>(arg)) {
            return InvalidCommand{"Invalid 'blpop' arg type"};
        }
    }

    std::string lenKey = std::get<RespString>(args.back());

    double timeoutSec = 0;

    auto lenBegin = lenKey.data();
    auto lenEnd = lenKey.data() + lenKey.size();
    auto [ptr, ec] = std::from_chars(lenBegin, lenEnd, timeoutSec);
    if (ec != std::errc{} || ptr != lenEnd) {
        return InvalidCommand("value is not an integer or out of range");
    }

    if (timeoutSec < 0) {
        return InvalidCommand("timeout is negative");
    }

    BlpopCommand cmd{};

    for (size_t i = 0; i < args.size() - 1; ++i) {
        auto s = std::get<RespString>(args[i]);
        cmd.listKeys.push_back(s);
    }

    if (timeoutSec > 0) {
        cmd.timeout = std::chrono::milliseconds((long)(timeoutSec * 1000));
    }

    return cmd;
}

Command build_type(std::span<RespMessage> args) {
    if (args.size() != 1) return InvalidCommand{"Invalid 'type' args size"};

    if (auto* s = std::get_if<RespString>(&args[0])) {
        return TypeCommand{.key = *s};
    }
    else {
        return InvalidCommand{"Invalid 'type' key type"};
    }
}

std::expected<RespStreamId, InvalidCommand> parse_stream_id(const std::string& value) {
    if (value == "*") {
        return RespStreamSpecialId::Auto;
    }

    if (value == "-") {
        return RespStreamSpecialId::Min;
    }

    if (value == "+") {
        return RespStreamSpecialId::Max;
    }

    size_t pos = value.find('-');

    if (pos == std::string::npos) {
        return std::unexpected(InvalidCommand{"invalid id format"});
    }

    std::string msValue = value.substr(0, pos);
    std::string seqValue = value.substr(pos + 1);

    uint64_t ms;
    auto msResult = std::from_chars(msValue.data(), msValue.data() + msValue.size(), ms);
    if (msResult.ec != std::errc{}) {
        return std::unexpected(InvalidCommand{"invalid ms value"});
    }

    if (seqValue == "*") {
        return RespStreamMsId{ ms };
    }

    uint64_t seq;
    auto seqResult = std::from_chars(seqValue.data(), seqValue.data() + seqValue.size(), seq);
    if (seqResult.ec != std::errc{}) {
        return std::unexpected(InvalidCommand{"invalid seq value"});
    }

    return RespStreamMsSeqId{ ms, seq };
}

Command build_xadd(std::span<RespMessage> args) {
    if (args.size() < 4 || (args.size() % 2) != 0) {
        return InvalidCommand{"wrong number of arguments for 'xadd' command"};
    }

    std::string streamKey;
    if (auto* key = std::get_if<RespString>(&args[0])) {
        streamKey = *key;
    }
    else {
        return InvalidCommand{"wrong 'xadd' key type"};
    }

    RespStreamId id;
    if (auto* val = std::get_if<RespString>(&args[1])) {
        auto result = parse_stream_id(*val);
        if (!result) return result.error();
        id = *result;
    }
    else {
        return InvalidCommand{"wrong 'xadd' id type"};
    }

    std::vector<std::pair<std::string, std::string>> kvPairs;

    for (size_t i = 2; i < args.size(); i += 2) {
        std::string keyItem;
        if (auto* key = std::get_if<RespString>(&args[i])) {
            keyItem = *key;
        }
        else {
            return InvalidCommand{"wrong 'xadd' key type"};
        }

        std::string valItem;
        if (auto* val = std::get_if<RespString>(&args[i + 1])) {
            valItem = *val;
        }
        else {
            return InvalidCommand{"wrong 'xadd' val type"};
        }

        kvPairs.push_back({ keyItem, valItem });
    }

    return XaddCommand{
        .streamKey = streamKey,
        .id = id,
        .kvPairs = kvPairs,
    };
}

std::optional<InvalidCommand> validate_xrange_args(const std::span<RespMessage>& args) {
    if (args.size() != 3) {
        return InvalidCommand{"wrong number of arguments for 'xrange' command"};
    }

    if (!std::holds_alternative<RespString>(args[0])) {
        return InvalidCommand{"stream key should be a String type"};
    }

    if (!std::holds_alternative<RespString>(args[1]) || !std::holds_alternative<RespString>(args[2])) {
        return InvalidCommand{"stream id should be a String type"};
    }

    return std::nullopt;
}

Command build_xrange(const std::span<RespMessage>& args) {
    if (auto error = validate_xrange_args(args)) return *error;

    std::string streamKey;
    if (auto* key = std::get_if<RespString>(&args[0])) {
        streamKey = *key;
    }
    else {
        return InvalidCommand{"wrong 'xrange' key type"};
    }

    RespStreamId start;
    if (auto* val = std::get_if<RespString>(&args[1])) {
        auto result = parse_stream_id(*val);
        if (!result) return result.error();
        start = *result;
    }
    else {
        return InvalidCommand{"wrong 'xrange' start type"};
    }

    RespStreamId stop;
    if (auto* val = std::get_if<RespString>(&args[2])) {
        auto result = parse_stream_id(*val);
        if (!result) return result.error();
        stop = *result;
    }
    else {
        return InvalidCommand{"wrong 'xrange' stop type"};
    }

    return XrangeCommand{ streamKey, start, stop };
}

Command build_xread(const std::span<RespMessage>& args) {
    if (args.size() < 3) {
        return InvalidCommand{"wrong number of steams args"};
    }

    XreadCommand cmd{};

    std::string key;
    size_t index = 0;

    if (auto* val = std::get_if<RespString>(&args[index])) key = *val;
    else return InvalidCommand{"wrong 'xread' arg type"};

    if (key == "block") {
        if (auto* val = std::get_if<RespString>(&args[index+1])) {
            long ms;
            auto res = std::from_chars(val->data(), val->data() + val->size(), ms);
            if (res.ec != std::errc{}) {
                return InvalidCommand{"invalid ms value"};
            }
            if (ms < 0) {
                return InvalidCommand{"invalid block timeout"};
            }
            cmd.timoutMs = ms;
        }
        else {
            return InvalidCommand{"wrong 'xread' ms type"};
        }

        index += 2;
        if (auto* val = std::get_if<RespString>(&args[index])) key = *val;
        else return InvalidCommand{"wrong 'xread' arg type"};
    }

    if (key == "streams") {
        std::span<const RespMessage> streams = std::span(args).subspan(index + 1);

        if (streams.size() % 2 != 0) {
            return InvalidCommand{"wrong number of steams args"};
        }

        size_t idsStart = streams.size() / 2;
        for (size_t i = 0; i < idsStart; ++i) {
            std::string streamKey;
            if (auto* key = std::get_if<RespString>(&streams[i]))
                streamKey = *key;
            else
                return InvalidCommand{"wrong 'xread' arg type"};

            RespStreamId id;
            if (auto* val = std::get_if<RespString>(&streams[i + idsStart])) {
                auto result = parse_stream_id(*val);
                if (!result) return result.error();
                id = *result;
            }
            else {
                return InvalidCommand{"wrong 'xread' arg type"};
            }

            cmd.streams.push_back(std::pair{ streamKey, id });
        }

        return cmd;
    }

    return InvalidCommand{"wrong xread command"};
}

// void print_reps(RespMessage& resp_msg) {
//     if (auto* arr = std::get_if<RespArray>(&resp_msg)) {
//         std::println("ARR: [");
//         for (auto item : *arr) {
//             print_reps(item);
//         }
//         std::println("]");
//     }
//     else if (auto* str = std::get_if<RespString>(&resp_msg)) {
//         std::println("STR: {}", *str);
//     }
//     else {
//         assert(false && "Unexpected resp value");
//     }
// }

// std::string cmd_type_str(Command& cmd) {
//     if (std::holds_alternative<PingCommand>(cmd)) return "PingCommand";
//     if (std::holds_alternative<EchoCommand>(cmd)) return "EchoCommand";
//     if (std::holds_alternative<GetCommand>(cmd)) return "GetCommand";
//     if (std::holds_alternative<SetCommand>(cmd)) return "SetCommand";
//     if (std::holds_alternative<InvalidCommand>(cmd)) return "InvalidCommand";
//     else {
//         assert(false && "Unexpected command type");
//     }
// }

Command resp_msg_to_cmd(RespMessage& resp_msg) {
    if (auto* resp_arr = std::get_if<RespArray>(&resp_msg)) {
        if (resp_arr->size() == 0) {
            return InvalidCommand{"Invalid command"};
        }

        if (auto* cmd_str = std::get_if<RespString>(&resp_arr->front())) {
            auto args = std::span(*resp_arr).subspan(1);
            std::string cmd = to_lower_case(*cmd_str);
            if (cmd == "ping")      return PingCommand {};
            else if (cmd == "echo") return build_echo(args);
            else if (cmd == "get")  return build_get(args);
            else if (cmd == "set")  return build_set(args);
            else if (cmd == "rpush") return build_rpush(args);
            else if (cmd == "lrange") return build_lrange(args);
            else if (cmd == "lpush") return build_lpush(args);
            else if (cmd == "llen") return build_llen(args);
            else if (cmd == "lpop") return build_lpop(args);
            else if (cmd == "blpop") return build_blpop(args);
            else if (cmd == "type") return build_type(args);
            else if (cmd == "xadd") return build_xadd(args);
            else if (cmd == "xrange") return build_xrange(args);
            else if (cmd == "xread") return build_xread(args);
            else {
                return InvalidCommand {std::format("Unknown command: |{}|", cmd)};
            }
        }
    }

    return InvalidCommand {"Invalid resp command"};
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
                    return resp_msg_to_cmd(*next_msg);
                }
            }
            return std::nullopt;
        }
        else if (parser.state == ParsingState::BulkStringSize) {
            if (auto num_end = find_crlf(parser)) {
                if (auto res = consume_number(parser, *num_end)) {
                    parser.expected_str_len = *res;
                    parser.state = ParsingState::BulkString;
                    continue;
                } else {
                    return InvalidCommand{std::format("Failed to parse bulk string size: {}", res.error())};
                }
            }

            return std::nullopt;
        }
        else if (parser.state == ParsingState::BulkString) {
            size_t avail_size = parser.end - parser.pos;
            if (avail_size >= parser.expected_str_len + 2) {
                std::string msg = consume_msg(parser, parser.pos + parser.expected_str_len);
                RespMessage respStr = RespString{std::move(msg)};

                parser.state = ParsingState::Init;

                if (auto next_msg = append_to_frames(parser, respStr)) {
                    return resp_msg_to_cmd(*next_msg);
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
                    continue;
                } else {
                    return InvalidCommand{std::format("Failed to parse array size: {}", res.error())};
                }
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}
