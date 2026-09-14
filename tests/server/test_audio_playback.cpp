// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/audin.hpp>
#include <farland/channels/drdynvc.hpp>
#include <farland/channels/rdpsnd.hpp>
#include <farland/channels/svc.hpp>
#include <farland/server/audio_input.hpp>
#include <farland/server/audio_playback.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <deque>
#include <numbers>
#include <vector>

namespace rdpsnd = farland::channels::rdpsnd;
namespace audin = farland::channels::audin;
namespace dyn = farland::channels::drdynvc;
namespace svc = farland::channels::svc;
namespace pe = farland::server::playback_event;
namespace ie = farland::server::input_event;
using farland::server::AudioInput;
using farland::server::AudioPlayback;
using farland::server::AudioPlaybackOptions;
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::byte>;
using std::chrono::milliseconds;

namespace {

/// Stands in for Opus: one byte per packet, and it remembers the bitrate.
class FakeEncoder final : public farland::audio::Encoder {
public:
    FakeEncoder(std::size_t samples, std::uint32_t* bitrate) : samples_(samples), bitrate_(bitrate) {}
    [[nodiscard]] std::size_t packet_samples() const noexcept override { return samples_; }
    [[nodiscard]] std::vector<std::byte> encode(std::span<const std::int16_t> /*samples*/) override
    {
        return {std::byte{0x42}};
    }
    void set_bitrate(std::uint32_t bits_per_second) override { *bitrate_ = bits_per_second; }

private:
    std::size_t samples_;
    std::uint32_t* bitrate_;
};

/// The client end of the rdpsnd channel.
struct Client {
    std::vector<Bytes> received;
    std::uint32_t bitrate = 0;

    AudioPlayback playback(bool opus)
    {
        AudioPlaybackOptions options;
        if (opus) {
            options.make_opus =
                [this](farland::audio::PcmFormat format, milliseconds duration,
                       std::uint32_t rate) -> farland::Result<std::unique_ptr<farland::audio::Encoder>> {
                bitrate = rate;
                return std::make_unique<FakeEncoder>(format.samples(duration), &bitrate);
            };
        }
        return AudioPlayback([this](std::span<const std::byte> m) { received.emplace_back(m.begin(), m.end()); },
                             std::move(options));
    }

    std::vector<rdpsnd::ServerPdu> take()
    {
        std::vector<rdpsnd::ServerPdu> out;
        for (const auto& message : std::exchange(received, {})) {
            out.push_back(rdpsnd::decode_server_pdu(message).value());
        }
        return out;
    }
};

/// Runs the initialization sequence; the client takes every server format
/// in `accept` (by tag and rate) and asks for `quality`.
void negotiate(AudioPlayback& playback, Client& client, Clock::time_point now, std::uint16_t quality,
               std::vector<std::uint32_t> accept_rates, bool accept_opus)
{
    playback.start(now);
    const auto server = std::get<rdpsnd::ServerFormats>(client.take().at(0));
    rdpsnd::ClientFormats formats{.flags = rdpsnd::caps::alive, .version = 8, .formats = {}};
    for (const auto& format : server.formats) {
        const bool opus = format.tag == rdpsnd::format_tag::opus;
        if ((opus && accept_opus) ||
            (!opus && std::ranges::find(accept_rates, format.samples_per_sec) != accept_rates.end())) {
            formats.formats.push_back(format);
        }
    }
    REQUIRE(playback.receive(rdpsnd::encode_client_pdu(formats), now));
    REQUIRE(playback.receive(rdpsnd::encode_client_pdu(rdpsnd::QualityMode{quality}), now));
    const auto training = std::get<rdpsnd::Training>(client.take().at(0));
    REQUIRE(playback.receive(rdpsnd::encode_client_pdu(rdpsnd::TrainingConfirm{training.timestamp, 0}), now));
}

std::vector<std::int16_t> tone(const farland::audio::PcmFormat& format, milliseconds duration)
{
    std::vector<std::int16_t> samples(format.samples(duration));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double t = static_cast<double>(i / format.channels) / format.rate;
        samples[i] = static_cast<std::int16_t>(8000 * std::sin(2 * std::numbers::pi * 440 * t));
    }
    return samples;
}

}  // namespace

TEST_CASE("Audio output offers Opus first and PCM always")
{
    Client client;
    auto with_opus = client.playback(true);
    CHECK(with_opus.offered_formats().front() == AudioPlayback::opus_format());
    CHECK(with_opus.offered_formats().size() == 4);
    auto without = client.playback(false);
    CHECK(without.offered_formats().front() == rdpsnd::pcm_format(48000, 2));
}

