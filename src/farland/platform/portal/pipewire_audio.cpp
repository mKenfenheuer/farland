// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/pipewire_audio.hpp>
#include <farland/platform/portal/pipewire_stream.hpp>
#include <farland/platform/portal/pipewire_util.hpp>

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace farland::platform::portal {

namespace {

constexpr std::string_view log_component = "platform.pipewire.audio";

using pw_host::first_data;
using pw_host::StreamHost;
using pw_host::UniqueFd;

// SPA_AUDIO_FORMAT_S16_LE samples are copied as int16_t.
static_assert(std::endian::native == std::endian::little, "farland's PipeWire audio assumes a little-endian host");

/// Drops the oldest whole frames of `ring` beyond `max_samples`.
void trim(std::vector<std::int16_t>& ring, std::size_t max_samples, std::uint16_t channels)
{
    if (ring.size() <= max_samples) {
        return;
    }
    const std::size_t excess = (ring.size() - max_samples + channels - 1) / channels * channels;
    ring.erase(ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(std::min(excess, ring.size())));
}

/// An EnumFormat for interleaved S16LE PCM in `format`.
const spa_pod* format_pod(pw::PodBuilder& b, audio::PcmFormat format)
{
    spa_pod_frame f{};
    b.push_object(&f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    b.prop(SPA_FORMAT_mediaType);
    b.id(SPA_MEDIA_TYPE_audio);
    b.prop(SPA_FORMAT_mediaSubtype);
    b.id(SPA_MEDIA_SUBTYPE_raw);
    b.prop(SPA_FORMAT_AUDIO_format);
    b.id(SPA_AUDIO_FORMAT_S16_LE);
    b.prop(SPA_FORMAT_AUDIO_rate);
    b.int_(static_cast<std::int32_t>(format.rate));
    b.prop(SPA_FORMAT_AUDIO_channels);
    b.int_(format.channels);
    std::array<std::uint32_t, 2> positions{SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
    if (format.channels == 1) {
        positions[0] = SPA_AUDIO_CHANNEL_MONO;
    }
    b.prop(SPA_FORMAT_AUDIO_position);
    spa_pod_builder_array(b.get(), sizeof(std::uint32_t), SPA_TYPE_Id, format.channels, positions.data());
    return b.pop(&f);
}

/// The default sink's monitor. The process callback runs on PipeWire's
/// real-time thread and only copies into the ring.
class SinkMonitor final : public AudioSource {
public:
    SinkMonitor(audio::PcmFormat format, AudioCaptureOptions options)
        : format_(format), options_(std::move(options)), max_samples_(format.samples(options_.max_buffered)),
          wake_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    {
    }
    SinkMonitor(const SinkMonitor&) = delete;
    SinkMonitor& operator=(const SinkMonitor&) = delete;
    SinkMonitor(SinkMonitor&&) = delete;
    SinkMonitor& operator=(SinkMonitor&&) = delete;
    ~SinkMonitor() override { host_.shutdown(); }

    [[nodiscard]] Result<void> start()
    {
        if (wake_.get() < 0) {
            return fail(Errc::io, "cannot create an eventfd");
        }
        FARLAND_TRY_VOID(host_.connect("farland-audio-out", wake_.get()));
        const std::string latency = std::format("{}/{}", format_.frames(options_.quantum), format_.rate);
        const std::array<spa_dict_item, 7> items{{
            {PW_KEY_MEDIA_TYPE, "Audio"},
            {PW_KEY_MEDIA_CATEGORY, "Capture"},
            {PW_KEY_APP_NAME, "farland"},
            {PW_KEY_NODE_NAME, options_.node_name.c_str()},
            {PW_KEY_NODE_DESCRIPTION, options_.description.c_str()},
            {PW_KEY_NODE_LATENCY, latency.c_str()},
            // The monitor of the default sink rather than the default source.
            {PW_KEY_STREAM_CAPTURE_SINK, "true"},
        }};
        pw::PodBuilder b;
        const std::array<const spa_pod*, 1> params{format_pod(b, format_)};
        if (b.overflowed()) {
            return fail(Errc::limit_exceeded, "format parameters do not fit");
        }
        return host_.start_stream(options_.node_name.c_str(), items, events(), this, PW_DIRECTION_INPUT, params);
    }

    [[nodiscard]] int wake_fd() const override { return wake_.get(); }

    void read(std::vector<std::int16_t>& out) override
    {
        eventfd_t value = 0;
        static_cast<void>(::eventfd_read(wake_.get(), &value));
        const std::scoped_lock lock(mutex_);
        out.insert(out.end(), ring_.begin(), ring_.end());
        ring_.clear();
        if (host_.closed()) {
            static_cast<void>(::eventfd_write(wake_.get(), 1));  // stays readable once closed
        }
    }

    [[nodiscard]] bool closed() const override { return host_.closed(); }
    [[nodiscard]] std::string error() const override { return host_.error(); }

private:
    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* message)
    {
        static_cast<SinkMonitor*>(data)->host_.state_changed(old, state, message);
    }

    static void on_process(void* data)
    {
        auto& self = *static_cast<SinkMonitor*>(data);
        pw_buffer* buffer = pw_stream_dequeue_buffer(self.host_.stream());
        if (buffer == nullptr) {
            return;
        }
        if (const spa_data* d = first_data(buffer)) {
            const std::span<const std::byte> all(static_cast<const std::byte*>(d->data), d->maxsize);
            const std::uint32_t offset = std::min(d->chunk->offset, d->maxsize);
            const std::uint32_t size = std::min(d->chunk->size, d->maxsize - offset);
            self.append(all.subspan(offset, size));
        }
        pw_stream_queue_buffer(self.host_.stream(), buffer);
    }

    void append(std::span<const std::byte> bytes)
    {
        const std::size_t count = bytes.size() / sizeof(std::int16_t);
        if (count == 0) {
            return;
        }
        {
            const std::scoped_lock lock(mutex_);
            const std::size_t old = ring_.size();
            ring_.resize(old + count);
            std::memcpy(std::span(ring_).subspan(old).data(), bytes.data(), count * sizeof(std::int16_t));
            trim(ring_, max_samples_, format_.channels);
        }
        static_cast<void>(::eventfd_write(wake_.get(), 1));
    }

    static const pw_stream_events& events()
    {
        static const pw_stream_events e = [] {
            pw_stream_events v{};
            v.version = PW_VERSION_STREAM_EVENTS;
            v.state_changed = &SinkMonitor::on_state_changed;
            v.process = &SinkMonitor::on_process;
            return v;
        }();
        return e;
    }

    audio::PcmFormat format_;
    AudioCaptureOptions options_;
    std::size_t max_samples_;
    UniqueFd wake_;
    std::mutex mutex_;
    std::vector<std::int16_t> ring_;
    StreamHost host_;  ///< last: shut down (in the destructor) before the ring goes
};

/// A virtual source played out from a small jitter buffer.
class VirtualSource final : public AudioSink {
public:
    VirtualSource(audio::PcmFormat format, VirtualSourceOptions options)
        : format_(format), options_(std::move(options)), max_samples_(format.samples(options_.max_buffered)),
          prebuffer_samples_(format.samples(options_.prebuffer))
    {
    }
    VirtualSource(const VirtualSource&) = delete;
    VirtualSource& operator=(const VirtualSource&) = delete;
    VirtualSource(VirtualSource&&) = delete;
    VirtualSource& operator=(VirtualSource&&) = delete;
    ~VirtualSource() override { host_.shutdown(); }

    [[nodiscard]] Result<void> start()
    {
        FARLAND_TRY_VOID(host_.connect("farland-microphone", -1));
        const std::string latency = std::format("{}/{}", format_.frames(options_.quantum), format_.rate);
        const std::array<spa_dict_item, 7> items{{
            {PW_KEY_MEDIA_TYPE, "Audio"},
            // Audio/Source/Virtual, as virtual devices of PipeWire modules have it,
            // makes WirePlumber 0.5 stall every Pulse stream opened after this
            // node appears (Plasma 6.6, PipeWire 1.6); gnome-remote-desktop uses
            // Audio/Source too.
            {PW_KEY_MEDIA_CLASS, "Audio/Source"},
            {PW_KEY_APP_NAME, "farland"},
            {PW_KEY_NODE_NAME, options_.node_name.c_str()},
            {PW_KEY_NODE_DESCRIPTION, options_.description.c_str()},
            {PW_KEY_NODE_NICK, options_.description.c_str()},
            {PW_KEY_NODE_LATENCY, latency.c_str()},
        }};
        pw::PodBuilder b;
        const std::array<const spa_pod*, 1> params{format_pod(b, format_)};
        if (b.overflowed()) {
            return fail(Errc::limit_exceeded, "format parameters do not fit");
        }
        return host_.start_stream(options_.node_name.c_str(), items, events(), this, PW_DIRECTION_OUTPUT, params);
    }

    void write(std::span<const std::int16_t> samples) override
    {
        const std::scoped_lock lock(mutex_);
        ring_.insert(ring_.end(), samples.begin(), samples.end());
        trim(ring_, max_samples_, format_.channels);
    }

    [[nodiscard]] bool closed() const override { return host_.closed(); }
    [[nodiscard]] std::string error() const override { return host_.error(); }

private:
    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* message)
    {
        static_cast<VirtualSource*>(data)->host_.state_changed(old, state, message);
    }

    static void on_process(void* data)
    {
        auto& self = *static_cast<VirtualSource*>(data);
        pw_buffer* buffer = pw_stream_dequeue_buffer(self.host_.stream());
        if (buffer == nullptr) {
            return;
        }
        if (const spa_data* d = first_data(buffer)) {
            const std::size_t stride = std::size_t{self.format_.channels} * sizeof(std::int16_t);
            std::size_t frames = d->maxsize / stride;
            if (buffer->requested > 0) {
                frames = std::min<std::size_t>(frames, buffer->requested);
            }
            const std::span<std::byte> out(static_cast<std::byte*>(d->data), frames * stride);
            self.fill(out);
            d->chunk->offset = 0;
            d->chunk->stride = static_cast<std::int32_t>(stride);
            d->chunk->size = static_cast<std::uint32_t>(out.size());
        }
        pw_stream_queue_buffer(self.host_.stream(), buffer);
    }

    /// Plays out what is buffered; silence while (re)filling the jitter buffer.
    void fill(std::span<std::byte> out)
    {
        const std::size_t wanted = out.size() / sizeof(std::int16_t);
        std::size_t taken = 0;
        {
            const std::scoped_lock lock(mutex_);
            if (!playing_ && ring_.size() >= prebuffer_samples_) {
                playing_ = true;
            }
            if (playing_) {
                taken = std::min(wanted, ring_.size());
                std::memcpy(out.data(), ring_.data(), taken * sizeof(std::int16_t));
                ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(taken));
                playing_ = taken == wanted;  // ran dry: buffer up again
            }
        }
        std::ranges::fill(out.subspan(taken * sizeof(std::int16_t)), std::byte{0});
    }

    static const pw_stream_events& events()
    {
        static const pw_stream_events e = [] {
            pw_stream_events v{};
            v.version = PW_VERSION_STREAM_EVENTS;
            v.state_changed = &VirtualSource::on_state_changed;
            v.process = &VirtualSource::on_process;
            return v;
        }();
        return e;
    }

    audio::PcmFormat format_;
    VirtualSourceOptions options_;
    std::size_t max_samples_;
    std::size_t prebuffer_samples_;
    std::mutex mutex_;
    std::vector<std::int16_t> ring_;
    bool playing_ = false;
    StreamHost host_;  ///< last: shut down (in the destructor) before the ring goes
};

}  // namespace

Result<std::unique_ptr<AudioSource>> capture_default_sink(audio::PcmFormat format, const AudioCaptureOptions& options)
{
    auto monitor = std::make_unique<SinkMonitor>(format, options);
    FARLAND_TRY_VOID(monitor->start());
    return monitor;
}

Result<std::unique_ptr<AudioSink>> create_virtual_source(audio::PcmFormat format, const VirtualSourceOptions& options)
{
    auto source = std::make_unique<VirtualSource>(format, options);
    FARLAND_TRY_VOID(source->start());
    return source;
}

}  // namespace farland::platform::portal
