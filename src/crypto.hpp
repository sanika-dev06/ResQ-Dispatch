// crypto.hpp - SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256 and secure random bytes.
// Header-only, no third-party dependency. Verified against published test vectors
// (see tests/test_core.cpp).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Random bytes: MSVC's std::random_device is backed by the Windows secure RNG (rand_s), so it is used
// there. Other Windows compilers (MinGW) call BCryptGenRandom; elsewhere /dev/urandom is read.
// Define RESQ_USE_STD_RANDOM to force the std::random_device path on any platform.
#if defined(_MSC_VER) || defined(RESQ_USE_STD_RANDOM)
#include <random>
#define RESQ_RANDOM_STD 1
#elif defined(_WIN32)
// Declared by hand so <windows.h> (and its macros) never leaks into Crow's headers.
extern "C" __declspec(dllimport) long __stdcall BCryptGenRandom(void*, unsigned char*, unsigned long, unsigned long);
#endif

namespace crypto {

using Bytes = std::vector<uint8_t>;
using Digest = std::array<uint8_t, 32>;

class Sha256 {
 public:
  Sha256() { reset(); }

  void reset() {
    h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    total_ = 0;
    bufLen_ = 0;
  }

  void update(const uint8_t* data, size_t n) {
    total_ += n;
    while (n > 0) {
      size_t take = std::min(n, static_cast<size_t>(64) - bufLen_);
      std::memcpy(buf_ + bufLen_, data, take);
      bufLen_ += take;
      data += take;
      n -= take;
      if (bufLen_ == 64) {
        block(buf_);
        bufLen_ = 0;
      }
    }
  }
  void update(const std::string& s) { update(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
  void update(const Bytes& b) { update(b.data(), b.size()); }

  Digest finish() {
    const uint64_t bits = total_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    Digest out{};
    for (int i = 0; i < 8; ++i) {
      out[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
    return out;
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
             uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }

  std::array<uint32_t, 8> h_{};
  uint64_t total_ = 0;
  uint8_t buf_[64] = {0};
  size_t bufLen_ = 0;
};

inline std::string toHex(const uint8_t* d, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(digits[d[i] >> 4]);
    s.push_back(digits[d[i] & 15]);
  }
  return s;
}
inline std::string toHex(const Bytes& b) { return toHex(b.data(), b.size()); }
inline std::string toHex(const Digest& d) { return toHex(d.data(), d.size()); }

inline Bytes fromHex(const std::string& s) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (s.size() % 2 != 0) throw std::invalid_argument("odd hex length");
  Bytes out;
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    int hi = val(s[i]), lo = val(s[i + 1]);
    if (hi < 0 || lo < 0) throw std::invalid_argument("bad hex digit");
    out.push_back(static_cast<uint8_t>(hi * 16 + lo));
  }
  return out;
}

inline std::string sha256Hex(const std::string& s) {
  Sha256 h;
  h.update(s);
  return toHex(h.finish());
}

// HMAC-SHA256 with the two pad states precomputed so PBKDF2 needs only 2
// compression-function calls per iteration.
class Hmac {
 public:
  explicit Hmac(const Bytes& key) {
    Bytes k = key;
    if (k.size() > 64) {
      Sha256 h;
      h.update(k);
      Digest d = h.finish();
      k.assign(d.begin(), d.end());
    }
    k.resize(64, 0);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
      ipad[i] = k[i] ^ 0x36;
      opad[i] = k[i] ^ 0x5c;
    }
    inner0_.update(ipad, 64);
    outer0_.update(opad, 64);
  }
  Digest mac(const uint8_t* msg, size_t n) const {
    Sha256 in = inner0_;
    in.update(msg, n);
    Digest ih = in.finish();
    Sha256 out = outer0_;
    out.update(ih.data(), ih.size());
    return out.finish();
  }

 private:
  Sha256 inner0_, outer0_;
};

inline Digest hmacSha256(const Bytes& key, const Bytes& msg) {
  Hmac h(key);
  return h.mac(msg.data(), msg.size());
}

// PBKDF2-HMAC-SHA256, 32-byte output (one block).
inline Digest pbkdf2(const std::string& password, const Bytes& salt, uint32_t iterations) {
  Hmac h(Bytes(password.begin(), password.end()));
  Bytes first = salt;
  first.push_back(0); first.push_back(0); first.push_back(0); first.push_back(1);
  Digest u = h.mac(first.data(), first.size());
  Digest t = u;
  for (uint32_t i = 1; i < iterations; ++i) {
    u = h.mac(u.data(), u.size());
    for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
  }
  return t;
}

inline std::string pbkdf2Hex(const std::string& password, const Bytes& salt, uint32_t iterations) {
  return toHex(pbkdf2(password, salt, iterations));
}

// Operating-system CSPRNG: BCryptGenRandom on Windows, /dev/urandom elsewhere.
inline Bytes randomBytes(size_t n) {
  Bytes out(n);
#if defined(RESQ_RANDOM_STD)
  std::random_device rd;
  for (size_t i = 0; i < n;) {
    const uint32_t v = static_cast<uint32_t>(rd());
    for (int k = 0; k < 4 && i < n; ++k, ++i) out[i] = static_cast<uint8_t>(v >> (8 * k));
  }
#elif defined(_WIN32)
  // BCRYPT_USE_SYSTEM_PREFERRED_RNG = 0x00000002
  if (BCryptGenRandom(nullptr, out.data(), static_cast<unsigned long>(n), 0x00000002) != 0)
    throw std::runtime_error("BCryptGenRandom failed");
#else
  std::ifstream f("/dev/urandom", std::ios::binary);
  if (!f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n)))
    throw std::runtime_error("cannot read /dev/urandom");
#endif
  return out;
}
inline std::string randomHex(size_t n) { return toHex(randomBytes(n)); }

// Compare two strings without an early exit on the first mismatch.
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace crypto
