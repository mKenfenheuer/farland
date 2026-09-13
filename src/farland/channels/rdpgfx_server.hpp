// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpgfx.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

/// The server side of the Graphics Pipeline ([MS-RDPEGFX] 3.2) as a sans-IO
/// state machine over uncompressed RDPGFX PDUs: capability negotiation,
/// surfaces, frames and their acknowledgements, and the bitmap cache map.
///
/// Contract for callers:
/// - Feed the bytes of the Graphics DVC to `receive()`: client-to-server
///   PDUs are not compressed ([MS-RDPEGFX] 2.1). One call may carry several
///   PDUs back to back, and a PDU may be split across calls.
/// - After every `receive()` and every command, send each PDU from
///   `take_output()`, in order, wrapped in RDP_SEGMENTED_DATA (ZGFX). Several
///   consecutive PDUs may share one RDP_SEGMENTED_DATA (a whole frame, say),
///   but one PDU must never be split across two ([MS-RDPEGFX] 2.1).
/// - Then handle every event from `poll_event()` before issuing further
///   commands: a repeated Caps Advertise resets surfaces and the cache.
/// - Commands check their preconditions with FARLAND_ASSERT: they are
///   programming errors, not peer behaviour. Commands before `Ready` assert;
///   after `Failed` they do nothing (and return 0 / nullopt), so a failure
///   inside `receive()` never turns a pending command into a crash.
namespace farland::channels::rdpgfx {

struct GfxServerConfig {
    /// Highest capability set version to confirm (docs/PLAN.md §3.4).
    std::uint32_t max_version = cap_version::v10_7;
    /// Which H.264 modes the server can encode. They decide what `Negotiated`
    /// allows, and whether AVC420_ENABLED is confirmed for 8.1.
    bool avc420 = true;
    bool avc444 = true;
    bool avc444v2 = true;
};

/// What client and server agreed on (docs/PLAN.md §3.2 rule 5). Only the
/// confirmed flags apply to the connection ([MS-RDPEGFX] 1.7).
struct Negotiated {
    std::uint32_t version = 0;
    /// The flags the client advertised for `version`.
    std::uint32_t client_flags = 0;
    /// The flags sent in the Caps Confirm: the client's flags that
    /// [MS-RDPEGFX] 2.2.3 defines for `version`, minus AVC420_ENABLED when
    /// the server will not send AVC420. Never a bit the client did not set.
    std::uint32_t flags = 0;

    /// 8.0/8.1 THINCLIENT: RemoteFX (CAVIDEO) instead of Progressive.
    bool thin_client = false;
    /// 16 MB bitmap cache (THINCLIENT, SMALL_CACHE, or version 10.3).
    bool small_cache = false;
    /// H.264 modes the server may send in WireToSurface1. AVC420 needs 8.1
    /// with AVC420_ENABLED, or 10.4+ without AVC_DISABLED (which adds "YUV420
    /// in the same frame as other codecs", 2.2.3.7). For 10.0 to 10.3 the
    /// specification only promises YUV444, so there use AVC444 (LC=1 carries
    /// just the 4:2:0 luma frame).
    bool avc420 = false;
    bool avc444 = false;
    bool avc444v2 = false;
    /// 10.3+ AVC_THINCLIENT: the client prefers AVC444. A hint only.
    bool avc_thin_client = false;
    /// MapSurfaceToScaledOutput/Window allowed: 10.7 without SCALEDMAP_DISABLE.
    bool scaled_output = false;
    /// The client may send QoE Frame Acknowledge (10.0, 10.2 and later).
    bool qoe = false;
    std::uint16_t max_cache_slots = 0;
    std::uint32_t max_cache_bytes = 0;

    /// Whether `codec_id` may be used in WireToSurface1 (Progressive: in
    /// WireToSurface2).
    [[nodiscard]] bool allows(std::uint16_t codec_id) const noexcept;

