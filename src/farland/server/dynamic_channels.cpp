// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/dynamic_channels.hpp>

#include <memory>

namespace farland::server {

namespace {

/// Largest drdynvc message reassembled from Virtual Channel chunks. DVC PDUs
/// are at most 1600 bytes ([MS-RDPEDYC] 2.2.3); the slack covers clients
/// that ignore that.
constexpr std::size_t max_svc_message = std::size_t{64} * 1024;

}  // namespace

channels::DvcServerConfig DynamicChannels::default_config()
{
    channels::DvcServerConfig config;
    config.make_decompressor = [limit = config.max_message_size] {
        auto decompressor = std::make_shared<codec::ZgfxDecompressor>(codec::ZgfxVariant::rdp8_lite, limit);
        return channels::DvcDecompressor(
            [decompressor](std::span<const std::byte> data) { return decompressor->decompress(data); });
    };
    return config;
}

DynamicChannels::DynamicChannels(SendChunk send_chunk, channels::DvcServerConfig config, std::size_t chunk_length)
    : send_chunk_(std::move(send_chunk)), chunk_length_(chunk_length), reassembler_(max_svc_message),
      dvc_(std::move(config))
{
}

void DynamicChannels::start()
{
    dvc_.start();
    flush();
}

Result<void> DynamicChannels::receive(std::span<const std::byte> pdu)
{
    FARLAND_TRY(const auto message, reassembler_.add(pdu));
    if (message) {
        const auto handled = dvc_.receive(*message);
        flush();  // answers queued before an error still go out
        FARLAND_TRY_VOID(handled);
    }
    return {};
}

std::uint32_t DynamicChannels::open(std::string name, channels::DvcChannelOptions options)
{
    const auto id = dvc_.open(std::move(name), options);
    flush();
    return id;
}

bool DynamicChannels::send(std::uint32_t id, std::span<const std::byte> message)
{
    const bool sent = dvc_.send(id, message);
    flush();
    return sent;
}

void DynamicChannels::close(std::uint32_t id)
{
    dvc_.close(id);
    flush();
}

void DynamicChannels::flush()
{
    for (const auto& message : dvc_.take_output()) {
        for (const auto& chunk : channels::svc::encode_chunks(message, chunk_length_)) {
            send_chunk_(chunk);
        }
    }
}

}  // namespace farland::server
