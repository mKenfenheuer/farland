#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

"""A fake xdg-desktop-portal for the portal client tests.

Implements what farland uses of org.freedesktop.portal.RemoteDesktop,
.ScreenCast, .Request and .Session, and checks the argument types strictly.
org.farland.Mock at /org/farland/Mock returns the calls it saw, one line each:
the method name followed by its options or arguments as sorted key=value
pairs (handle tokens left out).

Prints "ready" once it owns org.freedesktop.portal.Desktop. Exits with 77
when python3-dbus or PyGObject is missing, so the tests can skip.
"""

import argparse
import os
import sys

try:
    import dbus
    import dbus.mainloop.glib
    import dbus.service
    from gi.repository import GLib
except ImportError as error:
    print(f"mock_portal: {error}", file=sys.stderr)
    sys.exit(77)

PORTAL = "org.freedesktop.portal.Desktop"
PATH = "/org/freedesktop/portal/desktop"
RD = "org.freedesktop.portal.RemoteDesktop"
SC = "org.freedesktop.portal.ScreenCast"
REQUEST = "org.freedesktop.portal.Request"
SESSION = "org.freedesktop.portal.Session"
PROPERTIES = "org.freedesktop.DBus.Properties"
MOCK = "org.farland.Mock"

INVALID_ARGS = "org.freedesktop.DBus.Error.InvalidArgs"

calls = []


class Failure(dbus.exceptions.DBusException):
    def __init__(self, message, name="org.freedesktop.portal.Error.Failed"):
        super().__init__(message, name=name)


def fmt(value):
    if isinstance(value, dbus.Boolean):
        return "true" if value else "false"
    if isinstance(value, float):
        return f"{value:g}"
    if isinstance(value, int):
        return str(int(value))
    return str(value)


def describe(method, options=None, **args):
    items = dict(args)
    for key, value in (options or {}).items():
        if key not in ("handle_token", "session_handle_token"):
            items[key] = value
    return " ".join([method] + [f"{key}={fmt(items[key])}" for key in sorted(items)])


def check_types(options, expected):
    for key, value in options.items():
        if key in expected and not isinstance(value, expected[key]):
            raise Failure(f"option {key} has type {type(value).__name__}", INVALID_ARGS)


def sender_element(sender):
    return sender[1:].replace(".", "_")


class Request(dbus.service.Object):
    def __init__(self, bus, path):
        super().__init__(bus, path)
        self.path = path
        self.done = False

    @dbus.service.method(REQUEST, in_signature="", out_signature="")
    def Close(self):
        calls.append("Request.Close")
        self.finish()

    @dbus.service.signal(REQUEST, signature="ua{sv}")
    def Response(self, response, results):
        pass

    def respond(self, code, results):
        if not self.done:
            self.Response(dbus.UInt32(code), dbus.Dictionary(results, signature="sv"))
            self.finish()
        return False

    def finish(self):
        if not self.done:
            self.done = True
            self.remove_from_connection()


class Session(dbus.service.Object):
    def __init__(self, bus, path, sender):
        super().__init__(bus, path)
        self.path = path
        self.sender = sender
        self.device_types = 0
        self.source_types = 0
        self.persist_mode = 0
        self.eis = False
        self.closed = False

    @dbus.service.method(SESSION, in_signature="", out_signature="")
    def Close(self):
        calls.append("Session.Close")
        self.finish()

    @dbus.service.signal(SESSION, signature="a{sv}")
    def Closed(self, details):
        pass

    def close_from_portal(self):
        if not self.closed:
            self.Closed(dbus.Dictionary({}, signature="sv"))
            self.finish()
        return False

    def finish(self):
        if not self.closed:
            self.closed = True
            self.remove_from_connection()