    friend bool operator==(const Negotiated&, const Negotiated&) = default;
};

/// Selects the highest capability set both sides support and derives
/// `Negotiated` from it; nullopt if the client offered none. Preference:
/// 10.7, 10.6, 10.6 (errata value), 10.5, 10.4, 10.3, 10.2, 10.1, 10.0, 8.1,
/// 8.0, each only up to `config.max_version`. 11.x is never selected.
[[nodiscard]] std::optional<Negotiated> negotiate(const CapsAdvertise& advertise, const GfxServerConfig& config);

enum class GfxState : std::uint8_t { wait_caps_advertise, ready, failed };

namespace event {
/// Caps Confirm is queued; commands may follow. `reset`: the client sent Caps
/// Advertise again ([MS-RDPEGFX] 3.2.5.18), and everything sent before is
/// gone on its side: surfaces, cache entries, frames. Start over with
/// ResetGraphics.
struct Ready {
    Negotiated negotiated;
    bool reset = false;
};
/// A Frame Acknowledge. `known`: `frame_id` was in flight; acks for other
/// frame ids are ignored apart from the queue depth, like FreeRDP does.
struct FrameAcked {
    std::uint32_t frame_id = 0;
    /// Bytes still queued at the client, 0 when unknown, or
    /// `suspend_frame_acknowledgement`.
    std::uint32_t queue_depth = 0;
    std::uint32_t total_frames_decoded = 0;
    bool known = false;
};
/// A QoE Frame Acknowledge, for statistics only ([MS-RDPEGFX] 3.2.5.21).
struct QoeFrameAcked {
    QoeFrameAcknowledge qoe;
};
/// A Cache Import Offer arrived and was answered; see `GfxServer`.
struct CacheImportOffered {
    std::size_t offered = 0;
    std::size_t imported = 0;
};
/// A protocol violation. The channel is unusable; close it.
struct Failed {
    std::string reason;
};
}  // namespace event

using GfxEvent =
    std::variant<event::Ready, event::FrameAcked, event::QoeFrameAcked, event::CacheImportOffered, event::Failed>;

/// See the namespace comment for the calling contract.
///
/// Frame acknowledgement ([MS-RDPEGFX] 3.2.5.13): a frame is in flight from
/// its End Frame until it is acknowledged. An ack for frame N also settles
/// every older frame still in flight (cumulative, as FreeRDP's and macRDP's
/// servers count), so a skipped ack cannot stall the window. queueDepth
/// SUSPEND_FRAME_ACKNOWLEDGEMENT empties the window and stops tracking until
/// the client acknowledges with any other queueDepth.
///
/// Cache Import Offer ([MS-RDPEGFX] 3.2.5.16): answered with an empty Cache
/// Import Reply. Importing entries only pays off when the server can compute
/// the same cache keys as the client's persistent cache, which farland does
/// not do yet; importing nothing is valid (3.2.5.17).
class GfxServer {
public:
    explicit GfxServer(GfxServerConfig config = {});

    void receive(std::span<const std::byte> bytes);
    /// Complete PDUs, one vector each, in the order they must be sent.
    [[nodiscard]] std::vector<std::vector<std::byte>> take_output();
    [[nodiscard]] std::optional<GfxEvent> poll_event();

    [[nodiscard]] GfxState state() const noexcept { return state_; }
    [[nodiscard]] bool ready() const noexcept { return state_ == GfxState::ready; }
    [[nodiscard]] std::string_view failure_reason() const noexcept { return failure_; }
    /// Set once Ready.
    [[nodiscard]] const std::optional<Negotiated>& negotiated() const noexcept { return negotiated_; }

    // Output layout ------------------------------------------------------

