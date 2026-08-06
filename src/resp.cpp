#include "resp.hpp"

std::string resp_simple_string(std::string_view msg) {
    return std::format("+{}\r\n", msg);
}

std::string resp_simple_error(std::string_view  msg) {
    return std::format("-ERR {}\r\n", msg);
}

std::string resp_bulk_string(std::string_view arg) {
    return std::format("${}\r\n{}\r\n", arg.size(), arg);
}

Command build_cmd(RespMessage& resp_msg) {
    if (auto* resp_arr = std::get_if<RespArray>(&resp_msg)) {
        if (resp_arr->size() == 0) {
            return InvalidCommand{"Invalid command"};
        }

        if (auto* resp_str = std::get_if<RespString>(&resp_arr->front())) {
            std::string cmd = to_lower_case(*resp_str);
            if (cmd == "ping") {
                return PingCommand {};
            }

            if (cmd == "echo") {
                auto args = std::span(*resp_arr).subspan(1);

                if (auto* msg = std::get_if<RespString>(&args.front())) {
                    return EchoCommand{*msg};
                }

                return InvalidCommand {"Invalid command"};
            }

            return InvalidCommand {std::format("Unknown command: |{}|", cmd)};
        }
    }

    return InvalidCommand {"Unexpected resp msg type"};
}