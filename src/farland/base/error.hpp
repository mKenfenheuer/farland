// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace farland {

/// Category of a decoding failure. Deliberately coarse: code branches on the
/// category, people read `Error::what` and `Error::offset`.
enum class Errc : std::uint8_t {
    truncated,       ///< Input ended before the structure did.
    invalid_value,   ///< A field holds a value the specification does not allow.
    invalid_length,  ///< A length field disagrees with its container or the specification.
    unsupported,     ///< Valid per the specification, but farland does not implement it.
    limit_exceeded,  ///< A size or count is above a farland resource limit.
    trailing_data,   ///< Bytes are left over where a structure must fill its container.
    io,              ///< A file or other local resource could not be read or written.
};

[[nodiscard]] std::string_view to_string(Errc code) noexcept;

/// A decoding error. Every byte a peer sends is untrusted, so parsers report
/// problems through `Result` and never through exceptions or assertions.
struct Error {
    Errc code{};
    /// Short description. Must refer to static storage (a string literal).
    std::string_view what;
    /// Absolute byte offset in the outermost input where the problem was found.
    std::size_t offset = 0;

    [[nodiscard]] std::string message() const;
    friend bool operator==(const Error&, const Error&) = default;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(Errc code, std::string_view what, std::size_t offset = 0) noexcept
{
    return std::unexpected(Error{code, what, offset});
}

}  // namespace farland

// FARLAND_TRY takes a declaration as `lhs`, which cannot be parenthesized.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define FARLAND_CONCAT_IMPL(a, b) a##b
#define FARLAND_CONCAT(a, b) FARLAND_CONCAT_IMPL(a, b)

/// Evaluates `expr` (a `Result<T>`). On error, returns the error from the
/// enclosing function; otherwise move-assigns the value to `lhs`, which may be
/// a declaration:
///
///     FARLAND_TRY(const auto length, per::read_length(r));
#define FARLAND_TRY(lhs, expr) FARLAND_TRY_IMPL(lhs, expr, FARLAND_CONCAT(farland_try_, __COUNTER__))
#define FARLAND_TRY_IMPL(lhs, expr, tmp)                                                                               \
    auto tmp = (expr);                                                                                                 \
    if (!tmp.has_value()) [[unlikely]] {                                                                               \
        return std::unexpected(std::move(tmp).error());                                                                \
    }                                                                                                                  \
    lhs = std::move(*tmp)

/// FARLAND_TRY for `Result<void>`.
#define FARLAND_TRY_VOID(expr) FARLAND_TRY_VOID_IMPL(expr, FARLAND_CONCAT(farland_try_, __COUNTER__))
#define FARLAND_TRY_VOID_IMPL(expr, tmp)                                                                               \
    if (auto tmp = (expr); !tmp.has_value()) [[unlikely]] {                                                            \
        return std::unexpected(std::move(tmp).error());                                                                \
    }                                                                                                                  \
    static_cast<void>(0)
// NOLINTEND(bugprone-macro-parentheses)
