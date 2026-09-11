#include <gtest/gtest.h>
#include <string>

#include <owl/ws/message.h>

TEST(WsMessage, OwnsPayloadAndOpcode) {
    const owl::ws::Message text{"hi", owl::ws::Opcode::Text};
    EXPECT_TRUE(text.is_text());
    EXPECT_FALSE(text.is_binary());
    EXPECT_EQ(text.size(), 2u);
    EXPECT_EQ(text.data(), "hi");

    const owl::ws::Message bin{std::string{'\0', '\1'}, owl::ws::Opcode::Binary};
    EXPECT_TRUE(bin.is_binary());
    EXPECT_EQ(bin.size(), 2u);
}