class Portal(dbus.service.Object):
    def __init__(self, bus, config):
        super().__init__(bus, PATH)
        self.config = config
        self.sessions = {}
        self.tokens = 0
        self.legacy_requests = 0

    # --- helpers

    def new_request(self, sender, options):
        if self.config.legacy_request_path:
            self.legacy_requests += 1
            path = f"{PATH}/request/legacy/r{self.legacy_requests}"
        else:
            token = options.get("handle_token")
            if not isinstance(token, dbus.String):
                raise Failure("handle_token missing", INVALID_ARGS)
            path = f"{PATH}/request/{sender_element(sender)}/{token}"
        return Request(self.connection, path)

    def answer(self, request, ok, code, results, delay_ms=0):
        """Replies with the request handle and emits the Response signal."""
        if self.config.response_before_reply and delay_ms == 0 and not self.config.legacy_request_path:
            # Only clients that subscribed before calling see this.
            request.respond(code, results)
            ok(request.path)
            return
        ok(request.path)
        if self.config.legacy_request_path:
            delay_ms = max(delay_ms, 50)
        GLib.timeout_add(max(delay_ms, 1), request.respond, code, results)

    def session(self, handle, sender):
        session = self.sessions.get(str(handle))
        if session is None or session.closed or session.sender != sender:
            raise Failure(f"no session {handle}", "org.freedesktop.portal.Error.NotFound")
        return session

    def streams(self, session):
        config = self.config
        streams = []

        def stream(node, index, source_type, position, size):
            props = {"id": dbus.String(str(index))}
            if position is not None:
                props["position"] = dbus.Struct((dbus.Int32(position[0]), dbus.Int32(position[1])), signature="ii")
            props["size"] = dbus.Struct((dbus.Int32(size[0]), dbus.Int32(size[1])), signature="ii")
            if config.sc_version >= 3:
                props["source_type"] = dbus.UInt32(source_type)
            if config.sc_version >= 5:
                props["mapping_id"] = dbus.String(f"mapping-{node}")
            streams.append(
                dbus.Struct((dbus.UInt32(node), dbus.Dictionary(props, signature="sv")), signature="ua{sv}")
            )

        if session.source_types & 1:
            stream(42, 0, 1, (0, 0), (1920, 1080))
            stream(43, 1, 1, (1920, 0), (1280, 1024))
        if session.source_types & 4:
            stream(44, 2, 4, None, (1280, 720))
        return dbus.Array(streams, signature="(ua{sv})")

    def pipe_with(self, text):
        read_end, write_end = os.pipe()
        os.write(write_end, text)
        os.close(write_end)
        fd = dbus.types.UnixFd(read_end)
        os.close(read_end)
        return fd

    # --- org.freedesktop.DBus.Properties

    def properties(self, interface):
        config = self.config
        if interface == RD:
            return {
                "version": dbus.UInt32(config.rd_version),
                "AvailableDeviceTypes": dbus.UInt32(config.device_types),
            }
        if interface == SC:
            props = {
                "version": dbus.UInt32(config.sc_version),
                "AvailableSourceTypes": dbus.UInt32(config.source_types),
            }
            if config.sc_version >= 2:
                props["AvailableCursorModes"] = dbus.UInt32(config.cursor_modes)
            return props
        raise Failure(f"No such interface {interface}", INVALID_ARGS)

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        props = self.properties(interface)
        if name not in props:
            raise Failure(f"No such property {name}", INVALID_ARGS)
        return props[name]

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.properties(interface), signature="sv")

    # --- RemoteDesktop

    @dbus.service.method(RD, in_signature="a{sv}", out_signature="o", sender_keyword="sender",
                         async_callbacks=("ok", "err"))
    def CreateSession(self, options, sender, ok, err):
        check_types(options, {"handle_token": dbus.String, "session_handle_token": dbus.String})
        calls.append(describe("CreateSession", options))
        token = options.get("session_handle_token")
        if token is None:
            raise Failure("session_handle_token missing", INVALID_ARGS)
        path = f"{PATH}/session/{sender_element(sender)}/{token}"
        self.sessions[path] = Session(self.connection, path, sender)
        request = self.new_request(sender, options)
        self.answer(request, ok, 0, {"session_handle": dbus.String(path)})

    @dbus.service.method(RD, in_signature="oa{sv}", out_signature="o", sender_keyword="sender",
                         async_callbacks=("ok", "err"))
    def SelectDevices(self, handle, options, sender, ok, err):
        session = self.session(handle, sender)
        check_types(options, {"types": dbus.UInt32, "persist_mode": dbus.UInt32, "restore_token": dbus.String})
        if self.config.rd_version < 2 and ("persist_mode" in options or "restore_token" in options):
            raise Failure("persist_mode and restore_token need RemoteDesktop version 2", INVALID_ARGS)
        calls.append(describe("SelectDevices", options))
        session.device_types = int(options.get("types", 7))
        session.persist_mode = int(options.get("persist_mode", 0))
        self.answer(self.new_request(sender, options), ok, 0, {})

    @dbus.service.method(RD, in_signature="osa{sv}", out_signature="o", sender_keyword="sender",
                         async_callbacks=("ok", "err"))
    def Start(self, handle, parent_window, options, sender, ok, err):
        session = self.session(handle, sender)
        config = self.config
        calls.append(describe("Start", options, parent_window=parent_window))
        request = self.new_request(sender, options)
        if config.start_never:
            ok(request.path)
            return
        results = {}
        if config.start_response == 0:
            results["devices"] = dbus.UInt32(session.device_types & config.device_types)
            results["clipboard_enabled"] = dbus.Boolean(False)
            results["streams"] = self.streams(session)
            if session.persist_mode == 2:
                self.tokens += 1
                results["restore_token"] = dbus.String(f"token-{self.tokens}")
            if config.close_after_ms >= 0:
                GLib.timeout_add(config.close_after_ms + config.start_delay_ms, session.close_from_portal)
        self.answer(request, ok, config.start_response, results, config.start_delay_ms)

    @dbus.service.method(RD, in_signature="oa{sv}", out_signature="h", sender_keyword="sender")
    def ConnectToEIS(self, handle, options, sender):
        session = self.session(handle, sender)
        if self.config.rd_version < 2:
            raise Failure("No such method ConnectToEIS", "org.freedesktop.DBus.Error.UnknownMethod")
        calls.append(describe("ConnectToEIS", options))
        session.eis = True
        return self.pipe_with(b"eis")

    def notify(self, handle, sender, line):
        session = self.session(handle, sender)
        if session.eis:
            raise Failure("input goes through EIS", "org.freedesktop.portal.Error.NotAllowed")
        calls.append(line)

    @dbus.service.method(RD, in_signature="oa{sv}iu", out_signature="", sender_keyword="sender")
    def NotifyKeyboardKeycode(self, handle, options, keycode, state, sender):
        self.notify(handle, sender, describe("NotifyKeyboardKeycode", options, keycode=keycode, state=state))

    @dbus.service.method(RD, in_signature="oa{sv}dd", out_signature="", sender_keyword="sender")
    def NotifyPointerMotion(self, handle, options, dx, dy, sender):
        self.notify(handle, sender, describe("NotifyPointerMotion", options, dx=dx, dy=dy))

    @dbus.service.method(RD, in_signature="oa{sv}udd", out_signature="", sender_keyword="sender")
    def NotifyPointerMotionAbsolute(self, handle, options, stream, x, y, sender):
        self.notify(handle, sender, describe("NotifyPointerMotionAbsolute", options, stream=stream, x=x, y=y))

    @dbus.service.method(RD, in_signature="oa{sv}iu", out_signature="", sender_keyword="sender")
    def NotifyPointerButton(self, handle, options, button, state, sender):
        self.notify(handle, sender, describe("NotifyPointerButton", options, button=button, state=state))

    @dbus.service.method(RD, in_signature="oa{sv}ui", out_signature="", sender_keyword="sender")
    def NotifyPointerAxisDiscrete(self, handle, options, axis, steps, sender):
        self.notify(handle, sender, describe("NotifyPointerAxisDiscrete", options, axis=axis, steps=steps))

    # --- ScreenCast

    @dbus.service.method(SC, in_signature="oa{sv}", out_signature="o", sender_keyword="sender",
                         async_callbacks=("ok", "err"))
    def SelectSources(self, handle, options, sender, ok, err):
        session = self.session(handle, sender)
        config = self.config
        check_types(options, {"types": dbus.UInt32, "multiple": dbus.Boolean, "cursor_mode": dbus.UInt32})
        # As xdg-desktop-portal's screen-cast.c does:
        if "persist_mode" in options or "restore_token" in options:
            raise Failure("Remote desktop sessions cannot persist", INVALID_ARGS)
        if "cursor_mode" in options:
            mode = int(options["cursor_mode"])
            if config.sc_version < 2 or (mode & config.cursor_modes) != mode or bin(mode).count("1") != 1:
                raise Failure(f"cursor mode {mode} is not available", INVALID_ARGS)
        types = int(options.get("types", 1))
        if types & ~config.source_types:
            raise Failure(f"source types {types} are not available", INVALID_ARGS)
        calls.append(describe("SelectSources", options))
        session.source_types = types
        self.answer(self.new_request(sender, options), ok, 0, {})

    @dbus.service.method(SC, in_signature="oa{sv}", out_signature="h", sender_keyword="sender")
    def OpenPipeWireRemote(self, handle, options, sender):
        self.session(handle, sender)
        calls.append(describe("OpenPipeWireRemote", options))
        return self.pipe_with(b"pipewire")


