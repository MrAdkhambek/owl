#include <gtest/gtest.h>

#include <stdexcept>
#include <string_view>

#include <redis/error.h>

TEST(Error, CarriesKindAndMessage) {
    const redis::error e{redis::error_kind::connection, "link died"};
    EXPECT_EQ(e.kind(), redis::error_kind::connection);
    EXPECT_STREQ(e.what(), "link died");
}

TEST(Error, ToStringNamesEveryKind) {
    using redis::error_kind;
    EXPECT_EQ(redis::to_string(error_kind::connect), "connect");
    EXPECT_EQ(redis::to_string(error_kind::connection), "connection");
    EXPECT_EQ(redis::to_string(error_kind::command), "command");
    EXPECT_EQ(redis::to_string(error_kind::timeout), "timeout");
    EXPECT_EQ(redis::to_string(error_kind::closed), "closed");
    EXPECT_EQ(redis::to_string(error_kind::conversion), "conversion");
}

TEST(Error, PrefixIsTheFirstWordOfACommandError) {
    const redis::error e{redis::error_kind::command, "WRONGTYPE Operation against a key holding the wrong kind of value"};
    EXPECT_EQ(e.prefix(), "WRONGTYPE");
    const redis::error one_word{redis::error_kind::command, "NOSCRIPT"};
    EXPECT_EQ(one_word.prefix(), "NOSCRIPT");
}

TEST(Error, PrefixIsEmptyForOtherKinds) {
    const redis::error e{redis::error_kind::connection, "ERR looks like one but is not"};
    EXPECT_EQ(e.prefix(), "");
}

TEST(Error, IsARuntimeError) {
    try {
        throw redis::error{redis::error_kind::closed, "client is closed"};
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "client is closed");
    }
}
