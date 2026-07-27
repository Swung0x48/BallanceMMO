// Unit tests for the BallanceMMO message layer: string/variable IO helpers,
// size-checked message views, and (de)serialization round-trips including
// truncated / forged-length payloads a malicious peer could send.
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <message/message.hpp>
#include <message/message_utils.hpp>
#include <message/chat_msg.hpp>
#include <message/login_request_v3_msg.hpp>
#include <message/countdown_msg.hpp>
#include <message/player_disconnected_msg.hpp>

namespace {

// SteamNetworkingMessage_t has a protected destructor (the real library
// manages instances on the heap); for view_as tests we only need m_pData
// and m_cbSize, so a trivial subclass lets us stack-allocate one.
struct test_networking_message: SteamNetworkingMessage_t {
    test_networking_message(void* data, int size) {
        m_pData = data;
        m_cbSize = size;
    }
};

std::string stream_payload(std::stringstream& stream) {
    return stream.str();
}

} // namespace

TEST(ReadWriteString, RoundTripDefaultLengthType) {
    std::stringstream stream;
    const std::string original = "hello, BallanceMMO! \xE7\x90\x83"; // includes UTF-8 bytes
    bmmo::message_utils::write_string(original, stream);

    std::string result;
    EXPECT_TRUE(bmmo::message_utils::read_string(stream, result));
    EXPECT_EQ(original, result);
}

TEST(ReadWriteString, RoundTripUint8LengthType) {
    std::stringstream stream;
    const std::string original(200, 'x');
    bmmo::message_utils::write_string<uint8_t>(original, stream);

    std::string result;
    EXPECT_TRUE(bmmo::message_utils::read_string<uint8_t>(stream, result));
    EXPECT_EQ(original, result);
}

TEST(ReadWriteString, EmptyStringAtEndOfBuffer) {
    std::stringstream stream;
    bmmo::message_utils::write_string("", stream);

    std::string result = "sentinel";
    EXPECT_TRUE(bmmo::message_utils::read_string(stream, result));
    EXPECT_TRUE(result.empty());
}

TEST(ReadWriteString, MultipleStringsSequentially) {
    std::stringstream stream;
    bmmo::message_utils::write_string("first", stream);
    bmmo::message_utils::write_string("second", stream);

    std::string a, b;
    EXPECT_TRUE(bmmo::message_utils::read_string(stream, a));
    EXPECT_TRUE(bmmo::message_utils::read_string(stream, b));
    EXPECT_EQ("first", a);
    EXPECT_EQ("second", b);
}

TEST(ReadWriteString, RejectsForgedLengthLargerThanRemaining) {
    // length prefix claims 100 bytes but only 5 follow
    std::stringstream stream;
    uint32_t forged_length = 100;
    stream.write(reinterpret_cast<const char*>(&forged_length), sizeof(forged_length));
    stream.write("abcde", 5);

    std::string result;
    EXPECT_FALSE(bmmo::message_utils::read_string(stream, result));
}

TEST(ReadWriteString, RejectsForgedLengthAfterPartialConsumption) {
    // regression test for the old `length > tellp()` check, which compared
    // against the total buffer size instead of what is left to read:
    // a forged length that is smaller than the total size but larger than
    // the remaining bytes must still be rejected.
    std::stringstream stream;
    bmmo::message_utils::write_string("a much longer leading string", stream);
    uint32_t forged_length = 20; // < total size, > remaining (5)
    stream.write(reinterpret_cast<const char*>(&forged_length), sizeof(forged_length));
    stream.write("abcde", 5);

    std::string leading, result;
    ASSERT_TRUE(bmmo::message_utils::read_string(stream, leading));
    EXPECT_FALSE(bmmo::message_utils::read_string(stream, result));
}

TEST(ReadWriteString, RejectsMissingLengthPrefix) {
    std::stringstream stream;
    stream.write("ab", 2); // too short to even contain a uint32_t length

    std::string result;
    EXPECT_FALSE(bmmo::message_utils::read_string(stream, result));
}

TEST(ReadVariable, FailsAtEndOfStream) {
    std::stringstream stream;
    uint16_t half = 0x1234;
    stream.write(reinterpret_cast<const char*>(&half), sizeof(half));

    uint32_t value{};
    EXPECT_FALSE(bmmo::message_utils::read_variable(stream, &value));
}

TEST(ViewAs, AcceptsExactSizeAndLarger) {
    bmmo::player_disconnected_msg msg{};
    msg.content.connection_id = 42;

    test_networking_message exact(&msg, sizeof(msg));
    auto* view = bmmo::message_utils::view_as<bmmo::player_disconnected_msg>(&exact);
    ASSERT_NE(nullptr, view);
    EXPECT_EQ(42u, view->content.connection_id);

    // trailing garbage after a full message must not be rejected
    std::vector<char> padded(sizeof(msg) + 7);
    std::memcpy(padded.data(), &msg, sizeof(msg));
    test_networking_message larger(padded.data(), static_cast<int>(padded.size()));
    EXPECT_NE(nullptr, bmmo::message_utils::view_as<bmmo::player_disconnected_msg>(&larger));
}

