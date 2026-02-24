#include <gtest/gtest.h>
#include "ipa/IpaCodec.hpp"

using namespace proxy;
using namespace proxy::ipa;

TEST(IpaEncodeTest, BasicFrame) {
    Bytes payload = {0x04, 0x01, 0x02};
    auto frame = encode(kStreamGsup, payload);
    // wireLen = payload.size() + 1 (stream byte) = 4
    ASSERT_EQ(frame.size(), kHeaderSize + payload.size());
    EXPECT_EQ(frame[0], 0x00);
    EXPECT_EQ(frame[1], 0x04); // wireLen
    EXPECT_EQ(frame[2], kStreamGsup);
    EXPECT_EQ(frame[3], 0x04);
    EXPECT_EQ(frame[4], 0x01);
    EXPECT_EQ(frame[5], 0x02);
}

TEST(IpaEncodeTest, EmptyPayload) {
    auto frame = encode(kStreamCcm, {});
    ASSERT_EQ(frame.size(), kHeaderSize);
    EXPECT_EQ(frame[0], 0x00);
    EXPECT_EQ(frame[1], 0x01); // wireLen=1 (just the stream byte)
    EXPECT_EQ(frame[2], kStreamCcm);
}

TEST(IpaDecodeTest, SingleFrame) {
    Bytes payload = {0xAA, 0xBB, 0xCC};
    auto wire = encode(kStreamGsup, payload);

    IpaDecoder dec;
    dec.feed(wire);
    auto frame = dec.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->streamId, kStreamGsup);
    EXPECT_EQ(frame->payload, payload);
    // No more frames
    EXPECT_FALSE(dec.next().has_value());
}

TEST(IpaDecodeTest, IncrementalFeed) {
    Bytes payload = {0x11, 0x22};
    auto wire = encode(kStreamGsup, payload);

    IpaDecoder dec;
    // Feed one byte at a time
    for (size_t i = 0; i < wire.size(); ++i) {
        dec.feed(&wire[i], 1);
        if (i + 1 < wire.size()) {
            EXPECT_FALSE(dec.next().has_value()) << "at byte " << i;
        }
    }
    auto frame = dec.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, payload);
}

TEST(IpaDecodeTest, TwoFramesInOneBuffer) {
    Bytes p1 = {0x01, 0x02};
    Bytes p2 = {0x03, 0x04, 0x05};
    auto wire = encode(kStreamGsup, p1);
    auto w2   = encode(kStreamCcm,  p2);
    wire.insert(wire.end(), w2.begin(), w2.end());

    IpaDecoder dec;
    dec.feed(wire);

    auto f1 = dec.next();
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->streamId, kStreamGsup);
    EXPECT_EQ(f1->payload,  p1);

    auto f2 = dec.next();
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->streamId, kStreamCcm);
    EXPECT_EQ(f2->payload,  p2);

    EXPECT_FALSE(dec.next().has_value());
}

TEST(IpaDecodeTest, PartialHeaderThenComplete) {
    Bytes payload = {0xDE, 0xAD};
    auto wire = encode(kStreamGsup, payload);

    IpaDecoder dec;
    // Feed only header (3 bytes), no payload yet
    dec.feed(wire.data(), 2); // only first 2 bytes
    EXPECT_FALSE(dec.next().has_value());
    dec.feed(wire.data() + 2, wire.size() - 2);
    auto frame = dec.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, payload);
}

TEST(IpaDecodeTest, RoundTripLargerPayload) {
    Bytes payload(200, 0x55);
    auto wire = encode(kStreamGsup, payload);
    IpaDecoder dec;
    dec.feed(wire);
    auto frame = dec.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload, payload);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

// wireLen == 0 must throw immediately.
TEST(IpaDecodeTest, ZeroWireLenThrows) {
    Bytes bad = {0x00, 0x00, kStreamGsup};
    IpaDecoder dec;
    dec.feed(bad);
    EXPECT_THROW(dec.next(), std::runtime_error);
}

// wireLen > kMaxIpaWireLen must throw and clear the buffer.
TEST(IpaDecodeTest, OversizedFrameThrows) {
    // wireLen = kMaxIpaWireLen + 1
    uint16_t bigLen = static_cast<uint16_t>(kMaxIpaWireLen + 1u);
    Bytes bad = {static_cast<uint8_t>(bigLen >> 8), static_cast<uint8_t>(bigLen & 0xFF), kStreamGsup};
    IpaDecoder dec;
    dec.feed(bad);
    EXPECT_THROW(dec.next(), std::runtime_error);
    // Buffer must be cleared — a subsequent feed/next should wait for new data.
    EXPECT_FALSE(dec.next().has_value());
}

// A frame right at the limit (kMaxIpaWireLen) must be accepted.
TEST(IpaDecodeTest, FrameAtMaxSizeAccepted) {
    // payload size = kMaxIpaWireLen - 1  (wireLen includes 1 stream byte)
    Bytes payload(kMaxIpaWireLen - 1u, 0xAB);
    auto wire = encode(kStreamGsup, payload);
    IpaDecoder dec;
    dec.feed(wire);
    auto frame = dec.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload.size(), payload.size());
}

// encode() must throw for a payload that would overflow uint16_t.
TEST(IpaEncodeTest, TooLargePayloadThrows) {
    Bytes huge(0x10000, 0x00); // 65536 bytes — exceeds uint16_t limit
    EXPECT_THROW(encode(kStreamGsup, huge), std::invalid_argument);
}

// Feeding an empty span should be a no-op: next() returns nullopt.
TEST(IpaDecodeTest, EmptyFeedIsNoop) {
    IpaDecoder dec;
    Bytes empty;
    dec.feed(empty.data(), 0);
    EXPECT_FALSE(dec.next().has_value());
}
