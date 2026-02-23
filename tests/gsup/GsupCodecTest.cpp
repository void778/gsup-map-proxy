#include <gtest/gtest.h>
#include "gsup/GsupCodec.hpp"
#include "gsup/GsupMessage.hpp"

using namespace proxy;
using namespace proxy::gsup;

// ── BCD helpers ──────────────────────────────────────────────────────────────

TEST(BcdTest, EncodeEvenLengthImsi) {
    // "123456789012" — 12 digits, 6 bytes
    auto bcd = imsiToBcd("123456789012");
    ASSERT_EQ(bcd.size(), 6u);
    // First byte: lo=1, hi=2 → 0x21
    EXPECT_EQ(bcd[0], 0x21);
    EXPECT_EQ(bcd[1], 0x43);
    EXPECT_EQ(bcd[2], 0x65);
    EXPECT_EQ(bcd[3], 0x87);
    EXPECT_EQ(bcd[4], 0x09);
    EXPECT_EQ(bcd[5], 0x21);
}

TEST(BcdTest, EncodeOddLengthImsi) {
    // "12345" — 5 digits, 3 bytes; last nibble = 0xF
    auto bcd = imsiToBcd("12345");
    ASSERT_EQ(bcd.size(), 3u);
    EXPECT_EQ(bcd[0], 0x21);
    EXPECT_EQ(bcd[1], 0x43);
    EXPECT_EQ(bcd[2], 0xF5); // hi=0xF (filler), lo=5
}

TEST(BcdTest, RoundTripImsi) {
    std::string imsi = "262019876543210";
    EXPECT_EQ(bcdToImsi(imsiToBcd(imsi)), imsi);
}

TEST(BcdTest, RoundTripOddImsi) {
    std::string imsi = "26201987654321";
    EXPECT_EQ(bcdToImsi(imsiToBcd(imsi)), imsi);
}

TEST(BcdTest, InvalidImsiNonDigit) {
    EXPECT_THROW(imsiToBcd("1234X6"), std::invalid_argument);
}

TEST(BcdTest, InvalidImsiEmpty) {
    EXPECT_THROW(imsiToBcd(""), std::invalid_argument);
}

TEST(BcdTest, InvalidImsiTooLong) {
    EXPECT_THROW(imsiToBcd("1234567890123456"), std::invalid_argument); // 16 digits
}

// ── SendAuthInfo Request ──────────────────────────────────────────────────────

TEST(GsupDecodeTest, SendAuthInfoRequest) {
    // Manually build: msgType=0x08 | IMSI IE | NumVectors IE
    std::string imsi = "262019876543210";
    Bytes imsiB = imsiToBcd(imsi);

    Bytes payload;
    payload.push_back(static_cast<uint8_t>(MessageType::SendAuthInfoRequest));
    payload.push_back(static_cast<uint8_t>(IeTag::Imsi));
    payload.push_back(static_cast<uint8_t>(imsiB.size()));
    payload.insert(payload.end(), imsiB.begin(), imsiB.end());
    payload.push_back(static_cast<uint8_t>(IeTag::NumVectorsReq));
    payload.push_back(1);
    payload.push_back(5);

    auto msg = decode(payload);
    EXPECT_EQ(msg.type, MessageType::SendAuthInfoRequest);
    EXPECT_EQ(msg.imsi, imsi);
    ASSERT_TRUE(msg.numVectorsRequested.has_value());
    EXPECT_EQ(*msg.numVectorsRequested, 5);
}

// ── SendAuthInfo Result with auth tuples ─────────────────────────────────────

TEST(GsupRoundTripTest, SendAuthInfoResult) {
    GsupMessage msg;
    msg.type = MessageType::SendAuthInfoResult;
    msg.imsi = "262019876543210";

    AuthTuple t;
    t.rand = Bytes(16, 0xAA);
    t.sres = Bytes(4,  0xBB);
    t.kc   = Bytes(8,  0xCC);
    msg.authTuples.push_back(t);

    auto encoded = encode(msg);
    auto decoded = decode(encoded);

    EXPECT_EQ(decoded.type, MessageType::SendAuthInfoResult);
    EXPECT_EQ(decoded.imsi, msg.imsi);
    ASSERT_EQ(decoded.authTuples.size(), 1u);
    EXPECT_EQ(decoded.authTuples[0].rand, t.rand);
    EXPECT_EQ(decoded.authTuples[0].sres, t.sres);
    EXPECT_EQ(decoded.authTuples[0].kc,   t.kc);
}

TEST(GsupRoundTripTest, SendAuthInfoResultWithUmts) {
    GsupMessage msg;
    msg.type = MessageType::SendAuthInfoResult;
    msg.imsi = "001010123456789";

    AuthTuple t;
    t.rand = Bytes(16, 0x11);
    t.sres = Bytes(4,  0x22);
    t.kc   = Bytes(8,  0x33);
    t.ik   = Bytes(16, 0x44);
    t.ck   = Bytes(16, 0x55);
    t.autn = Bytes(16, 0x66);
    t.res  = Bytes(8,  0x77);
    msg.authTuples.push_back(t);

    auto decoded = decode(encode(msg));
    ASSERT_EQ(decoded.authTuples.size(), 1u);
    EXPECT_EQ(decoded.authTuples[0].ik,   t.ik);
    EXPECT_EQ(decoded.authTuples[0].ck,   t.ck);
    EXPECT_EQ(decoded.authTuples[0].autn, t.autn);
    EXPECT_EQ(decoded.authTuples[0].res,  t.res);
}

// ── UpdateLocation ────────────────────────────────────────────────────────────

