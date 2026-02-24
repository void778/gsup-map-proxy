#include <gtest/gtest.h>
#include "transport/M3uaCodec.hpp"

using namespace proxy;
using namespace proxy::transport::m3ua;

// ── Common header ─────────────────────────────────────────────────────────────

TEST(M3uaCodec, EncodedHeaderVersion) {
    auto bytes = encode(makeAspUp());
    ASSERT_GE(bytes.size(), kCommonHeaderSize);
    EXPECT_EQ(bytes[0], kVersion);   // version = 1
    EXPECT_EQ(bytes[1], 0x00);       // reserved
    EXPECT_EQ(bytes[2], kClassAspsm);
    EXPECT_EQ(bytes[3], kTypeAspUp);
}

TEST(M3uaCodec, EncodedLengthMatchesBytes) {
    auto bytes = encode(makeAspUp());
    uint32_t len = (uint32_t(bytes[4]) << 24) | (uint32_t(bytes[5]) << 16)
                 | (uint32_t(bytes[6]) <<  8) |  uint32_t(bytes[7]);
    EXPECT_EQ(len, bytes.size());
}

// ── ASPUP round-trip ──────────────────────────────────────────────────────────

TEST(M3uaCodec, AspUpRoundTrip) {
    auto original = makeAspUp();
    auto bytes = encode(original);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msgClass, kClassAspsm);
    EXPECT_EQ(decoded->msgType,  kTypeAspUp);
    EXPECT_FALSE(decoded->routingContext.has_value());
}

// ── ASPUP_ACK round-trip ──────────────────────────────────────────────────────

TEST(M3uaCodec, AspUpAckRoundTrip) {
    M3uaMessage ack{kClassAspsm, kTypeAspUpAck, std::nullopt, std::nullopt, {}};
    auto bytes = encode(ack);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msgClass, kClassAspsm);
    EXPECT_EQ(decoded->msgType,  kTypeAspUpAck);
}

// ── ASPAC with routing context ────────────────────────────────────────────────

TEST(M3uaCodec, AspAcWithRoutingContext) {
    auto original = makeAspAc(42u);
    auto bytes = encode(original);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msgClass, kClassAsptm);
    EXPECT_EQ(decoded->msgType,  kTypeAspAc);
    ASSERT_TRUE(decoded->routingContext.has_value());
    EXPECT_EQ(*decoded->routingContext, 42u);
}

TEST(M3uaCodec, AspAcWithoutRoutingContext) {
    auto original = makeAspAc();
    auto bytes = encode(original);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(decoded->routingContext.has_value());
}

// ── Heartbeat round-trip ──────────────────────────────────────────────────────

TEST(M3uaCodec, HeartbeatEmptyData) {
    auto hb = makeHeartbeat();
    auto bytes = encode(hb);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msgClass, kClassAspsm);
    EXPECT_EQ(decoded->msgType,  kTypeHeartbeat);
    EXPECT_TRUE(decoded->heartbeatData.empty());
}

TEST(M3uaCodec, HeartbeatWithData) {
    Bytes hbData = {0x01, 0x02, 0x03, 0x04};
    auto hb = makeHeartbeat(hbData);
    auto bytes = encode(hb);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->heartbeatData, hbData);
}

TEST(M3uaCodec, HeartbeatAckRoundTrip) {
    Bytes data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto ack = makeHeartbeatAck(data);
    auto bytes = encode(ack);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msgClass, kClassAspsm);
    EXPECT_EQ(decoded->msgType,  kTypeHeartbeatAck);
    EXPECT_EQ(decoded->heartbeatData, data);
}

// ── DATA / Protocol Data round-trip ──────────────────────────────────────────

TEST(M3uaCodec, DataMessageRoundTrip) {
    ProtocolData pd;
    pd.opc      = 0x0001;
    pd.dpc      = 0x0002;
    pd.si       = kSiSccp;
    pd.ni       = kNiInternational;
    pd.userData = {0xAA, 0xBB, 0xCC};

    auto original = makeData(pd);
    auto bytes = encode(original);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msgClass, kClassTransf);
    EXPECT_EQ(decoded->msgType,  kTypeData);
    ASSERT_TRUE(decoded->protocolData.has_value());

    const auto& rpd = *decoded->protocolData;
    EXPECT_EQ(rpd.opc,      0x0001u);
    EXPECT_EQ(rpd.dpc,      0x0002u);
    EXPECT_EQ(rpd.si,       kSiSccp);
    EXPECT_EQ(rpd.userData, pd.userData);
}

TEST(M3uaCodec, DataWithRoutingContext) {
    ProtocolData pd;
    pd.opc = 1; pd.dpc = 2;
    pd.userData = {0x01};

    auto msg = makeData(pd, 99u);
    auto bytes = encode(msg);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->routingContext.has_value());
    EXPECT_EQ(*decoded->routingContext, 99u);
}

// ── Incomplete data returns nullopt ───────────────────────────────────────────

TEST(M3uaCodec, IncompleteHeaderReturnsNullopt) {
    Bytes partial = {0x01, 0x00, 0x03, 0x01}; // 4 bytes — not enough for header
    auto result = decode(partial);
    EXPECT_FALSE(result.has_value());
}

TEST(M3uaCodec, IncompleteBodyReturnsNullopt) {
    // Encode a valid message, then truncate
    auto bytes = encode(makeAspUp());
    bytes.resize(bytes.size() - 1);
    // Now set length field to original so decode sees incomplete body
    // Actually the real message has no body — let's use a heartbeat with data
    Bytes hbData(20, 0xAB);
    auto hbBytes = encode(makeHeartbeat(hbData));
    hbBytes.resize(hbBytes.size() - 2); // truncate
    auto result = decode(hbBytes);
    EXPECT_FALSE(result.has_value());
}

