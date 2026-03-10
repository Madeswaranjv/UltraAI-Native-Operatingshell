#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ultra::ai {

using Sha256Hash = std::array<std::uint8_t, 32>;

class Sha256Accumulator {
 public:
  Sha256Accumulator();

  void update(const std::uint8_t* data, std::size_t size);
  void update(const std::string& data);
  bool updateFromFile(const std::filesystem::path& path, std::string& error);
  Sha256Hash finalize();

 private:
  void transformBlock(const std::uint8_t block[64]);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t bitLength_{0};
  std::size_t bufferLength_{0};
  bool finalized_{false};
};

Sha256Hash sha256OfBytes(const std::uint8_t* data, std::size_t size);
Sha256Hash sha256OfString(const std::string& data);
bool sha256OfFile(const std::filesystem::path& path,
                  Sha256Hash& outHash,
                  std::string& error);
std::string hashToHex(const Sha256Hash& hash);
bool hashesEqual(const Sha256Hash& left, const Sha256Hash& right);
Sha256Hash zeroHash();

}  // namespace ultra::ai

