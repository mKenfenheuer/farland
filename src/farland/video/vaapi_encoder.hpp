// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/video/dmabuf_frame.hpp>
#include <farland/video/h264_encoder.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// VA-API H.264 backend (docs/PLAN.md §3.4, docs/ROADMAP.md M5): hardware
/// encoding on AMD and Intel GPUs through libva and a DRM render node. Only
/// built with -Dvaapi (libva and libva-drm >= 2.14).
///
/// Pictures come in two ways:
/// - encode(): an I420 picture from the CPU, uploaded into a VA surface;
/// - encode_dmabuf(): a captured frame in GPU memory (PipeWire's dmabuf),
///   imported as a VA surface without a copy (DRM PRIME 2, any modifier the
///   driver imports) and converted to NV12 on the GPU by VA video
///   processing, with the matrix and range of BackendOptions::color.
///
/// The stream is what h264_encoder.hpp promises: one access unit per picture
/// (P frames referencing the previous picture, no B frames), IDRs only when
/// asked for or every keyint frames, our own access unit delimiter, SPS and
/// PPS before every IDR, and a VUI with the colour space and no reordering.
/// Drivers differ in who writes the parameter sets:
/// - with packed headers (VAConfigAttribEncPackedHeaders: intel-media-driver,
///   and Mesa for the SPS) farland hands in its own SPS and PPS;
/// - otherwise the driver writes them, and farland replaces the VUI of the
///   driver's SPS (see bitstream::AccessUnitWriter).
/// Setting FARLAND_VAAPI_PACKED_HEADERS=0 in the environment makes the driver
/// write the headers even when it takes packed ones, for testing.
///
/// Rate control: constant_quality without a cap is VA_RC_CQP; with a cap,
/// and bitrate mode with a cap above the bitrate, VA_RC_VBR (min_qp =
/// quality in constant_quality); bitrate mode without a cap VA_RC_CBR. A new
/// bitrate within CBR or VBR applies to the next picture; a new QP in CQP
/// makes the next picture an IDR (the QP is in the PPS); a new mode re-opens
/// the encoder. Timestamps are ignored; rate control budgets by `fps`.
namespace farland::video::vaapi {

/// What a VA-API device offers for H.264.
struct DeviceInfo {
    std::string render_node;
    /// vaQueryVendorString, e.g. "Mesa Gallium driver 26.0.8 for AMD Radeon RX 6900 XT ...".
    std::string vendor;
    /// Profiles the device encodes.
    std::vector<Profile> profiles;
    bool enc_slice = false;     ///< VAEntrypointEncSlice (the shader/VCN path)
    bool enc_slice_lp = false;  ///< VAEntrypointEncSliceLP (Intel VDEnc)
    bool video_proc = false;    ///< VAEntrypointVideoProc, needed for encode_dmabuf
    /// VA_RC_* and VA_ENC_PACKED_HEADER_* bits of the first profile's encoder.
    std::uint32_t rate_control_modes = 0;
    std::uint32_t packed_headers = 0;
};

/// Opens `render_node` (empty: the first with an H.264 encoder) and reports
/// what it offers; Errc::unsupported if there is none.
[[nodiscard]] Result<DeviceInfo> probe(const std::string& render_node = {});
/// One line for logs.
[[nodiscard]] std::string describe(const DeviceInfo& info);

class VaapiEncoder final : public H264Encoder {
public:
    struct Impl;
    explicit VaapiEncoder(std::unique_ptr<Impl> impl);
    VaapiEncoder(const VaapiEncoder&) = delete;
    VaapiEncoder& operator=(const VaapiEncoder&) = delete;
    VaapiEncoder(VaapiEncoder&&) = delete;
    VaapiEncoder& operator=(VaapiEncoder&&) = delete;
    ~VaapiEncoder() override;

    [[nodiscard]] Backend backend() const noexcept override { return Backend::vaapi; }
    [[nodiscard]] const EncoderConfig& config() const noexcept override;
    [[nodiscard]] Result<void> configure(const EncoderConfig& config) override;
    [[nodiscard]] Result<void> set_rate_control(const RateControl& rate) override;
    void request_idr() noexcept override;
    [[nodiscard]] Result<EncodedFrame> encode(const codec::Yuv420View& picture, const FrameOptions& options) override;
    [[nodiscard]] bool accepts_dmabuf() const noexcept override;
    [[nodiscard]] Result<EncodedFrame> encode_dmabuf(const DmabufFrame& frame, const FrameOptions& options) override;

    /// Only the GPU colour conversion of encode_dmabuf(), read back as I420
    /// of the configured size. Slow; for tests and diagnostics.
    [[nodiscard]] Result<codec::Yuv420Frame> convert(const DmabufFrame& frame);

    [[nodiscard]] const DeviceInfo& device() const noexcept;

    /// Releases the imported dmabufs. Imports are cached (by device and
    /// inode of the descriptors, a handful at a time) because a capture
    /// cycles through the same few buffers; a capture that renegotiates its
    /// buffers should call this, so the old ones are freed.
    void forget_dmabufs() noexcept override;

private:
    std::unique_ptr<Impl> impl_;
};

/// Opens the device (BackendOptions::render_node) and configures an encoder.
[[nodiscard]] Result<std::unique_ptr<H264Encoder>> create(const EncoderConfig& config,
                                                          const BackendOptions& options = {});

}  // namespace farland::video::vaapi
