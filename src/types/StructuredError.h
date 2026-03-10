#pragma once

#include "Timestamp.h"

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include "../ai/Hashing.h"
#include <algorithm>
#include <cctype>

namespace ultra::types {

/// StructuredError — deterministic structural identity
///
/// Structural fields (participate in equality & hashing):
///   - errorType
///   - suggestedAction (category)
///   - severity (bitwise equality)
///   - normalized message
///   - symbol
///   - sourceFile
///   - sourceLine
///   - errorDomain (optional)
///
/// Transient fields (MUST NOT affect equality or hash):
///   - timestamp, context, internalTrackingId, debugPayload,
///     branchId, versionId, runtime snapshots, stackTrace, etc.
///
/// Note: equality and hash are intentionally structural-only to guarantee
/// deterministic behavior across sessions, overlays, and persisted reloads.
struct StructuredError {
  // --- Structural fields (participate in equality/hash) ---
  std::string errorType;
  std::string suggestedAction;
  double severity{0.5};
  std::string message;
  std::string symbol;
  std::string sourceFile;
  int sourceLine{0};
  std::string errorDomain; // optional, kept for domain classification

  // --- Transient/runtime fields (excluded from equality/hash) ---
  std::vector<std::string> context;
  Timestamp timestamp;
  std::string internalTrackingId;
  std::string debugPayload;
  std::string branchId;
  std::string versionId;

  // -------------------------
  // Normalization helper
  // -------------------------
  // Pure deterministic normalization: trim and collapse whitespace
  static std::string normalizedMessage(const std::string &m) noexcept {
    std::string out;
    out.reserve(m.size());
    bool inWS = false;
    for (unsigned char uc : m) {
      if (std::isspace(uc)) {
        if (!inWS) {
          out.push_back(' ');
          inWS = true;
        }
      } else {
        out.push_back(static_cast<char>(uc));
        inWS = false;
      }
    }
    if (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
  }

  // -------------------------
  // Structural equality (transient fields excluded)
  // -------------------------
  bool operator==(const StructuredError& other) const noexcept {
    if (errorType != other.errorType) return false;
    if (suggestedAction != other.suggestedAction) return false;

    // Bitwise compare severity
    uint64_t aBits = 0, bBits = 0;
    static_assert(sizeof(aBits) == sizeof(severity), "double size mismatch");
    std::memcpy(&aBits, &severity, sizeof(aBits));
    std::memcpy(&bBits, &other.severity, sizeof(bBits));
    if (aBits != bBits) return false;

    if (normalizedMessage(message) != normalizedMessage(other.message)) return false;
    if (symbol != other.symbol) return false;
    if (sourceFile != other.sourceFile) return false;
    if (sourceLine != other.sourceLine) return false;
    if (errorDomain != other.errorDomain) return false;

    return true;
  }

  bool operator!=(const StructuredError& other) const noexcept {
    return !(*this == other);
  }

  // -------------------------
  // Hashing (structural fields only)
  // -------------------------
  std::size_t hash() const noexcept {
    // Build a canonical string of structural fields in alphabetical order and hash it with SHA-256
    std::string combined;
    combined.reserve(errorType.size() + suggestedAction.size() + message.size() + symbol.size() + sourceFile.size() + errorDomain.size() + 64);

    combined += errorType; combined.push_back('\n');
    combined += suggestedAction; combined.push_back('\n');

    uint64_t sevBits = 0;
    std::memcpy(&sevBits, &severity, sizeof(sevBits));
    combined += std::to_string(sevBits); combined.push_back('\n');

    combined += normalizedMessage(message); combined.push_back('\n');
    combined += symbol; combined.push_back('\n');
    combined += sourceFile; combined.push_back('\n');
    combined += std::to_string(sourceLine); combined.push_back('\n');
    combined += errorDomain; combined.push_back('\n');

    auto digest = ultra::ai::sha256OfString(combined);
    std::size_t seed = 0;
    for (size_t i = 0; i < sizeof(seed); ++i) {
      seed = (seed << 8) | digest[i];
    }
    return seed;
  }

  struct Hasher {
    std::size_t operator()(const StructuredError &e) const noexcept {
      return e.hash();
    }
  };

  // Deterministic structural ordering for ordered containers
  struct StructuralLess {
    bool operator()(const StructuredError &a, const StructuredError &b) const noexcept {
      if (a.errorType != b.errorType) return a.errorType < b.errorType;
      if (a.suggestedAction != b.suggestedAction) return a.suggestedAction < b.suggestedAction;
      uint64_t ab = 0, bb = 0;
      std::memcpy(&ab, &a.severity, sizeof(ab));
      std::memcpy(&bb, &b.severity, sizeof(bb));
      if (ab != bb) return ab < bb;
      auto an = normalizedMessage(a.message);
      auto bn = normalizedMessage(b.message);
      if (an != bn) return an < bn;
      if (a.symbol != b.symbol) return a.symbol < b.symbol;
      if (a.sourceFile != b.sourceFile) return a.sourceFile < b.sourceFile;
      if (a.sourceLine != b.sourceLine) return a.sourceLine < b.sourceLine;
      if (a.errorDomain != b.errorDomain) return a.errorDomain < b.errorDomain;
      return false;
    }
  };
};

}  // namespace ultra::types