TEST_CASE("Audio output picks the format by quality mode ([MS-RDPEA] 3.1.1.4)")
{
    const auto now = Clock::now();
    SECTION("dynamic quality: Opus")
    {
        Client client;
        auto playback = client.playback(true);
        negotiate(playback, client, now, rdpsnd::quality::dynamic, {48000, 44100}, true);
        const auto ready = std::get<pe::Ready>(playback.poll_event().value());
        CHECK(ready.capture == farland::audio::PcmFormat{48000, 2});
        CHECK(ready.codec.starts_with("Opus"));
        CHECK(client.bitrate == 96000);
        playback.set_bandwidth(1000);
        CHECK(client.bitrate == 50000);
    }
    SECTION("high quality: PCM 48 kHz")
    {
        Client client;
        auto playback = client.playback(true);
        negotiate(playback, client, now, rdpsnd::quality::high, {48000, 44100}, true);
        const auto ready = std::get<pe::Ready>(playback.poll_event().value());
        CHECK(ready.codec.starts_with("PCM 48000"));
    }
    SECTION("a client with only PCM 44.1 kHz")
    {
        Client client;
        auto playback = client.playback(true);
        negotiate(playback, client, now, rdpsnd::quality::dynamic, {44100}, false);
        CHECK(std::get<pe::Ready>(playback.poll_event().value()).capture == farland::audio::PcmFormat{44100, 2});
    }
}

TEST_CASE("Audio output cuts PCM into 20 ms Wave2 PDUs")
{
    const auto now = Clock::now();
    Client client;
    auto playback = client.playback(false);
    negotiate(playback, client, now, rdpsnd::quality::high, {48000}, false);
    static_cast<void>(playback.poll_event());
    const auto format = *playback.capture_format();
    const auto samples = tone(format, milliseconds(50));
    playback.push(samples, now);
    auto pdus = client.take();
    REQUIRE(pdus.size() == 2);  // 40 ms sent, 10 ms wait for more
    const auto& first = std::get<rdpsnd::Wave2>(pdus[0]);
    CHECK(first.data.size() == 960 * 2 * 2);
    CHECK(first.format_no == 0);
    CHECK(std::to_integer<std::uint8_t>(first.data[2]) == (static_cast<std::uint16_t>(samples[1]) & 0xFF));
    playback.push(tone(format, milliseconds(10)), now);
    CHECK(client.take().size() == 1);
}

TEST_CASE("Audio output drops packets when confirmations fall behind")
{
    auto now = Clock::now();
    Client client;
    auto playback = client.playback(true);
    negotiate(playback, client, now, rdpsnd::quality::dynamic, {48000}, true);
    static_cast<void>(playback.poll_event());
    const auto format = *playback.capture_format();

    // Before the first confirmation, up to 400 ms go out unconfirmed. The
    // capture delivers 20 ms at a time.
    const auto chunk = tone(format, milliseconds(20));
    for (int i = 0; i < 50; ++i) {
        playback.push(chunk, now);
    }
    auto pdus = client.take();
    CHECK(pdus.size() == 20);
    auto stats = playback.take_stats(now);
    CHECK(stats.packets_dropped == 30);

    // The client confirms everything at once, 10 ms later, with nothing in
    // flight: from now on the backlog may grow to 100 ms only.
    now += milliseconds(10);
    const auto last = std::get<rdpsnd::Wave2>(pdus.back());
    REQUIRE(playback.receive(rdpsnd::encode_client_pdu(rdpsnd::WaveConfirm{last.timestamp, last.block_no}), now));
    stats = playback.take_stats(now);
    CHECK(stats.unconfirmed == milliseconds(0));
    CHECK(stats.max_round_trip == milliseconds(10));
    for (int i = 0; i < 10; ++i) {
        playback.push(chunk, now);
    }
    CHECK(client.take().size() == 5);
    CHECK(playback.take_stats(now).packets_dropped == 5);

    // Unconfirmed samples stop counting after two seconds.
    now += milliseconds(2500);
    playback.push(tone(format, milliseconds(40)), now);
    CHECK(client.take().size() == 2);
}

