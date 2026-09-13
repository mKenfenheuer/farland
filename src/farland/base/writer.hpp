// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/assert.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace farland {

/// Growable output buffer for encoding PDUs. Encoding our own data cannot fail
/// on bad input, so writes are infallible; misuse (patching past the end,
/// values out of range for a field) is a programmer error and asserts.
class Writer {
public:
    Writer() = default;
    explicit Writer(std::size_t reserve) { buf_.reserve(reserve); }

    void u8(std::uint8_t value) { buf_.push_back(std::byte{value}); }
    void u16le(std::uint16_t value) { write_le(value); }
    void u32le(std::uint32_t value) { write_le(value); }
    void u64le(std::uint64_t value) { write_le(value); }
    void u16be(std::uint16_t value) { write_be(value); }
    void u32be(std::uint32_t value) { write_be(value); }

    void bytes(std::span<const std::byte> data) { buf_.insert(buf_.end(), data.begin(), data.end()); }
    void zeros(std::size_t count) { buf_.resize(buf_.size() + count); }

    template <std::unsigned_integral T>
    void write_le(T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            buf_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * i))));
        }
    }

    template <std::unsigned_integral T>
    void write_be(T value)
    {
        for (std::size_t i = sizeof(T); i-- > 0;) {
            buf_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * i))));
        }
    }

    /// Overwrite an earlier placeholder, typically a length field that
    /// precedes content whose size was not known yet.
    void patch_u8(std::size_t pos, std::uint8_t value) { patch_le(pos, value); }
    void patch_u16le(std::size_t pos, std::uint16_t value) { patch_le(pos, value); }
    void patch_u32le(std::size_t pos, std::uint32_t value) { patch_le(pos, value); }
    void patch_u16be(std::size_t pos, std::uint16_t value) { patch_be(pos, value); }
    void patch_u32be(std::size_t pos, std::uint32_t value) { patch_be(pos, value); }

    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] std::span<const std::byte> view() const noexcept { return buf_; }
    [[nodiscard]] std::vector<std::byte> take() && noexcept { return std::move(buf_); }

private:
    template <std::unsigned_integral T>
    void patch_le(std::size_t pos, T value)
    {
        FARLAND_ASSERT(pos <= buf_.size() && sizeof(T) <= buf_.size() - pos);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            buf_[pos + i] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * i)));
        }
    }

    template <std::unsigned_integral T>
    void patch_be(std::size_t pos, T value)
    {
        FARLAND_ASSERT(pos <= buf_.size() && sizeof(T) <= buf_.size() - pos);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            buf_[pos + i] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * (sizeof(T) - 1U - i))));
        }
    }

    std::vector<std::byte> buf_;
};

}  // namespace farland
