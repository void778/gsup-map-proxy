#include "transport/ScccpCodec.hpp"
#include "common/Buffer.hpp"
#include <stdexcept>

namespace proxy::transport::sccp {

// ── Address encoding/decoding ─────────────────────────────────────────────────

// Pack E.164 digits into BCD (two digits per byte, low nibble = first digit).
// Trailing 0xF filler for odd-length strings.
static Bytes packBcd(const std::string& digits) {
    Bytes out;
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        uint8_t lo = static_cast<uint8_t>(digits[i] - '0');
        uint8_t hi = (i + 1 < digits.size())
                     ? static_cast<uint8_t>(digits[i + 1] - '0')
                     : 0x0F;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

static std::string unpackBcd(const uint8_t* data, std::size_t len, bool oddLen) {
    std::string digits;
    for (std::size_t i = 0; i < len; ++i) {
        digits += static_cast<char>('0' + (data[i] & 0x0F));
        uint8_t hi = (data[i] >> 4) & 0x0F;
        if (i + 1 == len && oddLen) break; // last nibble is filler
        digits += static_cast<char>('0' + hi);
    }
    return digits;
}

static Bytes encodeAddress(const SccpAddress& addr) {
    BufferWriter w;

    if (addr.routeByGt) {
        // Address Indicator: GTI=format4, SSN indicator if present
        uint8_t ai = kAiRoutByGt | kGtiTtNpEsNai;
        if (addr.ssnPresent) ai |= kAiSsnPresent;
        w.write8(ai);

        if (addr.ssnPresent) w.write8(addr.ssn);

        // GT format 4: TT | NP+ES | NAI | BCD digits
        Bytes bcd = packBcd(addr.digits);
        bool  odd = (addr.digits.size() % 2) != 0;
        uint8_t es  = odd ? kEsBcdOdd : kEsBcdEven;
        uint8_t nai = addr.nai;
        if (odd) nai |= 0x80; // bit 7 = odd flag in NAI byte

        w.write8(addr.tt);
        w.write8(static_cast<uint8_t>(addr.np | es));
        w.write8(nai);
        w.writeBytes(bcd);
    } else {
        // Route by SSN (PC+SSN, no GT)
        uint8_t ai = kAiRoutBySsn | kAiSsnPresent;
        if (addr.pcPresent) ai |= 0x01; // PC indicator
        w.write8(ai);
        if (addr.ssnPresent) w.write8(addr.ssn);
        if (addr.pcPresent) {
            // 14-bit point code, LS byte first (ITU)
            w.write8(addr.pc & 0xFF);
            w.write8((addr.pc >> 8) & 0x3F);
        }
    }
    return w.bytes();
}

static SccpAddress decodeAddress(BufferReader& r) {
    SccpAddress addr;
    uint8_t ai = r.read8();

    addr.routeByGt = !(ai & kAiRoutBySsn);
    bool hasSsn = (ai & kAiSsnPresent) != 0;
    uint8_t gti = ai & 0x70; // bits 6-4
    bool hasGt  = (gti != kGtiNoGt);

    addr.ssnPresent = hasSsn;
    if (hasSsn) addr.ssn = r.read8();

    if (hasGt) {
        if (gti == kGtiTtNpEsNai) {
            addr.tt = r.read8();
            uint8_t npEs = r.read8();
            addr.np = npEs & 0xF0;
            uint8_t es  = npEs & 0x0F;
            uint8_t naiRaw = r.read8();
            bool odd = (naiRaw & 0x80) != 0;
            addr.nai = naiRaw & 0x7F;

            // Remaining bytes are BCD digits
            Bytes bcd = r.readBytes(r.remaining());
            addr.digits = unpackBcd(bcd.data(), bcd.size(), odd);
            (void)es;
        } else {
            // Unknown GT format — consume rest of address as-is
            r.readBytes(r.remaining());
        }
    }
    return addr;
}

// ── UDT encode ────────────────────────────────────────────────────────────────
//
// UDT format (Q.713 §3.7):
//   Msg type (1) | Protocol class (1) | Ptr-called (1) | Ptr-calling (1) | Ptr-data (1)
//   | Called addr len (1) | Called addr ...
//   | Calling addr len (1) | Calling addr ...
//   | Data len (1) | Data ...
//
// The three mandatory-variable pointers point (relative to themselves) to the
// length octet of called/calling/data.

Bytes encodeUdt(const SccpUdt& udt) {
    Bytes called  = encodeAddress(udt.calledParty);
    Bytes calling = encodeAddress(udt.callingParty);

    // Build variable parts: each is {length_byte, value_bytes}
    // Pointers are byte offsets from the pointer octet to the length octet.
    // Pointer 1 starts at offset 0 from the first pointer byte (byte 2 of UDT).
    // There are 3 pointer bytes, so:
    //   ptr1 = 3  (3 bytes away from ptr1 itself: skip ptr2, ptr3, then land on len)
    //   ptr2 = 3 + 1 + called.size()
    //   ptr3 = 3 + 1 + called.size() + 1 + calling.size()

    // Each pointer is relative to its own byte position.
    // ptr1 at byte 2 → called_len at byte 5  → offset = 3
    // ptr2 at byte 3 → calling_len at byte 6+called.size() → offset = 3 + called.size()
    // ptr3 at byte 4 → data_len at byte 7+called.size()+calling.size() → offset = 3 + called.size() + calling.size()
    uint8_t ptr1 = 3;
    uint8_t ptr2 = static_cast<uint8_t>(3 + called.size());
    uint8_t ptr3 = static_cast<uint8_t>(3 + called.size() + calling.size());

    BufferWriter w;
    w.write8(kMsgUdt);
    w.write8(udt.protocolClass);
    w.write8(ptr1);
    w.write8(ptr2);
    w.write8(ptr3);

    // Variable parts
    w.write8(static_cast<uint8_t>(called.size()));
    w.writeBytes(called);

    w.write8(static_cast<uint8_t>(calling.size()));
    w.writeBytes(calling);

    w.write8(static_cast<uint8_t>(udt.data.size()));
    w.writeBytes(udt.data);

    return w.bytes();
}

// ── UDT decode ────────────────────────────────────────────────────────────────

SccpUdt decodeUdt(const Bytes& raw) {
    if (raw.size() < 5)
        throw std::runtime_error("SCCP UDT too short");

    BufferReader r(raw);
    uint8_t msgType = r.read8();
    if (msgType != kMsgUdt)
        throw std::runtime_error("SCCP: not a UDT message");

    uint8_t protoClass = r.read8();

    // Three relative pointers (each from its own position)
    std::size_t ptr1Pos = r.pos();
    uint8_t ptr1 = r.read8();
    std::size_t ptr2Pos = r.pos();
    uint8_t ptr2 = r.read8();
    std::size_t ptr3Pos = r.pos();
    uint8_t ptr3 = r.read8();

    // Decode called party
    std::size_t calledLenPos = ptr1Pos + ptr1;
    if (calledLenPos >= raw.size())
        throw std::runtime_error("SCCP UDT: called party pointer out of range");
    uint8_t calledLen = raw[calledLenPos];
    Bytes calledBytes(raw.begin() + calledLenPos + 1,
                      raw.begin() + calledLenPos + 1 + calledLen);
    BufferReader cr(calledBytes);
    SccpAddress calledAddr = decodeAddress(cr);

    // Decode calling party
    std::size_t callingLenPos = ptr2Pos + ptr2;
    if (callingLenPos >= raw.size())
        throw std::runtime_error("SCCP UDT: calling party pointer out of range");
    uint8_t callingLen = raw[callingLenPos];
    Bytes callingBytes(raw.begin() + callingLenPos + 1,
                       raw.begin() + callingLenPos + 1 + callingLen);
    BufferReader kr(callingBytes);
    SccpAddress callingAddr = decodeAddress(kr);

    // Decode data
    std::size_t dataLenPos = ptr3Pos + ptr3;
    if (dataLenPos >= raw.size())
        throw std::runtime_error("SCCP UDT: data pointer out of range");
    uint8_t dataLen = raw[dataLenPos];
    if (dataLenPos + 1 + dataLen > raw.size())
        throw std::runtime_error("SCCP UDT: data extends past message");
    Bytes data(raw.begin() + dataLenPos + 1,
               raw.begin() + dataLenPos + 1 + dataLen);

    SccpUdt udt;
    udt.protocolClass = protoClass;
    udt.calledParty   = calledAddr;
    udt.callingParty  = callingAddr;
    udt.data          = data;
    return udt;
}

// ── Address helpers ───────────────────────────────────────────────────────────

SccpAddress makeGtAddress(const std::string& digits, uint8_t ssn, bool hasSsn) {
    SccpAddress a;
    a.routeByGt = true;
    a.digits    = digits;
    a.tt        = kTtInternational;
    a.np        = kNpIsdnTelephony;
    a.nai       = kNaiInternational;
    if (hasSsn) {
        a.ssnPresent = true;
        a.ssn        = ssn;
    }
    return a;
}

SccpAddress makeSsnAddress(uint32_t pc, uint8_t ssn) {
    SccpAddress a;
    a.routeByGt  = false;
    a.pcPresent  = true;
    a.pc         = pc;
    a.ssnPresent = true;
    a.ssn        = ssn;
    return a;
}

} // namespace proxy::transport::sccp