    /// [MS-RDPEGFX] 2.2.2.14. Sets the Graphics Output Buffer size (1 to
    /// 32766) and monitor layout (at most 16; empty means one primary monitor
    /// covering the output). Required before the first `create_surface`, as
    /// Windows servers and mstsc expect. Not inside a frame. The client keeps
    /// its surfaces and cache but resets its codecs, so farland forgets all
    /// codec contexts.
    void reset_graphics(std::uint32_t width, std::uint32_t height, std::span<const MonitorDef> monitors = {});
    /// [MS-RDPEGFX] 2.2.2.9. Returns the lowest unused surface id.
    [[nodiscard]] std::uint16_t create_surface(std::uint16_t width, std::uint16_t height,
                                               std::uint8_t format = pixel_format::xrgb_8888);
    /// [MS-RDPEGFX] 2.2.2.10. Also forgets the surface's codec contexts.
    void delete_surface(std::uint16_t surface_id);
    /// [MS-RDPEGFX] 2.2.2.15.
    void map_surface_to_output(std::uint16_t surface_id, std::uint32_t x, std::uint32_t y);
    /// [MS-RDPEGFX] 2.2.2.22. Requires `negotiated()->scaled_output`.
    void map_surface_to_scaled_output(std::uint16_t surface_id, std::uint32_t x, std::uint32_t y,
                                      std::uint32_t target_width, std::uint32_t target_height);
    /// [MS-RDPEGFX] 2.2.2.20 (RAIL).
    void map_surface_to_window(std::uint16_t surface_id, std::uint64_t window_id, std::uint32_t mapped_width,
                               std::uint32_t mapped_height);
    /// [MS-RDPEGFX] 2.2.2.23. Requires `negotiated()->scaled_output`.
    void map_surface_to_scaled_window(std::uint16_t surface_id, std::uint64_t window_id, std::uint32_t mapped_width,
                                      std::uint32_t mapped_height, std::uint32_t target_width,
                                      std::uint32_t target_height);

    // Frames -------------------------------------------------------------

    /// [MS-RDPEGFX] 2.2.2.11. `timestamp` as from `make_timestamp`, or 0.
    /// Frames do not nest. Returns the frame id.
    [[nodiscard]] std::uint32_t start_frame(std::uint32_t timestamp = 0);
    /// [MS-RDPEGFX] 2.2.2.12. The frame is in flight from now on.
    void end_frame();

    // Drawing (inside a frame, [MS-RDPEGFX] 3.2.5) -----------------------
    //
    // Rectangles must be non-empty and inside their surface, and codecs
    // allowed by `Negotiated::allows`.

    /// [MS-RDPEGFX] 2.2.2.1. Not for Progressive. Mind the client's decoder
    /// buffers: keep each RDP_SEGMENTED_DATA well below 64 KB (PLAN §4.1).
    void wire_to_surface_1(std::uint16_t surface_id, std::uint16_t codec_id, std::uint8_t format, Rect16 dest_rect,
                           std::span<const std::byte> bitmap_data);
    /// [MS-RDPEGFX] 2.2.2.2: Progressive only. mstsc rejects a Progressive
    /// stream above 16 KB with 0x8007006f (PLAN §4.1); split larger updates
    /// into several WireToSurface2 PDUs.
    void wire_to_surface_2(std::uint16_t surface_id, std::uint16_t codec_id, std::uint32_t codec_context_id,
                           std::uint8_t format, std::span<const std::byte> bitmap_data);
    /// [MS-RDPEGFX] 2.2.2.3. The context must have been used in a
    /// WireToSurface2 on that surface.
    void delete_encoding_context(std::uint16_t surface_id, std::uint32_t codec_context_id);
    /// [MS-RDPEGFX] 2.2.2.4. At least one rectangle.
    void solid_fill(std::uint16_t surface_id, Color32 color, std::span<const Rect16> rects);
    /// [MS-RDPEGFX] 2.2.2.5. Each destination must fit in the destination surface.
    void surface_to_surface(std::uint16_t src_surface_id, std::uint16_t dest_surface_id, Rect16 src_rect,
                            std::span<const Point16> dest_points);

    // Bitmap cache ([MS-RDPEGFX] 3.2.1.1, 3.3.1.4) -----------------------

