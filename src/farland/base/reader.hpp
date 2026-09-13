// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace farland {

/// Bounds-checked, forward-only view over untrusted input. This is the only
/// way protocol code is allowed to look at received bytes.
///
/// A failed primitive read (`u8`, `bytes`, `sub`, ...) leaves the reader
/// unchanged. Composite decoders built on top may stop part-way through; after
/// an error, callers abandon the input.
class Reader {
public:
    constexpr Reader() noexcept = default;

    /// `base_offset` is the absolute position of `data[0]` in the outermost
    /// input, so that errors from nested readers point at the right byte.
    constexpr explicit Reader(std::span<const std::byte> data, std::size_t base_offset = 0) noexcept
        : data_(data), base_(base_offset)
    {
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] constexpr std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return pos_ == data_.size(); }
    /// Absolute offset of the next unread byte.
    [[nodiscard]] constexpr std::size_t offset() const noexcept { return base_ + pos_; }
    /// All bytes this reader covers, read or not.
    [[nodiscard]] constexpr std::span<const std::byte> data() const noexcept { return data_; }
    /// The unread bytes, without consuming them.
    [[nodiscard]] constexpr std::span<const std::byte> rest() const noexcept { return data_.subspan(pos_); }

    [[nodiscard]] Result<std::span<const std::byte>> bytes(std::size_t count) noexcept
    {
        if (count > remaining()) {
            return fail(Errc::truncated, "unexpected end of input", offset());
        }
        const auto out = data_.subspan(pos_, count);
        pos_ += count;
        return out;
    }

    /// Consumes `count` bytes and returns a reader limited to exactly those.
    [[nodiscard]] Result<Reader> sub(std::size_t count) noexcept
    {
        const std::size_t start = offset();
        FARLAND_TRY(const auto view, bytes(count));
        return Reader(view, start);
    }

    [[nodiscard]] Result<void> skip(std::size_t count) noexcept
    {
        FARLAND_TRY_VOID(bytes(count));
        return {};
    }

    [[nodiscard]] Result<std::uint8_t> peek_u8() const noexcept
    {
        if (empty()) {
            return fail(Errc::truncated, "unexpected end of input", offset());
        }
        return std::to_integer<std::uint8_t>(data_[pos_]);
    }

    [[nodiscard]] Result<std::uint8_t> u8() noexcept { return read_le<std::uint8_t>(); }
    [[nodiscard]] Result<std::uint16_t> u16le() noexcept { return read_le<std::uint16_t>(); }
    [[nodiscard]] Result<std::uint32_t> u32le() noexcept { return read_le<std::uint32_t>(); }
    [[nodiscard]] Result<std::uint64_t> u64le() noexcept { return read_le<std::uint64_t>(); }
    [[nodiscard]] Result<std::uint16_t> u16be() noexcept { return read_be<std::uint16_t>(); }
    [[nodiscard]] Result<std::uint32_t> u32be() noexcept { return read_be<std::uint32_t>(); }

    template <std::unsigned_integral T>
    [[nodiscard]] Result<T> read_le() noexcept
    {
        FARLAND_TRY(const auto raw, bytes(sizeof(T)));
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value = static_cast<T>(value | static_cast<T>(std::to_integer<T>(raw[i]) << (8U * i)));
        }
        return value;
    }

    template <std::unsigned_integral T>
    [[nodiscard]] Result<T> read_be() noexcept
    {
        FARLAND_TRY(const auto raw, bytes(sizeof(T)));
        T value = 0;
        for (const std::byte b : raw) {
            value = static_cast<T>(static_cast<T>(value << 8U) | std::to_integer<T>(b));
        }
        return value;
    }

    /// Fails with `Errc::trailing_data` unless every byte has been consumed.
    /// `what` names the structure that should have filled the input.
    [[nodiscard]] Result<void> expect_end(std::string_view what) const noexcept
    {
        if (!empty()) {
            return fail(Errc::trailing_data, what, offset());
        }
        return {};
    }

    /// An error located at the next unread byte.
    [[nodiscard]] Error error(Errc code, std::string_view what) const noexcept { return Error{code, what, offset()}; }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    std::size_t base_ = 0;
};

}  // namespace farland
