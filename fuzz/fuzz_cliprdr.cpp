// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// cliprdr and the clipboard format conversions. The first byte picks the mode:
// - 0, 1: one clipboard PDU with long (0) or short (1) format names. What
//   decodes must re-encode, and the re-encoding must be stable.
// - 2: a Packed File List, the same way.
// - 3: text and "HTML Format" conversions; wrapped HTML must parse back.
// - 4: a packed DIB; what decodes must survive DIB, BMP and PNG round trips.
// - 5: a PNG; what decodes must survive a PNG round trip.
// - 6: a ClipboardServer that announced text, HTML and files, fed records of
//   [u16le length][client PDU]. Requests are answered, the client's formats
//   requested; every output PDU must decode.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/clipboard_formats.hpp>
#include <farland/channels/cliprdr.hpp>
#include <farland/channels/cliprdr_server.hpp>
#include <farland/codec/dib.hpp>
#include <farland/codec/png.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>
#include <variant>
#include <vector>

using namespace farland;
namespace cliprdr = farland::channels::cliprdr;
namespace clipboard = farland::channels::clipboard;

namespace {

/// Images the round trips may spend time on.
constexpr std::uint64_t max_fuzz_pixels = std::uint64_t{1} << 16U;

void pdu_mode(std::span<const std::byte> input, bool long_names)
{
    const auto decoded = cliprdr::decode(input, long_names);
    if (!decoded) {
        return;
    }
    const auto bytes = cliprdr::encode(*decoded, long_names);
    const auto again = cliprdr::decode(bytes, long_names);
    FARLAND_ASSERT(again.has_value());
    FARLAND_ASSERT(cliprdr::encode(*again, long_names) == bytes);
}

void file_list_mode(std::span<const std::byte> input)
{
    const auto files = cliprdr::decode_file_list(input);
    if (!files) {
        return;
    }
    FARLAND_ASSERT(files->size() <= cliprdr::max_file_list_items);
    const auto bytes = cliprdr::encode_file_list(*files);
    const auto again = cliprdr::decode_file_list(bytes);
    FARLAND_ASSERT(again.has_value());
    FARLAND_ASSERT(cliprdr::encode_file_list(*again) == bytes);
}

void text_mode(std::span<const std::byte> input)
{
    static_cast<void>(clipboard::cf_html_to_html(input));
    const auto wrapped = clipboard::html_to_cf_html(input);
    FARLAND_ASSERT(clipboard::cf_html_to_html(wrapped).has_value());
    const auto text = clipboard::unicode_text_to_utf8(input);
    FARLAND_ASSERT(clipboard::unicode_text_to_utf8(clipboard::utf8_to_unicode_text(text)) == text);
    static_cast<void>(clipboard::ansi_text_to_utf8(input));
    static_cast<void>(clipboard::local_text_to_utf8(input));
}

void dib_mode(std::span<const std::byte> input)
{
    const auto image = codec::decode_dib(input);
    if (!image) {
        return;
    }
    const auto v5 = codec::decode_dib(codec::encode_dib(*image, codec::DibHeader::v5));
    FARLAND_ASSERT(v5.has_value() && *v5 == *image);
    const auto info = codec::decode_dib(codec::encode_dib(*image, codec::DibHeader::info));
    FARLAND_ASSERT(info.has_value() && (!image->opaque() || *info == *image));
    const auto bmp = codec::dib_to_bmp(input);
    FARLAND_ASSERT(bmp.has_value());
    const auto packed = codec::bmp_to_dib(*bmp);
    FARLAND_ASSERT(packed.has_value());
    const auto from_bmp = codec::decode_dib(*packed);
    FARLAND_ASSERT(from_bmp.has_value() && *from_bmp == *image);
    if (codec::png_supported() && std::uint64_t{image->width} * image->height <= max_fuzz_pixels) {
        const auto png = codec::encode_png(*image);
        FARLAND_ASSERT(png.has_value());
        const auto back = codec::decode_png(*png);
        FARLAND_ASSERT(back.has_value() && *back == *image);
    }
}

void png_mode(std::span<const std::byte> input)
{
    const auto image = codec::decode_png(input, max_fuzz_pixels);
    if (!image) {
        return;
    }
    const auto png = codec::encode_png(*image);
    FARLAND_ASSERT(png.has_value());
    const auto back = codec::decode_png(*png);
    FARLAND_ASSERT(back.has_value() && *back == *image);
}

void check_output(cliprdr::ClipboardServer& server)
{
    for (const auto& message : server.take_output()) {
        FARLAND_ASSERT(message.size() >= cliprdr::header_size);
        FARLAND_ASSERT(cliprdr::decode(message, server.has(cliprdr::general_flag::use_long_format_names)).has_value());
    }
}

void server_mode(Reader& r)
{
    using namespace std::chrono_literals;
    cliprdr::ClipboardServer server;
    auto now = cliprdr::ClipboardServer::Clock::time_point{} + 1h;
    server.start();
    server.announce(
        {{cliprdr::cf::unicode_text, ""}, {0xD001, cliprdr::html_format}, {0xD002, cliprdr::file_group_descriptor_w}});
    check_output(server);
    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length) {
            break;
        }
        const auto message = r.bytes(std::min<std::size_t>(*length, r.remaining())).value();
        const auto received = server.receive(message, now);
        if (!received) {
            FARLAND_ASSERT(server.failed());
            FARLAND_ASSERT(!server.receive(message, now).has_value());
            break;
        }
        while (auto event = server.poll_event()) {
            if (const auto* data = std::get_if<cliprdr::server_event::DataRequested>(&*event)) {
                const std::vector<std::byte> answer(data->format_id % 7);
                server.answer(data->id, answer);
            } else if (const auto* file = std::get_if<cliprdr::server_event::FileContentsRequested>(&*event)) {
                const std::vector<std::byte> answer(file->request.flags == cliprdr::file_contents::size ? 8 : 3);
                server.answer(file->id, answer);
            } else if (const auto* list = std::get_if<cliprdr::server_event::RemoteFormatList>(&*event)) {
                if (!list->formats.empty()) {
                    static_cast<void>(server.request_data(list->formats.front().id, now));
                } else {
                    static_cast<void>(server.request_file_contents(0, cliprdr::file_contents::size, 0, 0, 1U, now));
                }
            }
        }
        check_output(server);
        now += 10s;
        server.tick(now);
        check_output(server);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto mode = r.u8();
    if (!mode) {
        return 0;
    }
    const auto input = r.rest();
    switch (*mode % 7) {
    case 0:
    case 1:
        pdu_mode(input, *mode % 7 == 0);
        break;
    case 2:
        file_list_mode(input);
        break;
    case 3:
        text_mode(input);
        break;
    case 4:
        dib_mode(input);
        break;
    case 5:
        png_mode(input);
        break;
    default:
        server_mode(r);
        break;
    }
    return 0;
}
