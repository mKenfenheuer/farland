// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/kwin/kwin_eis.hpp>
#include <farland/platform/portal/portal_bus.hpp>

#include <fcntl.h>
#include <thread>

namespace farland::platform::kwin {

namespace {

constexpr std::string_view log_component = "platform.kwin";
constexpr const char* kwin_service = "org.kde.KWin";
constexpr const char* eis_path = "/org/kde/KWin/EIS/RemoteDesktop";
constexpr const char* eis_interface = "org.kde.KWin.EIS.RemoteDesktop";
using Clock = std::chrono::steady_clock;

}  // namespace

Result<std::unique_ptr<KWinEis>> KWinEis::connect(const std::string& bus_address, int capabilities,
                                                  std::chrono::milliseconds timeout)
{
    auto bus = portal::detail::Bus::open(bus_address);
    if (!bus) {
        log::error(log_component, "{}", bus.error().message);
        return fail(Errc::io, "cannot connect to the session bus");
    }
    std::unique_ptr<KWinEis> eis(new KWinEis());
    eis->bus_ = std::move(*bus);
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        auto call = eis->bus_->new_call(eis_interface, "connectToEIS", eis_path, kwin_service);
        if (!call) {
            return fail(Errc::io, "cannot build the connectToEIS call");
        }
        portal::detail::MessageWriter(call->get()).i32(capabilities);
        auto reply = eis->bus_->call(call->get(), "connectToEIS", deadline, -1);
        if (reply) {
            portal::detail::MessageReader reader(reply->get());
            int borrowed = -1;
            std::int32_t cookie = 0;
            if (!reader.fd(borrowed) || sd_bus_message_read_basic(reply->get(), 'i', &cookie) < 0) {
                return fail(Errc::io, "KWin's connectToEIS reply is malformed");
            }
            eis->socket_.reset(::fcntl(borrowed, F_DUPFD_CLOEXEC, 3));  // NOLINT(cppcoreguidelines-pro-type-vararg)
            eis->cookie_ = cookie;
            if (!eis->socket_.valid()) {
                return fail(Errc::io, "cannot duplicate KWin's EIS socket");
            }
            return eis;
        }
        if (Clock::now() >= deadline) {
            log::error(log_component, "{}", reply.error().message);
            return fail(Errc::io, "KWin gave no EIS socket (org.kde.KWin.EIS.RemoteDesktop)");
        }
        log::debug(log_component, "{}; trying again", reply.error().message);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

KWinEis::~KWinEis()
{
    if (bus_ && cookie_ != 0) {
        if (auto call = bus_->new_call(eis_interface, "disconnect", eis_path, kwin_service)) {
            portal::detail::MessageWriter(call->get()).i32(cookie_);
            bus_->send(call->get());
        }
    }
}

int KWinEis::bus_fd() const
{
    return sd_bus_get_fd(bus_->get());
}

void KWinEis::dispatch()
{
    if (const auto processed = bus_->process_pending(); !processed) {
        log::debug(log_component, "the session bus: {}", processed.error().message);
    }
}

}  // namespace farland::platform::kwin
