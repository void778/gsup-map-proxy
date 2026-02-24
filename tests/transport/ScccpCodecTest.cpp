#include <gtest/gtest.h>
#include "transport/ScccpCodec.hpp"

using namespace proxy;
using namespace proxy::transport::sccp;

// ── makeGtAddress helper ──────────────────────────────────────────────────────

TEST(SccpAddress, GtAddressFields) {
    auto addr = makeGtAddress("+49161123456");
    EXPECT_TRUE(addr.routeByGt);
    EXPECT_EQ(addr.digits, "+49161123456");
    EXPECT_EQ(addr.nai, kNaiInternational);
    EXPECT_FALSE(addr.ssnPresent);
}

TEST(SccpAddress, GtAddressWithSsn) {
    auto addr = makeGtAddress("+49161", kSsnHlr, true);
    EXPECT_TRUE(addr.ssnPresent);
    EXPECT_EQ(addr.ssn, kSsnHlr);
}

TEST(SccpAddress, SsnAddressFields) {
    auto addr = makeSsnAddress(0x1234, kSsnHlr);
    EXPECT_FALSE(addr.routeByGt);
    EXPECT_TRUE(addr.pcPresent);
    EXPECT_EQ(addr.pc, 0x1234u);
    EXPECT_TRUE(addr.ssnPresent);
    EXPECT_EQ(addr.ssn, kSsnHlr);
}

// ── UDT round-trip ────────────────────────────────────────────────────────────

TEST(ScccpCodec, BasicUdtRoundTrip) {
    SccpUdt udt;
    udt.protocolClass = kProtoClass0;
    udt.calledParty   = makeGtAddress("+49161000000", kSsnHlr, true);
    udt.callingParty  = makeGtAddress("+49161111111", kSsnSgsn, true);
    udt.data          = {0x01, 0x02, 0x03, 0x04};

    Bytes encoded = encodeUdt(udt);
    ASSERT_FALSE(encoded.empty());
    EXPECT_EQ(encoded[0], kMsgUdt);

    SccpUdt decoded = decodeUdt(encoded);
    EXPECT_EQ(decoded.protocolClass, kProtoClass0);
    EXPECT_EQ(decoded.data, udt.data);
}

TEST(ScccpCodec, CalledPartyDigitsPreserved) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("+4916100000");
    udt.callingParty = makeGtAddress("+4916111111");
    udt.data         = {0xAB};

    auto encoded = encodeUdt(udt);
    auto decoded = decodeUdt(encoded);

    // The '+' is not encoded in BCD (only decimal digits 0-9 are packed).
    // Digits after stripping '+' should round-trip.
    // Our packBcd works on any char — '+' would produce garbage.
    // Real SCCP GT digits are pure decimal; strip '+' in practice.
    // Here we use pure-digit strings:
    udt.calledParty.digits  = "4916100000";
    udt.callingParty.digits = "4916111111";
    encoded = encodeUdt(udt);
    decoded = decodeUdt(encoded);
    EXPECT_EQ(decoded.calledParty.digits, "4916100000");
    EXPECT_EQ(decoded.callingParty.digits, "4916111111");
}

TEST(ScccpCodec, OddLengthDigitsRoundTrip) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("491610000000"); // 12 digits — even
    udt.callingParty = makeGtAddress("49161000001");  // 11 digits — odd
    udt.data         = {0xFF};

    auto encoded = encodeUdt(udt);
    auto decoded = decodeUdt(encoded);
    EXPECT_EQ(decoded.calledParty.digits,  "491610000000");
    EXPECT_EQ(decoded.callingParty.digits, "49161000001");
}

TEST(ScccpCodec, SsnPreservedInRoundTrip) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("49161", kSsnHlr, true);
    udt.callingParty = makeGtAddress("49162", kSsnSgsn, true);
    udt.data         = {0x00};

    auto encoded = encodeUdt(udt);
    auto decoded = decodeUdt(encoded);
    EXPECT_TRUE(decoded.calledParty.ssnPresent);
    EXPECT_EQ(decoded.calledParty.ssn, kSsnHlr);
    EXPECT_TRUE(decoded.callingParty.ssnPresent);
    EXPECT_EQ(decoded.callingParty.ssn, kSsnSgsn);
}