TEST(GsupRoundTripTest, UpdateLocationRequest) {
    GsupMessage msg;
    msg.type = MessageType::UpdateLocationRequest;
    msg.imsi = "262019876543210";
    msg.cnDomain = CnDomain::Ps;

    auto decoded = decode(encode(msg));
    EXPECT_EQ(decoded.type, MessageType::UpdateLocationRequest);
    EXPECT_EQ(decoded.imsi, msg.imsi);
    ASSERT_TRUE(decoded.cnDomain.has_value());
    EXPECT_EQ(*decoded.cnDomain, CnDomain::Ps);
}

TEST(GsupRoundTripTest, UpdateLocationResult) {
    GsupMessage msg;
    msg.type      = MessageType::UpdateLocationResult;
    msg.imsi      = "262019876543210";
    msg.hlrNumber = Bytes{0x91, 0x49, 0x06, 0x01, 0x90, 0x00, 0xF8};

    auto decoded = decode(encode(msg));
    EXPECT_EQ(decoded.type, MessageType::UpdateLocationResult);
    ASSERT_TRUE(decoded.hlrNumber.has_value());
    EXPECT_EQ(*decoded.hlrNumber, *msg.hlrNumber);
}

// ── LocationCancel ────────────────────────────────────────────────────────────

TEST(GsupRoundTripTest, LocationCancelRequest) {
    GsupMessage msg;
    msg.type       = MessageType::LocationCancelRequest;
    msg.imsi       = "262019876543210";
    msg.cancelType = CancelType::UpdateProcedure;
    msg.freezePtmsi = true;

    auto decoded = decode(encode(msg));
    EXPECT_EQ(decoded.type, MessageType::LocationCancelRequest);
    ASSERT_TRUE(decoded.cancelType.has_value());
    EXPECT_EQ(*decoded.cancelType, CancelType::UpdateProcedure);
    EXPECT_TRUE(decoded.freezePtmsi);
}

// ── InsertSubscriberData ──────────────────────────────────────────────────────

TEST(GsupRoundTripTest, InsertSubscriberData) {
    GsupMessage msg;
    msg.type   = MessageType::InsertDataRequest;
    msg.imsi   = "262019876543210";
    msg.msisdn = Bytes{0x91, 0x49, 0x06, 0x10, 0x02, 0x30, 0xF0};
    msg.pdpInfoComplete = true;

    PdpInfo pdp;
    pdp.contextId = 1;
    pdp.pdpType   = 0x0121; // IPv4
    pdp.apn       = "internet";
    msg.pdpInfoList.push_back(pdp);

    auto decoded = decode(encode(msg));
    EXPECT_EQ(decoded.type, MessageType::InsertDataRequest);
    EXPECT_TRUE(decoded.pdpInfoComplete);
    ASSERT_EQ(decoded.pdpInfoList.size(), 1u);
    EXPECT_EQ(decoded.pdpInfoList[0].contextId, 1);
    ASSERT_TRUE(decoded.pdpInfoList[0].pdpType.has_value());
    EXPECT_EQ(*decoded.pdpInfoList[0].pdpType, 0x0121u);
    EXPECT_EQ(*decoded.pdpInfoList[0].apn, "internet");
}

// ── PurgeMS ───────────────────────────────────────────────────────────────────

TEST(GsupRoundTripTest, PurgeMsResult) {
    GsupMessage msg;
    msg.type = MessageType::PurgeMsResult;
    msg.imsi = "262019876543210";
    msg.freezePtmsi = true;

    auto decoded = decode(encode(msg));
    EXPECT_EQ(decoded.type, MessageType::PurgeMsResult);
    EXPECT_TRUE(decoded.freezePtmsi);
}

// ── Error messages ────────────────────────────────────────────────────────────

TEST(GsupRoundTripTest, SendAuthInfoError) {
    GsupMessage msg;
    msg.type  = MessageType::SendAuthInfoError;
    msg.imsi  = "262019876543210";
    msg.cause = 0x60; // no data

    auto decoded = decode(encode(msg));
    EXPECT_EQ(decoded.type, MessageType::SendAuthInfoError);
    ASSERT_TRUE(decoded.cause.has_value());
    EXPECT_EQ(*decoded.cause, 0x60);
}

// ── Malformed input ───────────────────────────────────────────────────────────

TEST(GsupDecodeTest, EmptyPayloadThrows) {
    EXPECT_THROW(decode(Bytes{}), std::runtime_error);
}

TEST(GsupDecodeTest, TruncatedIeThrows) {
    Bytes payload;
    payload.push_back(static_cast<uint8_t>(MessageType::SendAuthInfoRequest));
    payload.push_back(static_cast<uint8_t>(IeTag::Imsi));
    payload.push_back(10); // claims 10 bytes but none follow
    // The GSUP decoder checks the IE length bound explicitly and throws runtime_error.
    EXPECT_THROW(decode(payload), std::runtime_error);
}

// ── Multiple auth tuples ──────────────────────────────────────────────────────

TEST(GsupRoundTripTest, MultipleAuthTuples) {
    GsupMessage msg;
    msg.type = MessageType::SendAuthInfoResult;
    msg.imsi = "262019876543210";
    for (int i = 0; i < 3; ++i) {
        AuthTuple t;
        t.rand = Bytes(16, static_cast<uint8_t>(i));
        t.sres = Bytes(4,  static_cast<uint8_t>(i + 10));
        t.kc   = Bytes(8,  static_cast<uint8_t>(i + 20));
        msg.authTuples.push_back(t);
    }

    auto decoded = decode(encode(msg));
    ASSERT_EQ(decoded.authTuples.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(decoded.authTuples[i].rand, Bytes(16, static_cast<uint8_t>(i)));
    }
}
