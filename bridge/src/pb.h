// Minimal protobuf reader/writer.
//
// We need a handful of fields out of Meshtastic's FromRadio and a couple of
// small ToRadio messages back. Pulling in nanopb plus the whole .proto set for
// that is a lot of build surface, so this is a hand-rolled scanner — the same
// approach source/Proto.mc takes on the watch, and the field numbers are kept
// deliberately identical so the two stay in step.

#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace pb {

enum WireType {
    WIRE_VARINT = 0,
    WIRE_FIXED64 = 1,
    WIRE_BYTES = 2,
    WIRE_FIXED32 = 5,
};

// Cursor over an encoded message. Every read is bounds-checked; a truncated
// or malformed buffer makes the cursor go bad rather than run off the end.
class Reader {
  public:
    Reader(const uint8_t *buf, size_t len) : b_(buf), n_(len), p_(0), ok_(true) {}

    bool ok() const { return ok_; }
    bool done() const { return p_ >= n_ || !ok_; }
    size_t pos() const { return p_; }
    size_t remaining() const { return p_ < n_ ? n_ - p_ : 0; }

    // Reads a tag. Returns false at end of buffer or on a bad tag.
    bool tag(uint32_t *field, uint32_t *wire) {
        uint64_t t;
        if (!varint(&t)) return false;
        *field = (uint32_t)(t >> 3);
        *wire = (uint32_t)(t & 0x07);
        // Field 0 is never valid; treat it as corruption so we stop early
        // instead of misparsing the rest of the message.
        if (*field == 0) { ok_ = false; return false; }
        return true;
    }

    bool varint(uint64_t *out) {
        uint64_t v = 0;
        int shift = 0;
        while (p_ < n_) {
            uint8_t c = b_[p_++];
            v |= (uint64_t)(c & 0x7f) << shift;
            if (!(c & 0x80)) { *out = v; return true; }
            shift += 7;
            if (shift > 63) { ok_ = false; return false; }
        }
        ok_ = false;
        return false;
    }

    bool fixed32(uint32_t *out) {
        if (p_ + 4 > n_) { ok_ = false; return false; }
        *out = (uint32_t)b_[p_] | ((uint32_t)b_[p_ + 1] << 8) |
               ((uint32_t)b_[p_ + 2] << 16) | ((uint32_t)b_[p_ + 3] << 24);
        p_ += 4;
        return true;
    }

    bool fixed32f(float *out) {
        uint32_t u;
        if (!fixed32(&u)) return false;
        memcpy(out, &u, 4);
        return true;
    }

    // Length-delimited field: hands back a view into the buffer.
    bool bytes(const uint8_t **data, size_t *len) {
        uint64_t l;
        if (!varint(&l)) return false;
        if (l > remaining()) { ok_ = false; return false; }
        *data = b_ + p_;
        *len = (size_t)l;
        p_ += (size_t)l;
        return true;
    }

    // Skips a field we do not care about. Returns false if the wire type is
    // one we cannot skip safely, in which case the caller should stop.
    bool skip(uint32_t wire) {
        uint64_t v;
        const uint8_t *d;
        size_t l;
        switch (wire) {
            case WIRE_VARINT:  return varint(&v);
            case WIRE_FIXED32: { uint32_t u; return fixed32(&u); }
            case WIRE_FIXED64: if (p_ + 8 > n_) { ok_ = false; return false; }
                               p_ += 8; return true;
            case WIRE_BYTES:   return bytes(&d, &l);
            default:           ok_ = false; return false;
        }
    }

  private:
    const uint8_t *b_;
    size_t n_;
    size_t p_;
    bool ok_;
};

// Builder for the small ToRadio messages we send back to the node.
class Writer {
  public:
    void varintField(uint32_t field, uint64_t v) {
        tag(field, WIRE_VARINT);
        varint(v);
    }

    void fixed32Field(uint32_t field, uint32_t v) {
        tag(field, WIRE_FIXED32);
        out_.push_back(v & 0xff);
        out_.push_back((v >> 8) & 0xff);
        out_.push_back((v >> 16) & 0xff);
        out_.push_back((v >> 24) & 0xff);
    }

    void bytesField(uint32_t field, const uint8_t *data, size_t len) {
        tag(field, WIRE_BYTES);
        varint(len);
        out_.insert(out_.end(), data, data + len);
    }

    void subMessage(uint32_t field, const Writer &sub) {
        bytesField(field, sub.data(), sub.size());
    }

    const uint8_t *data() const { return out_.data(); }
    size_t size() const { return out_.size(); }
    void clear() { out_.clear(); }

  private:
    void tag(uint32_t field, uint32_t wire) { varint(((uint64_t)field << 3) | wire); }

    void varint(uint64_t v) {
        do {
            uint8_t c = v & 0x7f;
            v >>= 7;
            if (v) c |= 0x80;
            out_.push_back(c);
        } while (v);
    }

    std::vector<uint8_t> out_;
};

}  // namespace pb
