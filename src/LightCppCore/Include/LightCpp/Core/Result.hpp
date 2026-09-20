// LightCppCore - C++20-compatible result sum type.
//
// A minimal stand-in for std::expected (which is C++23 and therefore above the
// project's C++20 baseline). Used by the Qt-facing layers to return either a
// payload or an error without exceptions or out-parameters.

#pragma once

#include <optional>
#include <utility>

namespace LightCpp::Core {

/// Holds either a value of type T (success) or an error of type E.
/// Construct through Result<T,E>::success(...) / ::failure(...).
template <typename T, typename E>
class [[nodiscard]] Result {
public:
    static Result success(T value) noexcept { return Result(std::move(value)); }
    static Result failure(E error) noexcept { return Result(error); }

    [[nodiscard]] bool hasValue() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] const T& value() const& noexcept { return *value_; }
    [[nodiscard]] T& value() & noexcept { return *value_; }
    [[nodiscard]] T takeValue() noexcept { return std::move(*value_); }

    [[nodiscard]] E error() const noexcept { return error_; }

private:
    explicit Result(T value) noexcept : value_(std::move(value)) {}
    explicit Result(E error) noexcept : error_(error) {}

    std::optional<T> value_;
    E error_{};
};

}  // namespace LightCpp::Core
