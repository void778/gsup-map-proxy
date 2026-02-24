#include <gtest/gtest.h>
#include "map/BerCodec.hpp"

using namespace proxy;
using namespace proxy::map::ber;

// ── Length encoding ───────────────────────────────────────────────────────────

TEST(BerLengthTest, ShortForm) {
    BufferWriter w;
    writeLength(w, 42);
    auto b = w.bytes();
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0], 42);
}

TEST(BerLengthTest, ShortFormMaximum) {
    BufferWriter w;
    writeLength(w, 127);
    auto b = w.bytes();
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0], 127);
}

TEST(BerLengthTest, LongForm1Byte) {
    BufferWriter w;
    writeLength(w, 128);
    auto b = w.bytes();
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 0x81);
    EXPECT_EQ(b[1], 128);
}

TEST(BerLengthTest, LongForm2Bytes) {
    BufferWriter w;
    writeLength(w, 0x1234);
    auto b = w.bytes();
    ASSERT_EQ(b.size(), 3u);
    EXPECT_EQ(b[0], 0x82);
    EXPECT_EQ(b[1], 0x12);
    EXPECT_EQ(b[2], 0x34);
}

TEST(BerLengthTest, RoundTripShort) {
    for (size_t n : {0u, 1u, 50u, 127u}) {
        BufferWriter w;
        writeLength(w, n);
        BufferReader r(w.bytes());
        EXPECT_EQ(readLength(r), n) << "n=" << n;
    }
}

TEST(BerLengthTest, RoundTripLong) {
    for (size_t n : {128u, 200u, 500u, 0xFFFFu}) {
        BufferWriter w;
        writeLength(w, n);
        BufferReader r(w.bytes());
        EXPECT_EQ(readLength(r), n) << "n=" << n;
    }
}

TEST(BerLengthTest, IndefiniteFormThrows) {
    Bytes b = {0x80};
    BufferReader r(b);
    EXPECT_THROW(readLength(r), std::runtime_error);
}

// ── TLV ──────────────────────────────────────────────────────────────────────

TEST(BerTlvTest, WriteReadTlv) {
    Bytes value = {0x01, 0x02, 0x03};
    BufferWriter w;
    writeTlv(w, 0x04, value); // OCTET STRING

    BufferReader r(w.bytes());
    Bytes out;
    uint8_t tag = readTlv(r, out);
    EXPECT_EQ(tag, 0x04u);
    EXPECT_EQ(out, value);
}

TEST(BerTlvTest, WriteReadTlvInt) {
    BufferWriter w;
    writeTlvInt(w, 0x02, 42);
    BufferReader r(w.bytes());
    Bytes out;
    uint8_t tag = readTlv(r, out);
    EXPECT_EQ(tag, 0x02u);
    EXPECT_EQ(decodeInt(out), 42);
}

TEST(BerTlvTest, EmptyValue) {
    BufferWriter w;
    writeTlv(w, 0x05, {}); // NULL
    BufferReader r(w.bytes());
    Bytes out;
    uint8_t tag = readTlv(r, out);
    EXPECT_EQ(tag, 0x05u);
    EXPECT_TRUE(out.empty());
}

TEST(BerTlvTest, WrapConstructed) {
    Bytes inner = {0x01, 0x02};
    BufferWriter w;
    wrapConstructed(w, 0x30, inner); // SEQUENCE
    BufferReader r(w.bytes());
    Bytes out;
    uint8_t tag = readTlv(r, out);
    EXPECT_EQ(tag, 0x30u);
    EXPECT_EQ(out, inner);
}

// ── Integer ───────────────────────────────────────────────────────────────────

TEST(BerIntTest, Zero) {
    auto b = encodeInt(0);
    EXPECT_EQ(decodeInt(b), 0);
}

TEST(BerIntTest, PositiveSmall) {
    auto b = encodeInt(42);
    EXPECT_EQ(decodeInt(b), 42);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0], 42);
}

TEST(BerIntTest, PositiveLargeNeedsLeadingZero) {
    // 0x80 has its high bit set, so BER needs a leading 0x00
    auto b = encodeInt(0x80);
    EXPECT_EQ(decodeInt(b), 0x80);
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 0x00);
    EXPECT_EQ(b[1], 0x80);
}

TEST(BerIntTest, Positive256) {
    auto b = encodeInt(256);
    EXPECT_EQ(decodeInt(b), 256);
}

TEST(BerIntTest, NegativeMinusOne) {
    auto b = encodeInt(-1);
    EXPECT_EQ(decodeInt(b), -1);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0], 0xFF);
}

TEST(BerIntTest, NegativeLarge) {
    EXPECT_EQ(decodeInt(encodeInt(-128)), -128);
    EXPECT_EQ(decodeInt(encodeInt(-256)), -256);
    EXPECT_EQ(decodeInt(encodeInt(-1000)), -1000);
}

TEST(BerIntTest, OperationCodes) {
    // Validate the MAP operation codes we use
    for (int op : {2, 3, 7, 8, 23, 56, 67}) {
        EXPECT_EQ(decodeInt(encodeInt(op)), op) << "op=" << op;
    }
}

TEST(BerIntTest, DecodeEmptyThrows) {
    EXPECT_THROW(decodeInt({}), std::runtime_error);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

// readTlv with only a tag byte and no length should throw (BufferReader
// throws std::out_of_range when reading past end).
TEST(BerTlvTest, TruncatedAfterTagThrows) {
    Bytes b = {0x04}; // tag only, no length
    BufferReader r(b);
    Bytes out;
    EXPECT_THROW(readTlv(r, out), std::exception);
}

// readTlv with tag+length but fewer payload bytes than length claims.
TEST(BerTlvTest, TruncatedValueThrows) {
    BufferWriter w;
    writeLength(w, 10); // claims 10 value bytes
    Bytes buf = {0x04};
    buf.insert(buf.end(), w.bytes().begin(), w.bytes().end());
    buf.push_back(0xAA); // only 1 value byte instead of 10
    BufferReader r(buf);
    Bytes out;
    EXPECT_THROW(readTlv(r, out), std::exception);
}

// readLength with 0x84 (claims 4 following length bytes) on a buffer that
// has none must throw (BufferReader throws std::out_of_range).
TEST(BerLengthTest, MultiByteFormWithNoFollowingBytesThrows) {
    Bytes b = {0x84}; // says 4 length bytes follow, but none do
    BufferReader r(b);
    EXPECT_THROW(readLength(r), std::exception);
}

// Large negative integer round-trip.
TEST(BerIntTest, LargeNegativeRoundTrip) {
    for (int64_t v : {int64_t(-128), int64_t(-129), int64_t(-256), int64_t(-32768), int64_t(-2147483648LL)}) {
        EXPECT_EQ(decodeInt(encodeInt(v)), v) << "v=" << v;
    }
}

// Zero-length TLV value round-trips correctly (already covered by EmptyValue
// but verify via writeTlv + readTlv symmetry explicitly).
TEST(BerTlvTest, ZeroLengthValueRoundTrip) {
    BufferWriter w;
    writeTlv(w, 0x05, {});
    BufferReader r(w.bytes());
    Bytes out;
    uint8_t tag = readTlv(r, out);
    EXPECT_EQ(tag, 0x05u);
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(r.remaining()); // reader fully consumed
}
