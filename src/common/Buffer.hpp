#pragma once
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <string>

namespace proxy {

using Bytes = std::vector<uint8_t>;

// Lightweight read cursor over a byte span — no copies.
class BufferReader {
public:
    BufferReader(const uint8_t* data, size_t len)
        : data_(data), len_(len), pos_(0) {}

    explicit BufferReader(const Bytes& buf)
        : BufferReader(buf.data(), buf.size()) {}

    bool empty()     const { return pos_ >= len_; }
    size_t remaining() const { return len_ > pos_ ? len_ - pos_ : 0; }
    size_t pos()     const { return pos_; }

    uint8_t peek() const {
        check(1);
        return data_[pos_];
    }

    uint8_t read8() {
        check(1);
        return data_[pos_++];
    }

    uint16_t readBE16() {
        check(2);
        uint16_t v = (static_cast<uint16_t>(data_[pos_]) << 8) | data_[pos_ + 1];
        pos_ += 2;
        return v;
    }

    uint32_t readBE32() {
        check(4);
        uint32_t v = (static_cast<uint32_t>(data_[pos_])     << 24)
                   | (static_cast<uint32_t>(data_[pos_ + 1]) << 16)
                   | (static_cast<uint32_t>(data_[pos_ + 2]) <<  8)
                   |  static_cast<uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

    Bytes readBytes(size_t n) {
        check(n);
        Bytes out(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return out;
    }

    BufferReader slice(size_t n) {
        check(n);
        BufferReader sub(data_ + pos_, n);
        pos_ += n;
        return sub;
    }

private:
    void check(size_t n) const {
        if (pos_ + n > len_)
            throw std::out_of_range("BufferReader: read past end");
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_;
};

// Simple write buffer
class BufferWriter {
public:
    void write8(uint8_t v)    { buf_.push_back(v); }

    void writeBE16(uint16_t v) {
        buf_.push_back((v >> 8) & 0xFF);
        buf_.push_back(v & 0xFF);
    }

    void writeBE32(uint32_t v) {
        buf_.push_back((v >> 24) & 0xFF);
        buf_.push_back((v >> 16) & 0xFF);
        buf_.push_back((v >>  8) & 0xFF);
        buf_.push_back(v & 0xFF);
    }

    void writeBytes(const Bytes& b)              { buf_.insert(buf_.end(), b.begin(), b.end()); }
    void writeBytes(const uint8_t* p, size_t n)  { buf_.insert(buf_.end(), p, p + n); }

    // Reserve a 1-byte length slot; fill it later with patch8().
    size_t reserveLen8() {
        size_t off = buf_.size();
        buf_.push_back(0);
        return off;
    }

    void patch8(size_t offset, uint8_t v) { buf_[offset] = v; }

    const Bytes& bytes() const { return buf_; }
    Bytes        take()        { return std::move(buf_); }
    size_t       size()  const { return buf_.size(); }

private:
    Bytes buf_;
};

} // namespace proxy
