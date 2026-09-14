// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Conversions between the Windows clipboard formats of [MS-RDPECLIP] and
/// the MIME types of a Wayland desktop: text and HTML. Images are in
/// farland/codec/dib.hpp and png.hpp.
namespace farland::channels::clipboard {

/// text/plain;charset=utf-8 to CF_UNICODETEXT: UTF-16LE with CRLF line
/// ends and a terminating NUL. Invalid UTF-8 becomes U+FFFD.
[[nodiscard]] std::vector<std::byte> utf8_to_unicode_text(std::string_view text);
/// CF_UNICODETEXT to UTF-8 with LF line ends, up to the first NUL.
[[nodiscard]] std::string unicode_text_to_utf8(std::span<const std::byte> data);
/// CF_TEXT (and CF_OEMTEXT) to UTF-8 with LF line ends, up to the first NUL.
/// The client's ANSI code page is unknown; Windows-1252 is assumed.
[[nodiscard]] std::string ansi_text_to_utf8(std::span<const std::byte> data);
/// Local text to UTF-8: a UTF-16 byte order mark selects UTF-16, a UTF-8 one
/// is dropped, anything else is taken as UTF-8.
[[nodiscard]] std::string local_text_to_utf8(std::span<const std::byte> data);

/// text/html to "HTML Format" (CF_HTML: a header with byte offsets of the
/// document and the fragment, then the UTF-8 document). A document with a
/// <body> keeps it and has the fragment markers put inside; anything else is
/// wrapped as the fragment of a minimal document. `html` may carry a byte
/// order mark (local_text_to_utf8). The result ends with a NUL.
[[nodiscard]] std::vector<std::byte> html_to_cf_html(std::span<const std::byte> html);
/// "HTML Format" to text/html: the document between StartHTML and EndHTML
/// (or, when StartHTML is -1, the fragment), prefixed with a UTF-8 charset
/// declaration unless it declares one. Offsets outside the data, reversed
/// ranges and a missing header are errors.
[[nodiscard]] Result<std::string> cf_html_to_html(std::span<const std::byte> data);

}  // namespace farland::channels::clipboard