TEST_CASE("Audio output stops during silence and says so with a Close PDU ([MS-RDPEA] 3.3.5.2.1.7)")
{
    auto now = Clock::now();
    Client client;
    auto playback = client.playback(false);
    negotiate(playback, client, now, rdpsnd::quality::high, {48000}, false);
    static_cast<void>(playback.poll_event());
    const auto format = *playback.capture_format();
    const std::vector<std::int16_t> silence(format.samples(milliseconds(20)), 0);
    int waves = 0;
    int closes = 0;
    for (int i = 0; i < 100; ++i) {
        now += milliseconds(20);
        playback.push(silence, now);
        for (const auto& pdu : client.take()) {
            waves += std::holds_alternative<rdpsnd::Wave2>(pdu) ? 1 : 0;
            closes += std::holds_alternative<rdpsnd::Close>(pdu) ? 1 : 0;
        }
        // Confirm at once, so flow control stays out of the way.
        REQUIRE(playback.receive(
            rdpsnd::encode_client_pdu(rdpsnd::WaveConfirm{0, static_cast<std::uint8_t>(waves - 1)}), now));
    }
    CHECK(waves == 49);  // the first second, less the packet that reached the timeout
    CHECK(closes == 1);
    playback.push(tone(format, milliseconds(20)), now);
    CHECK(std::holds_alternative<rdpsnd::Wave2>(client.take().at(0)));
}

TEST_CASE("Audio output reports clients that cannot play")
{
    Client client;
    auto playback = client.playback(false);
    playback.start(Clock::now());
    static_cast<void>(client.take());
    REQUIRE(playback.receive(rdpsnd::encode_client_pdu(rdpsnd::ClientFormats{.flags = 0, .version = 8, .formats = {}}),
                             Clock::now()));
    CHECK(std::holds_alternative<pe::Unavailable>(playback.poll_event().value()));
}

namespace {

/// The client end of drdynvc, for the AUDIO_INPUT channel.
struct DvcClient {
    std::vector<Bytes> chunks;
    svc::Reassembler reassembler{std::size_t{1} << 20U};
    std::deque<Bytes> messages;

    farland::server::DynamicChannels::SendChunk sink()
    {
        return [this](std::span<const std::byte> chunk) { chunks.emplace_back(chunk.begin(), chunk.end()); };
    }
    std::vector<dyn::ServerPdu> take()
    {
        std::vector<dyn::ServerPdu> out;
        for (const auto& chunk : std::exchange(chunks, {})) {
            if (auto message = reassembler.add(chunk).value()) {
                messages.push_back(std::move(*message));
                out.push_back(dyn::decode_server_pdu(messages.back()).value());
            }
        }
        return out;
    }
};

void send(farland::server::DynamicChannels& channels, const dyn::ClientPdu& pdu)
{
    for (const auto& chunk : svc::encode_chunks(dyn::encode_client_pdu(pdu))) {
        REQUIRE(channels.receive(chunk).has_value());
    }
}

/// Feeds the channels' events to `input`.
void pump(farland::server::DynamicChannels& channels, AudioInput& input)
{
    while (auto event = channels.poll_event()) {
        input.handle(*event);
    }
}

}  // namespace

TEST_CASE("Audio input opens AUDIO_INPUT and delivers samples ([MS-RDPEAI] 3.1.3)")
{
    DvcClient client;
    farland::server::DynamicChannels channels(client.sink());
    channels.start();
    static_cast<void>(client.take());
    send(channels, dyn::CapsResponse{dyn::version3});
    static_cast<void>(channels.poll_event());

    AudioInput input(channels);
    const auto create = std::get<dyn::CreateRequest>(client.take().at(0));
    CHECK(create.name == "AUDIO_INPUT");
    send(channels, dyn::CreateResponse{create.channel_id, 0});
    pump(channels, input);
    const auto version = client.take();
    REQUIRE(version.size() == 1);
    CHECK(std::get<dyn::Data>(version[0]).data.size() == 5);  // the Version PDU, [MS-RDPEAI] 3.1.5.1

    const auto client_pdu = [&](const audin::ClientPdu& pdu) {
        send(channels, dyn::Data{create.channel_id, audin::encode_client_pdu(pdu)});
        pump(channels, input);
    };
    client_pdu(audin::Version{2});
    client_pdu(audin::Formats{{farland::channels::rdpsnd::pcm_format(48000, 1)}});
    client_pdu(audin::FormatChange{0});
    client_pdu(audin::OpenReply{0});
    CHECK(std::get<ie::Opened>(input.poll_event().value()).format == farland::audio::PcmFormat{48000, 1});
    const Bytes data{std::byte{0x01}, std::byte{0x80}, std::byte{0xFF}, std::byte{0x7F}};
    client_pdu(audin::Data{data});
    CHECK(std::get<ie::Samples>(input.poll_event().value()).samples == std::vector<std::int16_t>{-32767, 32767});

    client_pdu(audin::Data{std::span(data).first(3)});
    CHECK(std::holds_alternative<ie::Closed>(input.poll_event().value()));
    CHECK(input.closed());
}
