// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Pointer updates on both paths ([MS-RDPBCGR] 2.2.9.1.1.4, 2.2.9.1.2.1.4 -
// 2.2.9.1.2.1.11). The first byte picks the decoder: with bit 7 set, the
// slow-path TS_POINTER_PDU payload; otherwise the fast-path updateCode in
// the low four bits. Whatever decodes must re-encode to the same value, and
// its shape is drawn.

#include <farland/base/assert.hpp>
#include <farland/proto/pointer.hpp>

#include "fuzz.hpp"

#include <span>

using namespace farland;
namespace ptr = farland::proto::pointer;

namespace {

void draw(const ptr::Update& update)
{
    const ptr::Shape* shape = nullptr;
    if (const auto* p = std::get_if<ptr::ColorPointer>(&update)) {
        shape = &p->shape;
    } else if (const auto* n = std::get_if<ptr::NewPointer>(&update)) {
        shape = &n->shape;
    } else if (const auto* l = std::get_if<ptr::LargePointer>(&update)) {
        shape = &l->shape;
    }
    if (shape != nullptr) {
        const auto pixels = ptr::shape_to_bgra(*shape);
        FARLAND_ASSERT(!pixels || pixels->size() == std::size_t{shape->width} * shape->height * 4U);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));
    if (input.empty()) {
        return 0;
    }
    const auto selector = std::to_integer<std::uint8_t>(input[0]);
    const bool slow_path = (selector & 0x80U) != 0;
    const auto code = static_cast<std::uint8_t>(selector & 0x0FU);
    Reader r(input.subspan(1));
    const auto decoded = slow_path ? ptr::decode_slow_path(r) : ptr::decode_fastpath(code, r);
    if (!decoded) {
        return 0;
    }

    Writer w;
    if (slow_path) {
        ptr::encode_slow_path(w, *decoded);
        Reader again(w.view());
        const auto round_trip = ptr::decode_slow_path(again);
        FARLAND_ASSERT(round_trip && *round_trip == *decoded);
    } else {
        FARLAND_ASSERT(ptr::encode_fastpath(w, *decoded) == code);
        FARLAND_ASSERT(w.size() == ptr::fastpath_size(*decoded));
        Reader again(w.view());
        const auto round_trip = ptr::decode_fastpath(code, again);
        FARLAND_ASSERT(round_trip && *round_trip == *decoded);
    }
    draw(*decoded);
    return 0;
}
