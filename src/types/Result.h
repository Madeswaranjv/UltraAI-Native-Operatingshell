#pragma once

#include <string>
#include <variant>

namespace ultra::types {

/// Generic Result type with success or error variant.
///
/// Usage:
///   Result<int, std::string> r = Result<int, std::string>::ok(42);
///   if (r.isOk()) { use(r.value()); }
///
///   Result<int, std::string> r = Result<int, std::string>::err("fail");
///   if (r.isErr()) { handle(r.error()); }
template <typename T, typename E = std::string>
class Result {
 public:
  /// Create a success result.
  static Result ok(T value) { return Result(std::move(value)); }

  /// Create an error result.
  static Result err(E error) { return Result(ErrorTag{}, std::move(error)); }

  /// Check if this is a success result.
  bool isOk() const noexcept {
    return std::holds_alternative<T>(m_data);
  }

  /// Check if this is an error result.
  bool isErr() const noexcept {
    return std::holds_alternative<E>(m_data);
  }

  /// Access the success value. UB if isErr().
  const T& value() const& { return std::get<T>(m_data); }
  T& value() & { return std::get<T>(m_data); }
  T&& value() && { return std::get<T>(std::move(m_data)); }

  /// Access the error value. UB if isOk().
  const E& error() const& { return std::get<E>(m_data); }
  E& error() & { return std::get<E>(m_data); }
  E&& error() && { return std::get<E>(std::move(m_data)); }

  /// Get value or a default if error.
  T valueOr(T defaultValue) const {
    if (isOk()) return value();
    return defaultValue;
  }

 private:
  struct ErrorTag {};

  explicit Result(T value) : m_data(std::move(value)) {}
  Result(ErrorTag, E error) : m_data(std::move(error)) {}

  std::variant<T, E> m_data;
};

}  // namespace ultra::types