TEST(ViewAs, RejectsTruncatedMessage) {
    bmmo::player_disconnected_msg msg{};
    test_networking_message truncated(&msg, static_cast<int>(sizeof(msg)) - 1);
    EXPECT_EQ(nullptr, bmmo::message_utils::view_as<bmmo::player_disconnected_msg>(&truncated));

    test_networking_message empty(nullptr, 0);
    EXPECT_EQ(nullptr, bmmo::message_utils::view_as<bmmo::player_disconnected_msg>(&empty));
}

TEST(TriviallyCopyableMsg, DeserializeRoundTrip) {
    bmmo::countdown_msg msg{};
    msg.content.type = bmmo::countdown_type::Go;
    msg.content.sender = 7;

    auto copy = bmmo::message_utils::deserialize<bmmo::countdown_msg>(&msg, sizeof(msg));
    EXPECT_EQ(bmmo::Countdown, copy.code);
    EXPECT_EQ(bmmo::countdown_type::Go, copy.content.type);
    EXPECT_EQ(7u, copy.content.sender);
}

TEST(ChatMsg, SerializeDeserializeRoundTrip) {
    bmmo::chat_msg msg{};
    msg.player_id = 123;
    msg.chat_content = "hello world";
    ASSERT_TRUE(msg.serialize());

    bmmo::chat_msg parsed{};
    parsed.raw.write(stream_payload(msg.raw).data(), static_cast<std::streamsize>(msg.size()));
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ(123u, parsed.player_id);
    EXPECT_EQ("hello world", parsed.chat_content);
}

TEST(ChatMsg, DeserializeRejectsEveryTruncation) {
    bmmo::chat_msg msg{};
    msg.player_id = 123;
    msg.chat_content = "hello world";
    ASSERT_TRUE(msg.serialize());
    const std::string payload = stream_payload(msg.raw);

    for (size_t len = 0; len < payload.size(); ++len) {
        bmmo::chat_msg parsed{};
        parsed.raw.write(payload.data(), static_cast<std::streamsize>(len));
        EXPECT_FALSE(parsed.deserialize()) << "accepted a payload truncated to " << len << " bytes";
    }
}

TEST(ChatMsg, DeserializeRejectsWrongOpcode) {
    bmmo::chat_msg msg{};
    msg.chat_content = "hi";
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);
    const auto wrong_code = static_cast<uint32_t>(bmmo::Ping);
    std::memcpy(payload.data(), &wrong_code, sizeof(wrong_code));

    bmmo::chat_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}

TEST(LoginRequestV3Msg, SerializeDeserializeRoundTrip) {
    bmmo::login_request_v3_msg msg{};
    msg.nickname = "SmokeTester";
    msg.version = {3, 6, 8, bmmo::Beta, 18};
    msg.cheated = 1;
    for (int i = 0; i < 16; ++i) msg.uuid[i] = static_cast<uint8_t>(i * 3);
    ASSERT_TRUE(msg.serialize());

    bmmo::login_request_v3_msg parsed{};
    parsed.raw.write(stream_payload(msg.raw).data(), static_cast<std::streamsize>(msg.size()));
    ASSERT_TRUE(parsed.deserialize());
    EXPECT_EQ("SmokeTester", parsed.nickname);
    EXPECT_EQ(1, parsed.cheated);
    EXPECT_EQ(0, std::memcmp(msg.uuid, parsed.uuid, sizeof(msg.uuid)));
}

TEST(LoginRequestV3Msg, DeserializeRejectsEveryTruncation) {
    bmmo::login_request_v3_msg msg{};
    msg.nickname = "SmokeTester";
    msg.version = {3, 6, 8, bmmo::Beta, 18};
    for (int i = 0; i < 16; ++i) msg.uuid[i] = static_cast<uint8_t>(i);
    ASSERT_TRUE(msg.serialize());
    const std::string payload = stream_payload(msg.raw);

    for (size_t len = 0; len < payload.size(); ++len) {
        bmmo::login_request_v3_msg parsed{};
        parsed.raw.write(payload.data(), static_cast<std::streamsize>(len));
        EXPECT_FALSE(parsed.deserialize()) << "accepted a payload truncated to " << len << " bytes";
    }
}

TEST(LoginRequestV3Msg, DeserializeRejectsForgedNicknameLength) {
    bmmo::login_request_v3_msg msg{};
    msg.nickname = "SmokeTester";
    msg.version = {3, 6, 8, bmmo::Beta, 18};
    ASSERT_TRUE(msg.serialize());
    std::string payload = stream_payload(msg.raw);

    // nickname length prefix sits right after the opcode and version
    const size_t length_offset = sizeof(bmmo::opcode) + sizeof(bmmo::version_t);
    uint32_t forged_length = 0x7fffffff;
    std::memcpy(payload.data() + length_offset, &forged_length, sizeof(forged_length));

    bmmo::login_request_v3_msg parsed{};
    parsed.raw.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    EXPECT_FALSE(parsed.deserialize());
}
