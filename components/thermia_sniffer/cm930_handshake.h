// Pure C++ (no ESPHome dependency) so it can be unit-tested on a PC.
//
// CM930 gateway handshake, as recovered from Thermia Connect 2.1.105
// (device_client/cm/modbus_server15.py, cross-checked by compliance/tests/th930.py):
//   request : 0F 17 0730 0008 071C 0008 10 <16-byte challenge> CRC   (FC23)
//   reply   : 0F 17 10 <16-byte answer> CRC
//   answer  = XXTEA-encrypt(challenge, key), no padding, 16 bytes = 4 words, 19 rounds.
//   Bytes are packed into 32-bit words little-endian (as the Python xxtea package does);
//   the 16 output bytes are returned unchanged, i.e. as 8 big-endian Modbus registers.
//
// Verified byte-for-byte against two genuine gateway replies: the pair in
// CM930_HANDSHAKE_071C_0730.md and the 4 Oct 2026 13:49 cold-start capture on this bus.
//
// This file only COMPUTES. HandshakeChecker compares the computed answer with what the
// real gateway sends; nothing here transmits.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace cm930 {

// Firmware key words BC6725DC 41839788 ED7ACDF7 CA3393A4 (each serialized little-endian,
// giving the 16-byte key dc2567bc 88978341 f7cd7aed a49333ca).
static constexpr uint32_t HANDSHAKE_KEY[4] = {0xBC6725DCu, 0x41839788u, 0xED7ACDF7u, 0xCA3393A4u};

// XXTEA encrypt of exactly 16 bytes (n = 4 words, 6 + 52/4 = 19 rounds).
inline void handshake_response(const uint8_t challenge[16], uint8_t out[16]) {
  uint32_t v[4];
  for (int i = 0; i < 4; i++)
    v[i] = (uint32_t) challenge[4 * i] | ((uint32_t) challenge[4 * i + 1] << 8) |
           ((uint32_t) challenge[4 * i + 2] << 16) | ((uint32_t) challenge[4 * i + 3] << 24);
  const uint32_t *k = HANDSHAKE_KEY;
  const uint32_t DELTA = 0x9E3779B9u;
  const int n = 4;
  uint32_t sum = 0, z = v[n - 1], y;
  for (int rounds = 6 + 52 / n; rounds > 0; rounds--) {
    sum += DELTA;
    const uint32_t e = (sum >> 2) & 3;
    for (int p = 0; p < n; p++) {
      y = v[(p + 1) % n];
      const uint32_t mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(p & 3) ^ e] ^ z));
      v[p] += mx;
      z = v[p];
    }
  }
  for (int i = 0; i < 4; i++) {
    out[4 * i] = (uint8_t) v[i];
    out[4 * i + 1] = (uint8_t) (v[i] >> 8);
    out[4 * i + 2] = (uint8_t) (v[i] >> 16);
    out[4 * i + 3] = (uint8_t) (v[i] >> 24);
  }
}

// Watches CRC-valid frames for the gateway address and reports, for every handshake,
// whether a real gateway's reply equals the computed answer. Compute-and-compare only.
class HandshakeChecker {
 public:
  enum class Event : uint8_t {
    NONE,            // not a handshake frame
    REQUEST,         // challenge seen, answer computed (see challenge()/expected())
    REPLY_MATCH,     // gateway reply == computed answer
    REPLY_MISMATCH,  // gateway reply != computed answer (see got())
    REPLY_UNEXPECTED,// FC23 reply from the gateway with no pending request we could compute
    MALFORMED,       // FC23 to/from the gateway in a shape we do not recognise
  };

  explicit HandshakeChecker(uint8_t gw_addr = 0x0F) : addr_(gw_addr) {}

  // d/n: one CRC-valid frame (CRC included). For a REQUEST, prev_unanswered() tells
  // whether the previous request never got a reply.
  Event on_frame(const uint8_t *d, size_t n) {
    prev_unanswered_ = false;
    if (n < 5 || d[0] != addr_ || d[1] != 0x17) return Event::NONE;
    // Request: addr fc rs(2) rq(2) ws(2) wq(2) bc data crc(2)
    if (n >= 13 && n == (size_t) d[10] + 13) {
      read_start_ = rd_(d + 2);
      const uint16_t rq = rd_(d + 4), wq = rd_(d + 8);
      write_start_ = rd_(d + 6);
      if (rq != 8 || wq != 8 || d[10] != 16) {
        if (pending_) unanswered_++;
        pending_ = false;
        return Event::MALFORMED;
      }
      if (pending_) {
        unanswered_++;
        prev_unanswered_ = true;
      }
      canonical_ = (read_start_ == 0x0730 && write_start_ == 0x071C);
      memcpy(challenge_, d + 11, 16);
      handshake_response(challenge_, expected_);
      pending_ = true;
      requests_++;
      return Event::REQUEST;
    }
    // Reply: addr fc bc data crc(2)
    if (n == (size_t) d[2] + 5) {
      if (!pending_ || d[2] != 16) {
        pending_ = false;
        return Event::REPLY_UNEXPECTED;
      }
      pending_ = false;
      memcpy(got_, d + 3, 16);
      if (memcmp(got_, expected_, 16) == 0) {
        matches_++;
        return Event::REPLY_MATCH;
      }
      mismatches_++;
      return Event::REPLY_MISMATCH;
    }
    return Event::MALFORMED;
  }

  const uint8_t *challenge() const { return challenge_; }
  const uint8_t *expected() const { return expected_; }
  const uint8_t *got() const { return got_; }
  bool canonical() const { return canonical_; }  // 071C -> 0730 as in every capture
  uint16_t read_start() const { return read_start_; }
  uint16_t write_start() const { return write_start_; }
  bool pending() const { return pending_; }
  bool prev_unanswered() const { return prev_unanswered_; }
  uint32_t requests() const { return requests_; }
  uint32_t matches() const { return matches_; }
  uint32_t mismatches() const { return mismatches_; }
  uint32_t unanswered() const { return unanswered_; }  // excludes a request still pending

 private:
  static uint16_t rd_(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }
  uint8_t addr_;
  uint8_t challenge_[16]{}, expected_[16]{}, got_[16]{};
  uint16_t read_start_{0}, write_start_{0};
  bool canonical_{false}, pending_{false}, prev_unanswered_{false};
  uint32_t requests_{0}, matches_{0}, mismatches_{0}, unanswered_{0};
};

}  // namespace cm930
