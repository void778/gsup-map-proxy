#include "GsupCodec.hpp"
#include <stdexcept>
#include <sstream>

namespace proxy::gsup {

// ── BCD utilities ────────────────────────────────────────────────────────────

Bytes imsiToBcd(const std::string& imsi) {
    if (imsi.empty() || imsi.size() > 15)
        throw std::invalid_argument("IMSI must be 1-15 digits");
    for (char c : imsi)
        if (c < '0' || c > '9')
            throw std::invalid_argument("IMSI contains non-digit character");

    Bytes out;
    out.reserve((imsi.size() + 1) / 2);
    for (size_t i = 0; i < imsi.size(); i += 2) {
        uint8_t lo = static_cast<uint8_t>(imsi[i] - '0');
        uint8_t hi = (i + 1 < imsi.size())
                     ? static_cast<uint8_t>(imsi[i + 1] - '0')
                     : 0x0F;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string bcdToImsi(const Bytes& bcd) {
    std::string out;
    out.reserve(bcd.size() * 2);
    for (uint8_t byte : bcd) {
        uint8_t lo = byte & 0x0F;
        uint8_t hi = (byte >> 4) & 0x0F;
        if (lo > 9) throw std::runtime_error("Invalid BCD nibble in IMSI");
        out += static_cast<char>('0' + lo);
        if (hi != 0x0F) {
            if (hi > 9) throw std::runtime_error("Invalid BCD nibble in IMSI");
            out += static_cast<char>('0' + hi);
        }
    }
    return out;
}

Bytes isdnToBcd(const std::string& digits) {
    // TON/NPI byte: 0x91 = international, 0x81 = national (default international)
    Bytes out;
    out.push_back(0x91); // international, ISDN/telephony
    for (size_t i = 0; i < digits.size(); i += 2) {
        uint8_t lo = static_cast<uint8_t>(digits[i] - '0');
        uint8_t hi = (i + 1 < digits.size())
                     ? static_cast<uint8_t>(digits[i + 1] - '0')
                     : 0x0F;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string bcdToIsdn(const Bytes& bcd) {
    if (bcd.empty()) return {};
    // Skip TON/NPI byte
    std::string out;
    for (size_t i = 1; i < bcd.size(); ++i) {
        uint8_t lo = bcd[i] & 0x0F;
        uint8_t hi = (bcd[i] >> 4) & 0x0F;
        out += static_cast<char>('0' + lo);
        if (hi != 0x0F) out += static_cast<char>('0' + hi);
    }
    return out;
}

// ── Decoder ──────────────────────────────────────────────────────────────────

namespace {

AuthTuple decodeAuthTuple(BufferReader& r) {
    AuthTuple t;
    while (!r.empty()) {
        auto tag = static_cast<IeTag>(r.read8());
        uint8_t len = r.read8();
        Bytes val = r.readBytes(len);
        switch (tag) {
            case IeTag::Rand: t.rand = val; break;
            case IeTag::Sres: t.sres = val; break;
            case IeTag::Kc:   t.kc   = val; break;
            case IeTag::Ik:   t.ik   = val; break;
            case IeTag::Ck:   t.ck   = val; break;
            case IeTag::Autn: t.autn = val; break;
            case IeTag::Res:  t.res  = val; break;
            default: break; // ignore unknown sub-IEs
        }
    }
    return t;
}

PdpInfo decodePdpInfo(BufferReader& r) {
    PdpInfo p;
    while (!r.empty()) {
        auto tag = static_cast<IeTag>(r.read8());
        uint8_t len = r.read8();
        Bytes val = r.readBytes(len);
        switch (tag) {
            case IeTag::PdpContextId:
                p.contextId = val.empty() ? 0 : val[0];
                break;
            case IeTag::PdpType:
                if (val.size() >= 2)
                    p.pdpType = static_cast<uint16_t>((val[0] << 8) | val[1]);
                break;
            case IeTag::AccessPointName:
                p.apn = std::string(val.begin(), val.end());
                break;
            case IeTag::PdpAddress:
                p.pdpAddress = val;
                break;
            case IeTag::PdpQos:
                p.qos = val;
                break;
            default: break;
        }
    }
    return p;
}

} // anonymous namespace

GsupMessage decode(const uint8_t* data, size_t len) {
    if (len < 1) throw std::runtime_error("GSUP: empty payload");
    BufferReader r(data, len);

    GsupMessage msg;
    msg.type = static_cast<MessageType>(r.read8());

    while (!r.empty()) {
        if (r.remaining() < 2)
            throw std::runtime_error("GSUP: truncated IE header");
        auto tag = static_cast<IeTag>(r.read8());
        uint8_t ieLen = r.read8();
        if (r.remaining() < ieLen)
            throw std::runtime_error("GSUP: IE length exceeds payload");

        switch (tag) {
            case IeTag::Imsi: {
                Bytes bcd = r.readBytes(ieLen);
                msg.imsi = bcdToImsi(bcd);
                break;
            }
            case IeTag::Cause:
                msg.cause = r.read8();
                // skip remaining bytes of this IE if len > 1
                if (ieLen > 1) r.readBytes(ieLen - 1);
                break;
            case IeTag::AuthTuple: {
                BufferReader sub = r.slice(ieLen);
                msg.authTuples.push_back(decodeAuthTuple(sub));
                break;
            }
            case IeTag::PdpInfoComplete:
                r.readBytes(ieLen);
                msg.pdpInfoComplete = true;
                break;
            case IeTag::PdpInfo: {
                BufferReader sub = r.slice(ieLen);
                msg.pdpInfoList.push_back(decodePdpInfo(sub));
                break;
            }
            case IeTag::CancelType:
                if (ieLen >= 1)
                    msg.cancelType = static_cast<CancelType>(r.read8());
                if (ieLen > 1) r.readBytes(ieLen - 1);
                break;
            case IeTag::FreezePtmsi:
                r.readBytes(ieLen);
                msg.freezePtmsi = true;
                break;
            case IeTag::Msisdn:
                msg.msisdn = r.readBytes(ieLen);
                break;
            case IeTag::HlrNumber:
                msg.hlrNumber = r.readBytes(ieLen);
                break;
            case IeTag::NumVectorsReq:
                if (ieLen >= 1) msg.numVectorsRequested = r.read8();
                if (ieLen > 1) r.readBytes(ieLen - 1);
                break;
            case IeTag::CnDomain:
                if (ieLen >= 1) msg.cnDomain = static_cast<CnDomain>(r.read8());
                if (ieLen > 1) r.readBytes(ieLen - 1);
                break;
            default:
                r.readBytes(ieLen); // skip unknown IEs
                break;
        }
    }
    return msg;
}

GsupMessage decode(const Bytes& payload) {
    return decode(payload.data(), payload.size());
}

// ── Encoder ──────────────────────────────────────────────────────────────────

namespace {

void writeTlv(BufferWriter& w, IeTag tag, const Bytes& val) {
    if (val.size() > 255)
        throw std::runtime_error("GSUP IE value too large");
    w.write8(static_cast<uint8_t>(tag));
    w.write8(static_cast<uint8_t>(val.size()));
    w.writeBytes(val);
}

void writeTlv1(BufferWriter& w, IeTag tag, uint8_t val) {
    w.write8(static_cast<uint8_t>(tag));
    w.write8(1);
    w.write8(val);
}

void encodeAuthTuple(BufferWriter& w, const AuthTuple& t) {
    w.write8(static_cast<uint8_t>(IeTag::AuthTuple));
    size_t lenOff = w.reserveLen8();
    size_t start = w.size();

    writeTlv(w, IeTag::Rand, t.rand);
    writeTlv(w, IeTag::Sres, t.sres);
    writeTlv(w, IeTag::Kc,   t.kc);
    if (t.ik)   writeTlv(w, IeTag::Ik,   *t.ik);
    if (t.ck)   writeTlv(w, IeTag::Ck,   *t.ck);
    if (t.autn) writeTlv(w, IeTag::Autn, *t.autn);
    if (t.res)  writeTlv(w, IeTag::Res,  *t.res);

    w.patch8(lenOff, static_cast<uint8_t>(w.size() - start));
}

void encodePdpInfo(BufferWriter& w, const PdpInfo& p) {
    w.write8(static_cast<uint8_t>(IeTag::PdpInfo));
    size_t lenOff = w.reserveLen8();
    size_t start = w.size();

    writeTlv1(w, IeTag::PdpContextId, p.contextId);
    if (p.pdpType) {
        Bytes t = { static_cast<uint8_t>(*p.pdpType >> 8),
                    static_cast<uint8_t>(*p.pdpType & 0xFF) };
        writeTlv(w, IeTag::PdpType, t);
    }
    if (p.apn) {
        Bytes a(p.apn->begin(), p.apn->end());
        writeTlv(w, IeTag::AccessPointName, a);
    }
    if (p.pdpAddress) writeTlv(w, IeTag::PdpAddress, *p.pdpAddress);
    if (p.qos)        writeTlv(w, IeTag::PdpQos,     *p.qos);

    w.patch8(lenOff, static_cast<uint8_t>(w.size() - start));
}

} // anonymous namespace

Bytes encode(const GsupMessage& msg) {
    BufferWriter w;
    w.write8(static_cast<uint8_t>(msg.type));

    // IMSI — always present
    if (!msg.imsi.empty())
        writeTlv(w, IeTag::Imsi, imsiToBcd(msg.imsi));

    if (msg.cause)
        writeTlv1(w, IeTag::Cause, *msg.cause);
    if (msg.numVectorsRequested)
        writeTlv1(w, IeTag::NumVectorsReq, *msg.numVectorsRequested);

    for (const auto& t : msg.authTuples)
        encodeAuthTuple(w, t);

    if (msg.msisdn)    writeTlv(w, IeTag::Msisdn,    *msg.msisdn);
    if (msg.hlrNumber) writeTlv(w, IeTag::HlrNumber, *msg.hlrNumber);

    if (msg.pdpInfoComplete) {
        w.write8(static_cast<uint8_t>(IeTag::PdpInfoComplete));
        w.write8(0);
    }
    for (const auto& p : msg.pdpInfoList)
        encodePdpInfo(w, p);

    if (msg.cancelType)
        writeTlv1(w, IeTag::CancelType, static_cast<uint8_t>(*msg.cancelType));
    if (msg.freezePtmsi) {
        w.write8(static_cast<uint8_t>(IeTag::FreezePtmsi));
        w.write8(0);
    }
    if (msg.cnDomain)
        writeTlv1(w, IeTag::CnDomain, static_cast<uint8_t>(*msg.cnDomain));

    return w.take();
}

} // namespace proxy::gsup
