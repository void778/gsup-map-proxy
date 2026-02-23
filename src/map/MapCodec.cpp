#include "MapCodec.hpp"
#include "BerCodec.hpp"
#include "MapTypes.hpp"
#include <stdexcept>

// MAP/TCAP codec — implements the subset of TCAP (ITU-T Q.773) and MAP
// (3GPP TS 29.002) needed to proxy between GSUP and a real HLR.
//
// TCAP structure encoded:
//
//   Begin (request):
//     [APPLICATION 2] {
//       otid [APPLICATION 8] <4-byte TID>
//       components [APPLICATION 12] {
//         Invoke [1] {
//           invokeId INTEGER
//           operationCode INTEGER
//           <operation arguments SEQUENCE>
//         }
//       }
//     }
//
//   End (response):
//     [APPLICATION 4] {
//       dtid [APPLICATION 9] <4-byte TID>
//       components [APPLICATION 12] {
//         ReturnResultLast [2] {
//           invokeId INTEGER
//           result SEQUENCE {
//             operationCode INTEGER
//             <response arguments SEQUENCE>
//           }
//         }
//       }
//     }
//
// Notes:
//   - Dialogue portion omitted for brevity (not strictly required for basic MAP).
//   - Operation code uses LOCAL form: INTEGER.

namespace proxy::map {

using namespace ber;

// ── IMSI BCD helpers (same semantics as in GsupCodec) ────────────────────────
// IMSI in MAP is an OCTET STRING of packed BCD (same format as GSUP).
static Bytes imsiToBcd(const std::string& imsi) {
    Bytes out;
    for (size_t i = 0; i < imsi.size(); i += 2) {
        uint8_t lo = static_cast<uint8_t>(imsi[i] - '0');
        uint8_t hi = (i + 1 < imsi.size())
                     ? static_cast<uint8_t>(imsi[i + 1] - '0')
                     : 0x0F;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

static std::string bcdToImsi(const Bytes& bcd) {
    std::string out;
    for (uint8_t byte : bcd) {
        uint8_t lo = byte & 0x0F;
        uint8_t hi = (byte >> 4) & 0x0F;
        if (lo > 9) throw std::runtime_error("MAP: invalid BCD in IMSI");
        out += static_cast<char>('0' + lo);
        if (hi != 0x0F) {
            if (hi > 9) throw std::runtime_error("MAP: invalid BCD in IMSI");
            out += static_cast<char>('0' + hi);
        }
    }
    return out;
}

// ── Operation argument encoders ──────────────────────────────────────────────

static Bytes encodeSendAuthInfoReq(const MapMessage& m) {
    // SendAuthenticationInfoArg ::= SEQUENCE { imsi IMSI, numVectors INTEGER, ... }
    BufferWriter inner;
    writeTlv(inner, kBerOctetStr, imsiToBcd(m.imsi));
    writeTlvInt(inner, kBerInt, m.numRequestedVectors.value_or(1));
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

static Bytes encodeSendAuthInfoRes(const MapMessage& m) {
    // SendAuthenticationInfoRes ::= SEQUENCE { authenticationSetList [...] }
    BufferWriter tripletSet;
    for (const auto& t : m.authTriplets) {
        BufferWriter trip;
        writeTlv(trip, kBerOctetStr, t.rand);
        writeTlv(trip, kBerOctetStr, t.sres);
        writeTlv(trip, kBerOctetStr, t.kc);
        wrapConstructed(tripletSet, kBerSequence, trip.bytes());
    }
    for (const auto& q : m.authQuintuplets) {
        BufferWriter quint;
        writeTlv(quint, kBerOctetStr, q.rand);
        writeTlv(quint, kBerOctetStr, q.xres);
        writeTlv(quint, kBerOctetStr, q.ck);
        writeTlv(quint, kBerOctetStr, q.ik);
        writeTlv(quint, kBerOctetStr, q.autn);
        wrapConstructed(tripletSet, kBerSequence, quint.bytes());
    }
    BufferWriter res;
    wrapConstructed(res, kBerSequence, tripletSet.bytes());
    return res.take();
}

static Bytes encodeUpdateGprsLocationReq(const MapMessage& m) {
    BufferWriter inner;
    writeTlv(inner, kBerOctetStr, imsiToBcd(m.imsi));
    if (m.sgsnNumber)  writeTlv(inner, kBerOctetStr, *m.sgsnNumber);
    if (m.sgsnAddress) writeTlv(inner, kBerOctetStr, *m.sgsnAddress);
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

static Bytes encodeUpdateGprsLocationRes(const MapMessage& m) {
    BufferWriter inner;
    if (m.hlrNumber) writeTlv(inner, kBerOctetStr, *m.hlrNumber);
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

static Bytes encodeCancelLocationReq(const MapMessage& m) {
    BufferWriter inner;
    writeTlv(inner, kBerOctetStr, imsiToBcd(m.imsi));
    if (m.cancelType) writeTlvOctet(inner, kBerInt, *m.cancelType);
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

static Bytes encodeInsertSubscriberDataReq(const MapMessage& m) {
    BufferWriter inner;
    writeTlv(inner, kBerOctetStr, imsiToBcd(m.imsi));
    if (m.msisdn) writeTlv(inner, kBerOctetStr, *m.msisdn);
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

static Bytes encodeDeleteSubscriberDataReq(const MapMessage& m) {
    BufferWriter inner;
    writeTlv(inner, kBerOctetStr, imsiToBcd(m.imsi));
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

static Bytes encodePurgeMsReq(const MapMessage& m) {
    BufferWriter inner;
    writeTlv(inner, kBerOctetStr, imsiToBcd(m.imsi));
    if (m.sgsnNumber) writeTlv(inner, kBerOctetStr, *m.sgsnNumber);
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

static Bytes encodePurgeMsRes(const MapMessage& m) {
    BufferWriter inner;
    if (m.freezePtmsi) {
        // freezePTMSI NULL
        inner.write8(kBerNull);
        inner.write8(0);
    }
    BufferWriter seq;
    wrapConstructed(seq, kBerSequence, inner.bytes());
    return seq.take();
}

// ── Component encoder ────────────────────────────────────────────────────────

static Bytes encodeInvoke(const MapMessage& m, const Bytes& args) {
    BufferWriter body;
    writeTlvInt(body, kBerInt, m.invokeId);
    writeTlvInt(body, kBerInt, static_cast<int64_t>(m.operation));
    body.writeBytes(args);
    BufferWriter comp;
    wrapConstructed(comp, kCompTagInvoke, body.bytes());
    return comp.take();
}

static Bytes encodeReturnResult(const MapMessage& m, const Bytes& result) {
    // ReturnResultLast ::= SEQUENCE { invokeId, result SEQUENCE { opCode, args }}
    BufferWriter resultInner;
    writeTlvInt(resultInner, kBerInt, static_cast<int64_t>(m.operation));
    resultInner.writeBytes(result);
    BufferWriter resultSeq;
    wrapConstructed(resultSeq, kBerSequence, resultInner.bytes());

    BufferWriter body;
    writeTlvInt(body, kBerInt, m.invokeId);
    body.writeBytes(resultSeq.bytes());
    BufferWriter comp;
    wrapConstructed(comp, kCompTagReturnResult, body.bytes());
    return comp.take();
}

static Bytes encodeReturnError(const MapMessage& m) {
    BufferWriter body;
    writeTlvInt(body, kBerInt, m.invokeId);
    writeTlvInt(body, kBerInt, m.errorCode.value_or(34)); // 34 = systemFailure
    BufferWriter comp;
    wrapConstructed(comp, kCompTagReturnError, body.bytes());
    return comp.take();
}

// ── Main encoder ─────────────────────────────────────────────────────────────

Bytes encode(const MapMessage& m) {
    // 1. Build operation arguments
    Bytes args;
    switch (m.operation) {
        case MapOperation::SendAuthenticationInfo:
            args = (m.component == ComponentType::Invoke)
                   ? encodeSendAuthInfoReq(m)
                   : encodeSendAuthInfoRes(m);
            break;
        case MapOperation::UpdateGprsLocation:
            args = (m.component == ComponentType::Invoke)
                   ? encodeUpdateGprsLocationReq(m)
                   : encodeUpdateGprsLocationRes(m);
            break;
        case MapOperation::CancelLocation:
            args = encodeCancelLocationReq(m);
            break;
        case MapOperation::InsertSubscriberData:
            args = encodeInsertSubscriberDataReq(m);
            break;
        case MapOperation::DeleteSubscriberData:
            args = encodeDeleteSubscriberDataReq(m);
            break;
        case MapOperation::PurgeMS:
            args = (m.component == ComponentType::Invoke)
                   ? encodePurgeMsReq(m)
                   : encodePurgeMsRes(m);
            break;
        default:
            break;
    }

    // 2. Wrap in component
    Bytes component;
    switch (m.component) {
        case ComponentType::Invoke:
            component = encodeInvoke(m, args);
            break;
        case ComponentType::ReturnResult:
            component = encodeReturnResult(m, args);
            break;
        case ComponentType::ReturnError:
            component = encodeReturnError(m);
            break;
        default:
            throw std::runtime_error("MAP: unsupported component type");
    }

    // 3. Wrap components in Component Portion
    BufferWriter compPortion;
    wrapConstructed(compPortion, kTcapTagComponents, component);

    // 4. Build TID bytes
    Bytes tid = {
        static_cast<uint8_t>((m.transactionId >> 24) & 0xFF),
        static_cast<uint8_t>((m.transactionId >> 16) & 0xFF),
        static_cast<uint8_t>((m.transactionId >>  8) & 0xFF),
        static_cast<uint8_t>( m.transactionId        & 0xFF),
    };

    // 5. Build TCAP PDU
    BufferWriter tcap;
    bool isRequest = (m.component == ComponentType::Invoke);
    if (isRequest) {
        writeTlv(tcap, kTcapTagOtid, tid);     // otid
    } else {
        writeTlv(tcap, kTcapTagDtid, tid);     // dtid
    }
    tcap.writeBytes(compPortion.bytes());

    uint8_t pdvTag = isRequest ? kTcapTagBegin : kTcapTagEnd;
    BufferWriter out;
    wrapConstructed(out, pdvTag, tcap.bytes());
    return out.take();
}

// ── Decoder helpers ──────────────────────────────────────────────────────────

static MapMessage decodeSendAuthInfoReq(BufferReader& r, MapMessage m) {
    // SEQUENCE { imsi, numVectors, ... }
    if (r.empty()) return m;
    Bytes seqVal;
    uint8_t tag = readTlv(r, seqVal);
    if (tag != kBerSequence) return m;
    BufferReader sr(seqVal);
    while (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerOctetStr && m.imsi.empty())
            m.imsi = bcdToImsi(val);
        else if (t == kBerInt && !m.numRequestedVectors)
            m.numRequestedVectors = static_cast<uint8_t>(decodeInt(val));
    }
    return m;
}

static MapMessage decodeSendAuthInfoRes(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal); // outer SEQUENCE
    BufferReader sr(seqVal);
    while (!sr.empty()) {
        Bytes tripVal;
        uint8_t t = readTlv(sr, tripVal);
        if (t != kBerSequence) continue;
        BufferReader tr(tripVal);
        // Try to detect triplet vs quintuplet by count of OCTET STRINGs
        std::vector<Bytes> fields;
        while (!tr.empty()) {
            Bytes v;
            uint8_t ft = readTlv(tr, v);
            if (ft == kBerOctetStr) fields.push_back(v);
        }
        if (fields.size() >= 3 && fields.size() < 5) {
            MapAuthTriplet trip;
            trip.rand = fields[0];
            trip.sres = fields[1];
            trip.kc   = fields[2];
            m.authTriplets.push_back(std::move(trip));
        } else if (fields.size() >= 5) {
            MapAuthQuintuplet q;
            q.rand = fields[0];
            q.xres = fields[1];
            q.ck   = fields[2];
            q.ik   = fields[3];
            q.autn = fields[4];
            m.authQuintuplets.push_back(std::move(q));
        }
    }
    return m;
}

static MapMessage decodeUpdateGprsLocationReq(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal);
    BufferReader sr(seqVal);
    bool firstOctet = true;
    while (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerOctetStr) {
            if (firstOctet && m.imsi.empty()) { m.imsi = bcdToImsi(val); firstOctet = false; }
            else if (!m.sgsnNumber)  m.sgsnNumber  = val;
            else if (!m.sgsnAddress) m.sgsnAddress = val;
        }
    }
    return m;
}

static MapMessage decodeUpdateGprsLocationRes(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal);
    BufferReader sr(seqVal);
    while (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerOctetStr && !m.hlrNumber) m.hlrNumber = val;
    }
    return m;
}

static MapMessage decodeCancelLocationReq(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal);
    BufferReader sr(seqVal);
    bool firstOctet = true;
    while (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerOctetStr && firstOctet) { m.imsi = bcdToImsi(val); firstOctet = false; }
        else if (t == kBerInt) m.cancelType = static_cast<uint8_t>(decodeInt(val));
    }
    return m;
}

static MapMessage decodeInsertSubscriberDataReq(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal);
    BufferReader sr(seqVal);
    bool firstOctet = true;
    while (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerOctetStr) {
            if (firstOctet) { m.imsi = bcdToImsi(val); firstOctet = false; }
            else if (!m.msisdn) m.msisdn = val;
        }
    }
    return m;
}

