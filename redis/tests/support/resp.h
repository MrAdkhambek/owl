#pragma once

// RESP bytes in, reply out, through hiredis's own reader -- no socket -- so
// the reply tests and the fake server's scripts speak the same bytes. cmd()
// renders what a client sends for an argv, for expect{} steps.

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>

#include <hiredis/hiredis.h>

#include <redis/reply.h>

namespace redis_test {
    inline redis::reply parse(const std::string_view bytes) {
        redisReader* const reader = redisReaderCreate();
        redisReaderFeed(reader, bytes.data(), bytes.size());
        void* out = nullptr;
        const int rc = redisReaderGetReply(reader, &out);
        EXPECT_EQ(rc, REDIS_OK) << reader->errstr;
        EXPECT_NE(out, nullptr) << "incomplete RESP: " << bytes;
        redisReaderFree(reader);
        return redis::reply{static_cast<redisReply*>(out)};
    }

    inline std::string cmd(const std::initializer_list<std::string_view> argv) {
        std::string out = "*" + std::to_string(argv.size()) + "\r\n";
        for (const auto a : argv) {
            out += "$" + std::to_string(a.size()) + "\r\n";
            out += a;
            out += "\r\n";
        }
        return out;
    }
}
