#include "resp.hpp"

Command build_command(RespMessage& resp_msg) {
    if (auto* resp_arr = std::get_if<RespArray>(&resp_msg)) {
        // TODO: validate arr size > 0
        if (auto* resp_str = std::get_if<RespString>(&resp_arr->front())) {
            std::string cmd = to_lower_case(*resp_str);
            if (cmd == "ping") {
                return PingCommand {};
            }
            else {
                return InvalidCommand {std::format("Unknown command: |{}|", cmd)};
            }
        }
        else {
            return InvalidCommand {"Unexpected resp msg type"};
        }
    }
    else {
        return InvalidCommand {std::format("Unexpected client message format. Array expected.")};
    }
}

std::string resp_simple_string(std::string_view msg) {
    return std::format("+{}\r\n", msg);
}

std::string resp_simple_error(std::string_view  msg) {
    return std::format("-ERR {}\r\n", msg);
}