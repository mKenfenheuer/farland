// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// Fast-path output: server-to-client updates without the TPKT/X.224/MCS
/// framing, [MS-RDPBCGR] 2.2.9.1.2. Input lives in input.hpp.
namespace farland::proto::fastpath {

/// updateCode values, [MS-RDPBCGR] 2.2.9.1.2.1.
namespace update_code {
inline constexpr std::uint8_t orders = 0x0;
inline constexpr std::uint8_t bitmap = 0x1;
inline constexpr std::uint8_t palette = 0x2;
inline constexpr std::uint8_t synchronize = 0x3;
inline constexpr std::uint8_t surface_commands = 0x4;
inline constexpr std::uint8_t pointer_hidden = 0x5;
inline constexpr std::uint8_t pointer_default = 0x6;
inline constexpr std::uint8_t pointer_position = 0x8;
inline constexpr std::uint8_t color_pointer = 0x9;
inline constexpr std::uint8_t cached_pointer = 0xA;
inline constexpr std::uint8_t new_pointer = 0xB;
inline constexpr std::uint8_t large_pointer = 0xC;
}  // namespace update_code

enum class Fragmentation : std::uint8_t { single = 0, last = 1, first = 2, next = 3 };

/// Largest fragment farland puts in one PDU, as FreeRDP does: well below the
/// 15-bit fast-path length limit and friendly to every client.
inline constexpr std::size_t max_fragment_size = 0x3FFF - 20;

/// Appends one update as one or more fast-path output PDUs, fragmenting it
/// when it is larger than `max_fragment`. The caller must respect the
/// client's MultifragmentUpdate MaxRequestSize for the whole update.
void encode_update(Writer& w, std::uint8_t code, std::span<const std::byte> data,
                   std::size_t max_fragment = max_fragment_size);

/// One TS_FP_UPDATE as it appears in a PDU. `data` refers into the input.
struct UpdateFragment {
    std::uint8_t code = 0;
    Fragmentation fragmentation = Fragmentation::single;
    std::span<const std::byte> data;
};

/// Decodes a complete fast-path output PDU into its updates.
[[nodiscard]] Result<std::vector<UpdateFragment>> decode_output_pdu(Reader& pdu);

/// Reassembles fragmented updates (client side and tests).
class Reassembler {
public:
    explicit Reassembler(std::size_t max_update_size) : max_size_(max_update_size) {}

    struct Update {
        std::uint8_t code = 0;
        std::vector<std::byte> data;
    };

    /// Returns a complete update once its last fragment arrives.
    [[nodiscard]] Result<std::optional<Update>> add(const UpdateFragment& fragment);

private:
    std::size_t max_size_;
    std::optional<std::uint8_t> code_;
    std::vector<std::byte> buffer_;
};

}  // namespace farland::proto::fastpath