// ── Bad version throws ────────────────────────────────────────────────────────

TEST(M3uaCodec, BadVersionThrows) {
    auto bytes = encode(makeAspUp());
    bytes[0] = 0x02; // corrupt version
    EXPECT_THROW(decode(bytes), std::runtime_error);
}

// ── Streaming decoder ─────────────────────────────────────────────────────────

TEST(M3uaDecoder, SingleMessage) {
    auto bytes = encode(makeAspUp());
    M3uaDecoder dec;
    dec.feed(bytes.data(), bytes.size());
    auto msg = dec.next();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->msgClass, kClassAspsm);
    EXPECT_EQ(msg->msgType,  kTypeAspUp);
    EXPECT_FALSE(dec.next().has_value());
}

TEST(M3uaDecoder, TwoMessagesInOneFeed) {
    auto b1 = encode(makeAspUp());
    auto b2 = encode(makeAspAc());
    Bytes combined;
    combined.insert(combined.end(), b1.begin(), b1.end());
    combined.insert(combined.end(), b2.begin(), b2.end());

    M3uaDecoder dec;
    dec.feed(combined.data(), combined.size());

    auto m1 = dec.next();
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->msgClass, kClassAspsm);

    auto m2 = dec.next();
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->msgClass, kClassAsptm);

    EXPECT_FALSE(dec.next().has_value());
}

TEST(M3uaDecoder, ChunkedFeed) {
    auto bytes = encode(makeAspAc(7u));
    M3uaDecoder dec;
    // Feed one byte at a time
    for (std::size_t i = 0; i < bytes.size() - 1; ++i) {
        dec.feed(&bytes[i], 1);
        EXPECT_FALSE(dec.next().has_value());
    }
    dec.feed(&bytes[bytes.size() - 1], 1);
    auto msg = dec.next();
    ASSERT_TRUE(msg.has_value());
    ASSERT_TRUE(msg->routingContext.has_value());
    EXPECT_EQ(*msg->routingContext, 7u);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

// Unknown message class/type must be decoded without throwing (ignored TLVs).
TEST(M3uaCodec, UnknownMessageClassRoundTrip) {
    M3uaMessage msg;
    msg.msgClass = 0xFF;
    msg.msgType  = 0xFF;
    auto bytes   = encode(msg);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msgClass, 0xFFu);
    EXPECT_EQ(decoded->msgType,  0xFFu);
}

// TLV with length < 4 must throw.
TEST(M3uaCodec, TlvLengthTooSmallThrows) {
    auto bytes = encode(makeAspUp());
    // Append a bogus TLV with tag=0x0001, len=2 (invalid: minimum is 4)
    bytes.push_back(0x00); bytes.push_back(0x01); // tag
    bytes.push_back(0x00); bytes.push_back(0x02); // len = 2
    // Fix the header length to include the extra 4 bytes
    uint32_t newLen = static_cast<uint32_t>(bytes.size());
    bytes[4] = (newLen >> 24) & 0xFF;
    bytes[5] = (newLen >> 16) & 0xFF;
    bytes[6] = (newLen >>  8) & 0xFF;
    bytes[7] = (newLen      ) & 0xFF;
    EXPECT_THROW(decode(bytes), std::runtime_error);
}

// Message with totalLen < 8 (header size) must throw.
TEST(M3uaCodec, TotalLenLessThanHeaderThrows) {
    auto bytes = encode(makeAspUp());
    // Set totalLen = 4 (less than kCommonHeaderSize = 8)
    bytes[4] = 0; bytes[5] = 0; bytes[6] = 0; bytes[7] = 4;
    EXPECT_THROW(decode(bytes), std::runtime_error);
}

// M3uaDecoder must reject a message whose totalLen exceeds kMaxM3uaMessageSize.
TEST(M3uaDecoder, OversizedMessageThrows) {
    // Build a header claiming a 128 KiB body (> 64 KiB limit).
    Bytes oversized(kCommonHeaderSize, 0);
    oversized[0] = kVersion;
    oversized[2] = kClassAspsm;
    oversized[3] = kTypeAspUp;
    constexpr uint32_t bigLen = 128u * 1024u; // 128 KiB
    oversized[4] = (bigLen >> 24) & 0xFF;
    oversized[5] = (bigLen >> 16) & 0xFF;
    oversized[6] = (bigLen >>  8) & 0xFF;
    oversized[7] = (bigLen      ) & 0xFF;

    M3uaDecoder dec;
    dec.feed(oversized.data(), oversized.size());
    EXPECT_THROW(dec.next(), std::runtime_error);
    // After the throw the decoder should have cleared its buffer
    EXPECT_FALSE(dec.next().has_value());
}

// After one complete message is consumed the decoder should correctly parse
// a second message fed in the same buffer.
TEST(M3uaDecoder, SecondMessageAfterFirstConsumed) {
    auto b1 = encode(makeAspUp());
    auto b2 = encode(makeAspAc(99u));

    M3uaDecoder dec;
    dec.feed(b1.data(), b1.size());
    ASSERT_TRUE(dec.next().has_value()); // consume first
    EXPECT_FALSE(dec.next().has_value()); // buffer drained

    dec.feed(b2.data(), b2.size());
    auto m2 = dec.next();
    ASSERT_TRUE(m2.has_value());
    ASSERT_TRUE(m2->routingContext.has_value());
    EXPECT_EQ(*m2->routingContext, 99u);
}