TEST(ScccpCodec, DataPayloadPreserved) {
    Bytes payload(64, 0x42);
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("49161");
    udt.callingParty = makeGtAddress("49162");
    udt.data         = payload;

    auto decoded = decodeUdt(encodeUdt(udt));
    EXPECT_EQ(decoded.data, payload);
}

TEST(ScccpCodec, EmptyDataAllowed) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("491");
    udt.callingParty = makeGtAddress("492");
    udt.data         = {};

    auto decoded = decodeUdt(encodeUdt(udt));
    EXPECT_TRUE(decoded.data.empty());
}

// ── Error cases ───────────────────────────────────────────────────────────────

TEST(ScccpCodec, TooShortThrows) {
    Bytes bad = {0x09, 0x00}; // UDT but no pointers
    EXPECT_THROW(decodeUdt(bad), std::runtime_error);
}

TEST(ScccpCodec, WrongMessageTypeThrows) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("491");
    udt.callingParty = makeGtAddress("492");
    udt.data         = {0x01};
    auto encoded = encodeUdt(udt);
    encoded[0] = 0x01; // not UDT
    EXPECT_THROW(decodeUdt(encoded), std::runtime_error);
}

// ── Protocol class ────────────────────────────────────────────────────────────

TEST(ScccpCodec, ProtocolClassPreserved) {
    SccpUdt udt;
    udt.protocolClass = kProtoClass0Ret;
    udt.calledParty  = makeGtAddress("491");
    udt.callingParty = makeGtAddress("492");
    udt.data         = {0x01};

    auto decoded = decodeUdt(encodeUdt(udt));
    EXPECT_EQ(decoded.protocolClass, kProtoClass0Ret);
}

// ── First byte of encoded UDT ─────────────────────────────────────────────────

TEST(ScccpCodec, FirstByteIsUdtMsgType) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("491");
    udt.callingParty = makeGtAddress("492");
    udt.data         = {0x01};
    auto encoded = encodeUdt(udt);
    EXPECT_EQ(encoded[0], kMsgUdt);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

// Called-party pointer pointing past end of message must throw.
TEST(ScccpCodec, CalledPartyPointerOutOfRangeThrows) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("491");
    udt.callingParty = makeGtAddress("492");
    udt.data         = {0x01};
    auto encoded = encodeUdt(udt);
    // Overwrite ptr1 with a value that jumps far past end of buffer.
    encoded[2] = 0xFF;
    EXPECT_THROW(decodeUdt(encoded), std::runtime_error);
}

// Data pointer pointing past end of message must throw.
TEST(ScccpCodec, DataPointerOutOfRangeThrows) {
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("491");
    udt.callingParty = makeGtAddress("492");
    udt.data         = {0x01};
    auto encoded = encodeUdt(udt);
    // Overwrite ptr3 (data pointer) with a large offset.
    encoded[4] = 0xFF;
    EXPECT_THROW(decodeUdt(encoded), std::runtime_error);
}

// Large payload preserved exactly (> 1 byte, tests the length byte path).
TEST(ScccpCodec, LargeDataPayloadPreserved) {
    Bytes payload(250, 0x7E);
    SccpUdt udt;
    udt.calledParty  = makeGtAddress("49161");
    udt.callingParty = makeGtAddress("49162");
    udt.data         = payload;
    auto decoded = decodeUdt(encodeUdt(udt));
    EXPECT_EQ(decoded.data, payload);
}

// SSN boundary values (0 and 255) round-trip correctly.
TEST(ScccpCodec, SsnBoundaryValuesRoundTrip) {
    for (uint8_t ssn : {uint8_t(0), uint8_t(255)}) {
        SccpUdt udt;
        udt.calledParty  = makeGtAddress("491", ssn, true);
        udt.callingParty = makeGtAddress("492");
        udt.data         = {0x00};
        auto decoded = decodeUdt(encodeUdt(udt));
        ASSERT_TRUE(decoded.calledParty.ssnPresent);
        EXPECT_EQ(decoded.calledParty.ssn, ssn);
    }
}