static MapMessage decodeDeleteSubscriberDataReq(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal);
    BufferReader sr(seqVal);
    if (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerOctetStr && m.imsi.empty())
            m.imsi = bcdToImsi(val);
    }
    return m;
}

static MapMessage decodePurgeMsReq(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal);
    BufferReader sr(seqVal);
    bool firstOctet = true;
    while (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerOctetStr) {
            if (firstOctet) { m.imsi = bcdToImsi(val); firstOctet = false; }
            else if (!m.sgsnNumber) m.sgsnNumber = val;
        }
    }
    return m;
}

static MapMessage decodePurgeMsRes(BufferReader& r, MapMessage m) {
    if (r.empty()) return m;
    Bytes seqVal;
    readTlv(r, seqVal);
    BufferReader sr(seqVal);
    while (!sr.empty()) {
        Bytes val;
        uint8_t t = readTlv(sr, val);
        if (t == kBerNull) m.freezePtmsi = true;
    }
    return m;
}

// ── Main decoder ─────────────────────────────────────────────────────────────

MapMessage decode(const uint8_t* data, size_t len) {
    BufferReader r(data, len);

    // Outer TCAP PDU tag
    Bytes tcapBody;
    uint8_t pdvTag = readTlv(r, tcapBody);
    bool isRequest = (pdvTag == kTcapTagBegin || pdvTag == kTcapTagUnidirectional);

    BufferReader tr(tcapBody);
    MapMessage msg;

    // Parse TCAP fields
    while (!tr.empty()) {
        Bytes val;
        uint8_t t = readTlv(tr, val);
        if (t == kTcapTagOtid || t == kTcapTagDtid) {
            // TID: up to 4 bytes
            uint32_t tid = 0;
            for (uint8_t b : val) tid = (tid << 8) | b;
            msg.transactionId = tid;
        } else if (t == kTcapTagComponents) {
            BufferReader cr(val);
            while (!cr.empty()) {
                Bytes compVal;
                uint8_t compTag = readTlv(cr, compVal);
                BufferReader cbr(compVal);

                if (compTag == kCompTagInvoke) {
                    msg.component = ComponentType::Invoke;
                    // invokeId
                    Bytes iv; readTlv(cbr, iv);
                    msg.invokeId = static_cast<uint8_t>(decodeInt(iv));
                    // operationCode
                    Bytes ov; readTlv(cbr, ov);
                    msg.operation = static_cast<MapOperation>(decodeInt(ov));
                    // arguments
                    switch (msg.operation) {
                        case MapOperation::SendAuthenticationInfo:
                            msg = decodeSendAuthInfoReq(cbr, msg); break;
                        case MapOperation::UpdateGprsLocation:
                            msg = decodeUpdateGprsLocationReq(cbr, msg); break;
                        case MapOperation::CancelLocation:
                            msg = decodeCancelLocationReq(cbr, msg); break;
                        case MapOperation::InsertSubscriberData:
                            msg = decodeInsertSubscriberDataReq(cbr, msg); break;
                        case MapOperation::DeleteSubscriberData:
                            msg = decodeDeleteSubscriberDataReq(cbr, msg); break;
                        case MapOperation::PurgeMS:
                            msg = decodePurgeMsReq(cbr, msg); break;
                        default: break;
                    }
                } else if (compTag == kCompTagReturnResult) {
                    msg.component = ComponentType::ReturnResult;
                    Bytes iv; readTlv(cbr, iv);
                    msg.invokeId = static_cast<uint8_t>(decodeInt(iv));
                    // result SEQUENCE { opCode, args }
                    Bytes resultVal; readTlv(cbr, resultVal);
                    BufferReader rr(resultVal);
                    Bytes opVal; readTlv(rr, opVal);
                    msg.operation = static_cast<MapOperation>(decodeInt(opVal));
                    switch (msg.operation) {
                        case MapOperation::SendAuthenticationInfo:
                            msg = decodeSendAuthInfoRes(rr, msg); break;
                        case MapOperation::UpdateGprsLocation:
                            msg = decodeUpdateGprsLocationRes(rr, msg); break;
                        case MapOperation::PurgeMS:
                            msg = decodePurgeMsRes(rr, msg); break;
                        default: break;
                    }
                } else if (compTag == kCompTagReturnError) {
                    msg.component = ComponentType::ReturnError;
                    Bytes iv; readTlv(cbr, iv);
                    msg.invokeId = static_cast<uint8_t>(decodeInt(iv));
                    Bytes ev; readTlv(cbr, ev);
                    msg.errorCode = static_cast<uint8_t>(decodeInt(ev));
                }
            }
        }
        // kTcapTagDialogue ignored (no dialogue portion in our simplified encoding)
    }
    (void)isRequest;
    return msg;
}

MapMessage decode(const Bytes& data) {
    return decode(data.data(), data.size());
}

} // namespace proxy::map