    /// [MS-RDPEGFX] 2.2.2.6: caches `src_rect` under `cache_key` in the lowest
    /// free slot and returns it, or nullopt (sending nothing) if the slots or
    /// the cache bytes (width * height * 4 per entry) of the negotiated cache
    /// size are used up: evict first. `cache_key` must not be cached yet
    /// (look it up with `cache_slot`). Inside a frame.
    [[nodiscard]] std::optional<std::uint16_t> surface_to_cache(std::uint16_t surface_id, Rect16 src_rect,
                                                                std::uint64_t cache_key);
    /// [MS-RDPEGFX] 2.2.2.7. Inside a frame.
    void cache_to_surface(std::uint16_t cache_slot, std::uint16_t surface_id, std::span<const Point16> dest_points);
    /// [MS-RDPEGFX] 2.2.2.8.
    void evict_cache_entry(std::uint16_t cache_slot);
    [[nodiscard]] std::optional<std::uint16_t> cache_slot(std::uint64_t cache_key) const;
    [[nodiscard]] std::size_t cache_slots_used() const noexcept { return cache_keys_.size(); }
    [[nodiscard]] std::uint64_t cache_bytes_used() const noexcept { return cache_bytes_; }

    // State for the frame scheduler --------------------------------------

    [[nodiscard]] bool in_frame() const noexcept { return in_frame_; }
    [[nodiscard]] bool has_surface(std::uint16_t surface_id) const { return surfaces_.contains(surface_id); }
    /// Frames sent (End Frame) and not acknowledged; always 0 while the
    /// client has suspended acknowledgements.
    [[nodiscard]] std::size_t frames_in_flight() const noexcept { return unacked_.size(); }
    /// The client sent SUSPEND_FRAME_ACKNOWLEDGEMENT: do not wait for acks.
    [[nodiscard]] bool acks_suspended() const noexcept { return acks_suspended_; }
    /// queueDepth of the last Frame Acknowledge (0: unknown).
    [[nodiscard]] std::uint32_t queue_depth() const noexcept { return queue_depth_; }
    [[nodiscard]] std::uint32_t total_frames_decoded() const noexcept { return total_frames_decoded_; }

private:
    struct Surface {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint8_t format = 0;
    };
    struct CacheEntry {
        std::uint64_t key = 0;
        std::uint32_t bytes = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        bool used = false;
    };

    void handle_pdu(std::span<const std::byte> bytes);
    void on_caps_advertise(const CapsAdvertise& advertise);
    void on_frame_acknowledge(const FrameAcknowledge& ack);
    void on_qoe_frame_acknowledge(const QoeFrameAcknowledge& qoe);
    void on_cache_import_offer(const CacheImportOffer& offer);
    void reset_protocol();
    /// False after a failure; asserts that the server is ready otherwise.
    [[nodiscard]] bool usable() const;
    /// The negotiated capabilities; only once ready.
    [[nodiscard]] const Negotiated& caps() const;
    [[nodiscard]] const Surface& surface(std::uint16_t surface_id) const;
    [[nodiscard]] CacheEntry& cache_entry(std::uint16_t cache_slot);
    void send(const Pdu& pdu);
    void fail(std::string reason);

    GfxServerConfig config_;
    GfxState state_ = GfxState::wait_caps_advertise;
    std::string failure_;
    std::optional<Negotiated> negotiated_;
    std::vector<std::byte> input_;
    std::vector<std::vector<std::byte>> output_;
    std::deque<GfxEvent> events_;

    bool graphics_reset_ = false;
    std::map<std::uint16_t, Surface> surfaces_;
    std::set<std::pair<std::uint16_t, std::uint32_t>> codec_contexts_;  ///< surface id, codec context id

    bool in_frame_ = false;
    std::uint32_t current_frame_ = 0;
    std::uint32_t next_frame_id_ = 1;
    std::deque<std::uint32_t> unacked_;  ///< in the order sent
    bool acks_suspended_ = false;
    std::uint32_t queue_depth_ = 0;
    std::uint32_t total_frames_decoded_ = 0;

    std::vector<CacheEntry> cache_;  ///< index slot - 1; sized on first use
    std::unordered_map<std::uint64_t, std::uint16_t> cache_keys_;
    std::uint64_t cache_bytes_ = 0;
    std::size_t cache_hint_ = 0;
};

}  // namespace farland::channels::rdpgfx