class Mock(dbus.service.Object):
    def __init__(self, bus, portal):
        super().__init__(bus, "/org/farland/Mock")
        self.portal = portal

    @dbus.service.method(MOCK, in_signature="", out_signature="as")
    def Calls(self):
        return dbus.Array(calls, signature="s")

    @dbus.service.method(MOCK, in_signature="", out_signature="")
    def CloseSessions(self):
        for session in list(self.portal.sessions.values()):
            session.close_from_portal()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", required=True)
    parser.add_argument("--rd-version", type=int, default=2)
    parser.add_argument("--sc-version", type=int, default=5)
    parser.add_argument("--device-types", type=int, default=7)
    parser.add_argument("--source-types", type=int, default=7)
    parser.add_argument("--cursor-modes", type=int, default=7)
    parser.add_argument("--start-response", type=int, default=0)
    parser.add_argument("--start-delay-ms", type=int, default=0)
    parser.add_argument("--start-never", action="store_true")
    parser.add_argument("--close-after-ms", type=int, default=-1)
    parser.add_argument("--response-before-reply", action="store_true")
    parser.add_argument("--legacy-request-path", action="store_true")
    config = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(config.address)
    portal = Portal(bus, config)
    mock = Mock(bus, portal)
    name = dbus.service.BusName(PORTAL, bus, do_not_queue=True)
    print("ready", flush=True)
    try:
        GLib.MainLoop().run()
    finally:
        del name, mock, portal


if __name__ == "__main__":
    main()
