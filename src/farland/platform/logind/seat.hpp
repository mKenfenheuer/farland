// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/portal/portal_session.hpp>
#include <farland/platform/portal/sd_bus.hpp>

#include <memory>
#include <string>

/// The user's session on its seat, through logind and the display manager
/// (docs/PLAN.md §3.3): who has the screen at the machine, and how to hand it
/// over. Nothing here is specific to a compositor — GNOME and Plasma take a
/// local session over the same way, and only the picture side of it differs.
namespace farland::platform::logind {

/// The D-Bus plumbing and its error type come from the portal backend.
using portal::PortalErrc;
using portal::PortalError;
template <class T>
using LogindResult = portal::PortalResult<T>;

/// Watches the user's graphical session in logind. A compositor draws the
/// session that is active on its seat, so a connection that depends on the
/// seat holds the session only while it is active; one whose screens keep
/// being drawn off the seat instead ends when the session comes *back* to the
/// seat, because somebody logged in at the machine and took it.
class SeatWatch {
public:
    /// Watches this process's session; fails without logind or a session.
    [[nodiscard]] static LogindResult<std::unique_ptr<SeatWatch>> create();

    SeatWatch(const SeatWatch&) = delete;
    SeatWatch& operator=(const SeatWatch&) = delete;
    SeatWatch(SeatWatch&&) = delete;
    SeatWatch& operator=(SeatWatch&&) = delete;
    ~SeatWatch() = default;

    /// Poll this for readability and call process() then.
    [[nodiscard]] int fd() const noexcept;
    void process();
    /// The session left its seat after we started watching: something else is
    /// on the seat now.
    [[nodiscard]] bool left_the_seat() const noexcept { return left_seat_; }
    /// The session came back to its seat after having left it: somebody
    /// logged in at the machine and took it back, and a client that handed
    /// the seat a login screen has to let go.
    [[nodiscard]] bool returned_to_the_seat() const noexcept { return returned_; }
    /// The return this reported was not a takeover after all: whoever logged
    /// in at the machine was refused ([policy] seat_takeover), and the seat
    /// only fell back to the session because the display manager gave up the
    /// login screen it was showing. The next return counts again.
    void forget_return() noexcept { returned_ = false; }
    /// The session is active on its seat now.
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    SeatWatch() = default;
    static int on_properties_changed(sd_bus_message* message, void* userdata, sd_bus_error* error);
    void set_active(bool active);

    portal::detail::BusPtr bus_;
    portal::detail::SlotPtr watch_;
    std::string session_path_;
    /// It was active for us and then lost the seat.
    bool left_seat_ = false;
    /// It lost the seat and then got it back.
    bool returned_ = false;
    bool active_ = false;
};

/// Switches the seat this session is on to the display manager's login
/// screen, as "switch user" does: the seat shows the greeter, and logging in
/// again there brings the session back to the seat. GDM
/// (org.gnome.DisplayManager.LocalDisplayFactory.CreateTransientDisplay) and
/// the freedesktop display manager API that SDDM and LightDM implement
/// (org.freedesktop.DisplayManager.Seat.SwitchToGreeter) are both tried, so
/// this works whichever display manager runs; both policies let the user at
/// the seat ask for a greeter.
///
/// Careful: a compositor draws only the session that is active on its seat
/// unless it was asked to keep drawing off it, so a session switched away
/// this way may stop producing frames, and a client holding it would see
/// nothing. Only call it where the screens keep being drawn.
/// Never makes a second greeter: where one is already on the seat this
/// switches to it, and fails (without creating anything) where it may not,
/// so that a privileged caller can do the switch instead. A new greeter is
/// created only when the seat has none.
[[nodiscard]] LogindResult<void> switch_seat_to_greeter();

/// Brings the user's graphical session to its seat if something else is
/// there (logind Session.Activate, and the seat's VT switch where that is not
/// enough): a compositor draws only the session that is active on its seat,
/// so a session has to be active before a client can see anything of it.
/// Succeeds at once for a session without a seat (a headless one the display
/// manager started), which is always drawn.
[[nodiscard]] LogindResult<void> activate_user_session();

/// Whether this process's session is locked -- the screen at the machine is
/// showing the lock screen, whether somebody locked it or it locked itself.
///
/// It matters because a locked GNOME session refuses to be shared at all:
/// Mutter answers RemoteDesktop.CreateSession with "Session creation
/// inhibited" while the lock screen is up, so a client cannot attach to the
/// session until it is unlocked. False where there is no logind or no
/// session, so that a build without either behaves as it always did.
[[nodiscard]] bool user_session_locked();

}  // namespace farland::platform::logind
