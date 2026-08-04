#pragma once

#include <cstring>
#include <cassert>
#include <optional>
#include <vector>
#include <expected>
#include <print>
#include <charconv>
#include <memory>

#include "resp.hpp"

constexpr size_t max_buf_cap = 1024;

enum class ParsingState {
    Init,
    String,
    BulkStringSize,
    BulkString,
    ArraySize
};

struct Parser {
    // char* data;
    std::unique_ptr<char[]> data;

    size_t cap = 0;
    size_t pos = 0;
    size_t end = 0;

    size_t expected_str_len = 0;

    ParsingState state;

    std::vector<RespArray> frames;
};

void ensure_buf_cap(Parser& buf, size_t need);
std::optional<RespMessage> process_input(Parser& buf);