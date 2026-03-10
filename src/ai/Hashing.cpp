#include "Hashing.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include<iostream>
namespace ultra::ai {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::array<std::uint32_t, 8> kInitialState{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

constexpr std::uint32_t rotr(const std::uint32_t value,
                             const std::uint32_t bits) {
  return (value >> bits) | (value << (32U - bits));
}

constexpr std::uint32_t ch(const std::uint32_t x,
                           const std::uint32_t y,
                           const std::uint32_t z) {
  return (x & y) ^ (~x & z);
}

constexpr std::uint32_t maj(const std::uint32_t x,
                            const std::uint32_t y,
                            const std::uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t bigSigma0(const std::uint32_t x) {
  return rotr(x, 2U) ^ rotr(x, 13U) ^ rotr(x, 22U);
}

constexpr std::uint32_t bigSigma1(const std::uint32_t x) {
  return rotr(x, 6U) ^ rotr(x, 11U) ^ rotr(x, 25U);
}

constexpr std::uint32_t smallSigma0(const std::uint32_t x) {
  return rotr(x, 7U) ^ rotr(x, 18U) ^ (x >> 3U);
}

constexpr std::uint32_t smallSigma1(const std::uint32_t x) {
  return rotr(x, 17U) ^ rotr(x, 19U) ^ (x >> 10U);
}

}  // namespace

Sha256Accumulator::Sha256Accumulator() : state_(kInitialState) {}

void Sha256Accumulator::update(const std::uint8_t* data, std::size_t size) {
  if (finalized_) {
    throw std::logic_error("SHA-256 accumulator finalized");
  }
  if (data == nullptr || size == 0U) {
    return;
  }

  bitLength_ += static_cast<std::uint64_t>(size) * 8ULL;
  std::size_t offset = 0U;
  while (offset < size) {
    const std::size_t available = 64U - bufferLength_;
    const std::size_t toCopy = std::min(available, size - offset);
    std::copy_n(data + offset, toCopy, buffer_.begin() + static_cast<long long>(bufferLength_));
    bufferLength_ += toCopy;
    offset += toCopy;

    if (bufferLength_ == 64U) {
      transformBlock(buffer_.data());
      bufferLength_ = 0U;
    }
  }
}

void Sha256Accumulator::update(const std::string& data) {
  const auto* bytes =
      reinterpret_cast<const std::uint8_t*>(data.data());
  update(bytes, data.size());
}

bool Sha256Accumulator::updateFromFile(const std::filesystem::path& path,
                                       std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Failed to open file for hashing: " + path.string();
    return false;
  }

  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize readBytes = input.gcount();
    if (readBytes <= 0) {
      continue;
    }
    update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
           static_cast<std::size_t>(readBytes));
  }

  if (input.bad()) {
    error = "I/O error while hashing file: " + path.string();
    return false;
  }

  return true;
}

Sha256Hash Sha256Accumulator::finalize() {
  if (finalized_) {
    Sha256Hash already{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
      const std::uint32_t word = state_[i];
      already[i * 4U + 0U] = static_cast<std::uint8_t>((word >> 24U) & 0xFFU);
      already[i * 4U + 1U] = static_cast<std::uint8_t>((word >> 16U) & 0xFFU);
      already[i * 4U + 2U] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
      already[i * 4U + 3U] = static_cast<std::uint8_t>(word & 0xFFU);
    }
    return already;
  }

  buffer_[bufferLength_++] = 0x80U;
  if (bufferLength_ > 56U) {
    while (bufferLength_ < 64U) {
      buffer_[bufferLength_++] = 0U;
    }
    transformBlock(buffer_.data());
    bufferLength_ = 0U;
  }
  while (bufferLength_ < 56U) {
    buffer_[bufferLength_++] = 0U;
  }

  for (int i = 7; i >= 0; --i) {
    buffer_[static_cast<std::size_t>(63 - i)] =
        static_cast<std::uint8_t>((bitLength_ >> (static_cast<std::uint64_t>(i) * 8ULL)) &
                                  0xFFULL);
  }

  transformBlock(buffer_.data());
  finalized_ = true;

  Sha256Hash digest{};
  for (std::size_t i = 0; i < state_.size(); ++i) {
    const std::uint32_t word = state_[i];
    digest[i * 4U + 0U] = static_cast<std::uint8_t>((word >> 24U) & 0xFFU);
    digest[i * 4U + 1U] = static_cast<std::uint8_t>((word >> 16U) & 0xFFU);
    digest[i * 4U + 2U] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
    digest[i * 4U + 3U] = static_cast<std::uint8_t>(word & 0xFFU);
  }
  return digest;
}

void Sha256Accumulator::transformBlock(const std::uint8_t block[64]) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t i = 0; i < 16U; ++i) {
    const std::size_t base = i * 4U;
    words[i] = (static_cast<std::uint32_t>(block[base + 0U]) << 24U) |
               (static_cast<std::uint32_t>(block[base + 1U]) << 16U) |
               (static_cast<std::uint32_t>(block[base + 2U]) << 8U) |
               static_cast<std::uint32_t>(block[base + 3U]);
  }

  for (std::size_t i = 16U; i < 64U; ++i) {
    words[i] = smallSigma1(words[i - 2U]) + words[i - 7U] +
               smallSigma0(words[i - 15U]) + words[i - 16U];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64U; ++i) {
    const std::uint32_t temp1 =
        h + bigSigma1(e) + ch(e, f, g) + kRoundConstants[i] + words[i];
    const std::uint32_t temp2 = bigSigma0(a) + maj(a, b, c);

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

Sha256Hash sha256OfBytes(const std::uint8_t* data, const std::size_t size) {
  Sha256Accumulator accumulator;
  accumulator.update(data, size);
  return accumulator.finalize();
}

Sha256Hash sha256OfString(const std::string& data) {
  return sha256OfBytes(reinterpret_cast<const std::uint8_t*>(data.data()),
                       data.size());
}

bool sha256OfFile(const std::filesystem::path& path,
                  Sha256Hash& outHash,
                  std::string& error) {
  Sha256Accumulator accumulator;
  if (!accumulator.updateFromFile(path, error)) {
    return false;
  }
  outHash = accumulator.finalize();
  return true;
}

std::string hashToHex(const Sha256Hash& hash) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const std::uint8_t byte : hash) {
    stream << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return stream.str();
}

bool hashesEqual(const Sha256Hash& left, const Sha256Hash& right) {
  return left == right;
}

Sha256Hash zeroHash() {
  return Sha256Hash{};
}
}  // namespace ultra::ai

