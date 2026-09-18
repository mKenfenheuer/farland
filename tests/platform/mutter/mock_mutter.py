#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

"""A fake Mutter for the Mutter backend's tests.

Implements what farland uses of org.gnome.Mutter.RemoteDesktop (with its
Session objects) and org.gnome.Mutter.ScreenCast (with Session and Stream
objects), as Mutter 50 has them, and checks argument types and the caller
strictly: like Mutter, only the connection that created a session may use it.

org.farland.Mock at /org/farland/Mock plays the rest of the desktop:
- Calls: the calls seen, one line each, the method name followed by its
  options or arguments as sorted key=value pairs;
- CloseSessions: Mutter closes every session; Vanish: Mutter leaves the bus;
- SetNodeIds: the PipeWire node ids the next streams announce (by default
  1000, 1001, ...);
- Copy, Paste and Written: the desktop's side of the clipboard.

Prints "ready" once it owns the Mutter names. Exits with 77 when python3-dbus
or PyGObject is missing, so the tests can skip.
"""

import argparse
import fcntl
import os
import sys
import threading

try:
    import dbus
    import dbus.mainloop.glib
    import dbus.service
    from gi.repository import GLib
except ImportError as error:
    print(f"mock_mutter: {error}", file=sys.stderr)
    sys.exit(77)

RD_NAME = "org.gnome.Mutter.RemoteDesktop"
SC_NAME = "org.gnome.Mutter.ScreenCast"
RD = "org.gnome.Mutter.RemoteDesktop"
RD_SESSION = "org.gnome.Mutter.RemoteDesktop.Session"
SC = "org.gnome.Mutter.ScreenCast"
SC_SESSION = "org.gnome.Mutter.ScreenCast.Session"
STREAM = "org.gnome.Mutter.ScreenCast.Stream"
PROPERTIES = "org.freedesktop.DBus.Properties"
MOCK = "org.farland.Mock"

FAILED = "org.freedesktop.DBus.Error.Failed"
ACCESS_DENIED = "org.freedesktop.DBus.Error.AccessDenied"
INVALID_ARGS = "org.freedesktop.DBus.Error.InvalidArgs"
LIMITS = "org.freedesktop.DBus.Error.LimitsExceeded"
F_GET_SEALS = 1034
F_SEAL_WRITE = 0x0008

calls = []
counter = {"session": 0, "stream": 0, "node": 1000, "virtual": 0}
node_ids = []


class Failure(dbus.exceptions.DBusException):
    def __init__(self, message, name=FAILED):
        super().__init__(message, name=name)


def fmt(value):
    if isinstance(value, dbus.Boolean):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(int(value))
    return str(value)


def describe(method, options=None, **args):
    items = dict(args)
    items.update(options or {})
    return " ".join([method] + [f"{key}={fmt(items[key])}" for key in sorted(items)])


def check_types(options, expected):
    for key, value in options.items():
        if key in expected and not isinstance(value, expected[key]):
            raise Failure(f"option {key} has type {type(value).__name__}", INVALID_ARGS)


class Properties(dbus.service.Object):
    """An object with read-only properties: self.props() -> {interface: {name: value}}."""

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        props = self.props().get(str(interface))
        if props is None or name not in props:
            raise Failure(f"No such property {interface}.{name}", INVALID_ARGS)
        return props[name]

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.props().get(str(interface), {}), signature="sv")


class Stream(Properties):
    def __init__(self, bus, path, session, options):
        super().__init__(bus, path)
        self.path = path
        self.session = session
        self.options = options
        self.started = False
        self.stopped = False
        self.node = None

    def props(self):
        params = {}
        if not self.session.mock.config.no_mapping_id:
            params["mapping-id"] = dbus.String(f"mapping-{self.path.rsplit('/', 1)[1]}")
        return {STREAM: {"Parameters": dbus.Dictionary(params, signature="sv")}}

    def check(self, sender):
        if sender != self.session.sender:
            raise Failure("Permission denied", ACCESS_DENIED)

    def begin(self):
        self.started = True
        self.node = node_ids.pop(0) if node_ids else counter["node"]
        counter["node"] += 1
        # Mutter creates the virtual monitor the stream shows.
        display_config = self.session.mock.display_config
        if display_config is not None:
            display_config.add_virtual(f"Meta-{counter['virtual']}", 1920, 1080)
            counter["virtual"] += 1
        GLib.idle_add(self.announce)

    def announce(self):
        if not self.stopped:
            self.PipeWireStreamAdded(dbus.UInt32(self.node))
        return False

    @dbus.service.method(STREAM, in_signature="", out_signature="", sender_keyword="sender")
    def Start(self, sender):
        self.check(sender)
        calls.append(f"Stream.Start {self.path}")
        if not self.session.rd.started:
            raise Failure("Failed to start stream: session not started")
        self.begin()

    @dbus.service.method(STREAM, in_signature="", out_signature="", sender_keyword="sender")
    def Stop(self, sender):
        self.check(sender)
        calls.append(f"Stream.Stop {self.path}")
        self.stopped = True

    @dbus.service.signal(STREAM, signature="u")
    def PipeWireStreamAdded(self, node_id):
        pass


class ScreenCastSession(dbus.service.Object):
    def __init__(self, bus, path, sender, rd, mock):
        super().__init__(bus, path)
        self.path = path
        self.sender = sender
        self.rd = rd
        self.mock = mock
        self.streams = []

    @dbus.service.method(SC_SESSION, in_signature="a{sv}", out_signature="o", sender_keyword="sender")
    def RecordVirtual(self, options, sender):
        if sender != self.sender:
            raise Failure("Permission denied", ACCESS_DENIED)
        check_types(options, {"cursor-mode": dbus.UInt32, "is-platform": dbus.Boolean})
        calls.append(describe("RecordVirtual", options))
        counter["stream"] += 1
        path = f"/org/gnome/Mutter/ScreenCast/Stream/u{counter['stream']}"
        stream = Stream(self.connection, path, self, options)
        self.streams.append(stream)
        return dbus.ObjectPath(path)

    @dbus.service.method(SC_SESSION, in_signature="", out_signature="")
    def Start(self):
        raise Failure("Must be started from remote desktop session")

    @dbus.service.method(SC_SESSION, in_signature="", out_signature="")
    def Stop(self):
        raise Failure("Must be stopped from remote desktop session")

    @dbus.service.signal(SC_SESSION, signature="")
    def Closed(self):
        pass


class RemoteDesktopSession(Properties):
    def __init__(self, bus, path, sender, mock):
        super().__init__(bus, path)
        self.path = path
        self.sender = sender
        self.mock = mock
        self.session_id = f"session-{path.rsplit('/', 1)[1]}"
        self.started = False
        self.closed = False
        self.screen_cast = None
        self.clipboard = False
        self.reading = None
        self.transfers = {}

    def props(self):
        props = {"SessionId": dbus.String(self.session_id)}
        if self.mock.config.empty_keymap_capabilities:
            props["KeymapCapabilities"] = dbus.Dictionary({}, signature="sv")
        elif not self.mock.config.no_keymap:
            props["KeymapCapabilities"] = dbus.Dictionary(
                {
                    "supported-keymap-types": dbus.Array([dbus.UInt32(0)], signature="u"),
                    "supported-xkb-keymap-formats": dbus.Array([dbus.UInt32(1), dbus.UInt32(2)], signature="u"),
                },
                signature="sv",
            )
        return {RD_SESSION: props}

    def check(self, sender):
        if self.closed:
            raise Failure("session closed")
        if sender != self.sender:
            raise Failure("Permission denied", ACCESS_DENIED)

    def close(self):
        if not self.closed:
            self.closed = True
            self.Closed()
            if self.screen_cast is not None:
                self.screen_cast.Closed()

    @dbus.service.method(RD_SESSION, in_signature="", out_signature="", sender_keyword="sender")
    def Start(self, sender):
        self.check(sender)
        if self.started:
            raise Failure("Already started")
        calls.append("RemoteDesktop.Start")
        self.started = True
        for stream in self.screen_cast.streams if self.screen_cast else []:
            if not stream.stopped:
                stream.begin()

    @dbus.service.method(RD_SESSION, in_signature="", out_signature="", sender_keyword="sender")
    def Stop(self, sender):
        self.check(sender)
        calls.append("RemoteDesktop.Stop")
        self.close()

    @dbus.service.signal(RD_SESSION, signature="")
    def Closed(self):
        pass

    @dbus.service.method(RD_SESSION, in_signature="a{sv}", out_signature="h", sender_keyword="sender")
    def ConnectToEIS(self, options, sender):
        self.check(sender)
        check_types(options, {"device-types": dbus.UInt32})
        calls.append(describe("ConnectToEIS", options))
        read_end, write_end = os.pipe()
        os.close(write_end)
        fd = dbus.types.UnixFd(read_end)
        os.close(read_end)
        return fd

    @dbus.service.method(RD_SESSION, in_signature="a{sv}", out_signature="", sender_keyword="sender")
    def SetKeymap(self, options, sender):
        self.check(sender)
        check_types(options, {"keymap-type": dbus.UInt32, "xkb-keymap-format": dbus.UInt32})
        handle = options.get("xkb-keymap")
        if not isinstance(handle, dbus.types.UnixFd):
            raise Failure("No keymap handle")
        fd = handle.take()
        sealed = (fcntl.fcntl(fd, F_GET_SEALS) & F_SEAL_WRITE) != 0
        os.lseek(fd, 0, os.SEEK_SET)
        text = b""
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            text += chunk
        os.close(fd)
        described = {k: v for k, v in options.items() if k != "xkb-keymap"}
        calls.append(describe("SetKeymap", described, sealed=dbus.Boolean(sealed),
                              text=text.split(b" ", 1)[0].decode(), nul=dbus.Boolean(text.endswith(b"\0"))))

    # --- Clipboard

    def announce(self, mimes, own):
        options = {}
        if mimes is not None:
            # A tuple around the list, as Mutter's generate_owner_changed_variant() builds it.
            options = {"mime-types": dbus.Struct((dbus.Array(mimes, signature="s"),), signature="as"),
                       "session-is-owner": dbus.Boolean(own)}
        self.SelectionOwnerChanged(dbus.Dictionary(options, signature="sv"))

    def clipboard_check(self, sender):
        self.check(sender)
        if not self.clipboard:
            raise Failure("Clipboard not enabled")

    @dbus.service.method(RD_SESSION, in_signature="a{sv}", out_signature="", sender_keyword="sender")
    def EnableClipboard(self, options, sender):
        self.check(sender)
        if self.clipboard:
            raise Failure("Already enabled")
        calls.append(describe("EnableClipboard", options))
        self.clipboard = True
        if self.mock.local:
            # Before the reply, as Mutter does.
            self.announce(list(self.mock.local), False)

    @dbus.service.method(RD_SESSION, in_signature="", out_signature="", sender_keyword="sender")
    def DisableClipboard(self, sender):
        self.clipboard_check(sender)
        calls.append("DisableClipboard")
        self.clipboard = False

    @dbus.service.method(RD_SESSION, in_signature="a{sv}", out_signature="", sender_keyword="sender")
    def SetSelection(self, options, sender):
        self.clipboard_check(sender)
        mimes = options.get("mime-types")
        if mimes is not None and (not isinstance(mimes, dbus.Array) or
                                  any(not isinstance(m, dbus.String) for m in mimes)):
            raise Failure("mime-types must be as", INVALID_ARGS)
        calls.append("SetSelection mime-types=" + ",".join(str(m) for m in mimes or []))
        self.announce([str(m) for m in mimes or []], True)

    @dbus.service.method(RD_SESSION, in_signature="u", out_signature="h", sender_keyword="sender")
    def SelectionWrite(self, serial, sender):
        self.clipboard_check(sender)
        transfer = self.mock.transfers.get(int(serial))
        if transfer is None:
            raise Failure(f"no transfer {serial}", INVALID_ARGS)
        read_end, write_end = os.pipe()

        def drain():
            chunks = []
            while True:
                data = os.read(read_end, 65536)
                if not data:
                    break
                chunks.append(data)
            os.close(read_end)
            transfer["data"] = b"".join(chunks)

        threading.Thread(target=drain, daemon=True).start()
        fd = dbus.types.UnixFd(write_end)
        os.close(write_end)
        return fd

    @dbus.service.method(RD_SESSION, in_signature="ub", out_signature="", sender_keyword="sender")
    def SelectionWriteDone(self, serial, success, sender):
        self.clipboard_check(sender)
        calls.append(describe("SelectionWriteDone", serial=serial, success=success))
        if int(serial) in self.mock.transfers:
            self.mock.transfers[int(serial)]["done"] = bool(success)

    @dbus.service.method(RD_SESSION, in_signature="s", out_signature="h", sender_keyword="sender")
    def SelectionRead(self, mime_type, sender):
        self.clipboard_check(sender)
        if self.reading is not None and self.reading.is_alive():
            raise Failure("Tried to read in parallel", LIMITS)
        calls.append(describe("SelectionRead", mime_type=mime_type))
        data = self.mock.local.get(str(mime_type))
        if data is None:
            raise Failure(f"the clipboard has no {mime_type}")
        read_end, write_end = os.pipe()

        def feed():
            view = memoryview(data)
            try:
                while view:
                    view = view[os.write(write_end, view):]
            finally:
                os.close(write_end)

        self.reading = threading.Thread(target=feed, daemon=True)
        self.reading.start()
        fd = dbus.types.UnixFd(read_end)
        os.close(read_end)
        return fd

    @dbus.service.signal(RD_SESSION, signature="a{sv}")
    def SelectionOwnerChanged(self, options):
        pass

    @dbus.service.signal(RD_SESSION, signature="su")
    def SelectionTransfer(self, mime_type, serial):
        pass


class RemoteDesktop(Properties):
    def __init__(self, bus, mock):
        super().__init__(bus, "/org/gnome/Mutter/RemoteDesktop")
        self.mock = mock

    def props(self):
        return {RD: {"Version": dbus.Int32(self.mock.config.rd_version),
                     "SupportedDeviceTypes": dbus.UInt32(self.mock.config.device_types)}}

    @dbus.service.method(RD, in_signature="", out_signature="o", sender_keyword="sender")
    def CreateSession(self, sender):
        calls.append("RemoteDesktop.CreateSession")
        counter["session"] += 1
        path = f"/org/gnome/Mutter/RemoteDesktop/Session/u{counter['session']}"
        session = RemoteDesktopSession(self.connection, path, sender, self.mock)
        self.mock.sessions[session.session_id] = session
        return dbus.ObjectPath(path)


class ScreenCast(Properties):
    def __init__(self, bus, mock):
        super().__init__(bus, "/org/gnome/Mutter/ScreenCast")
        self.mock = mock

    def props(self):
        return {SC: {"Version": dbus.Int32(self.mock.config.sc_version)}}

    @dbus.service.method(SC, in_signature="a{sv}", out_signature="o", sender_keyword="sender")
    def CreateSession(self, properties, sender):
        check_types(properties, {"remote-desktop-session-id": dbus.String, "disable-animations": dbus.Boolean})
        calls.append(describe("ScreenCast.CreateSession", properties))
        rd = self.mock.sessions.get(str(properties.get("remote-desktop-session-id", "")))
        if rd is None or rd.sender != sender:
            raise Failure("No remote desktop session found")
        counter["session"] += 1
        path = f"/org/gnome/Mutter/ScreenCast/Session/u{counter['session']}"
        rd.screen_cast = ScreenCastSession(self.connection, path, sender, rd, self.mock)
        return dbus.ObjectPath(path)


class DisplayConfig(dbus.service.Object):
    """org.gnome.Mutter.DisplayConfig: the seat's monitor plus the virtual ones."""

    def __init__(self, bus, mock):
        super().__init__(bus, "/org/gnome/Mutter/DisplayConfig")
        self.mock = mock
        self.serial = 1
        # connector -> (vendor, product, [(mode, width, height)], current mode)
        self.monitors = {}
        if not mock.config.no_seat_monitor:
            self.monitors["Virtual-1"] = ("RHT", "QEMU Monitor", [("1280x800@60.000", 1280, 800)], "1280x800@60.000")
        # Logical monitors: connector -> (x, y, scale, primary)
        self.logical = {c: (0, 0, 1.25, True) for c in self.monitors}

    def add_virtual(self, connector, width, height):
        mode = f"{width}x{height}@60.000"
        self.monitors[connector] = ("MetaVendor", "Virtual remote monitor", [(mode, width, height)], mode)
        x = sum(self.monitors[c][3] and self.width_of(c) for c in self.logical)
        self.logical[connector] = (x, 0, 1.0, not self.logical)
        self.serial += 1

    def width_of(self, connector):
        vendor, product, modes, current = self.monitors[connector]
        return next((w for (m, w, h) in modes if m == current), 0)

    @dbus.service.method("org.gnome.Mutter.DisplayConfig", in_signature="",
                         out_signature="ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv}")
    def GetCurrentState(self):
        monitors = []
        for connector, (vendor, product, modes, current) in self.monitors.items():
            mode_list = [
                dbus.Struct((dbus.String(m), dbus.Int32(w), dbus.Int32(h), dbus.Double(60.0), dbus.Double(1.0),
                             dbus.Array([dbus.Double(1.0)], signature="d"),
                             dbus.Dictionary({"is-current": dbus.Boolean(m == current),
                                              "is-preferred": dbus.Boolean(True)}, signature="sv")),
                            signature="siiddada{sv}")
                for (m, w, h) in modes
            ]
            monitors.append(dbus.Struct(
                (dbus.Struct((dbus.String(connector), dbus.String(vendor), dbus.String(product), dbus.String("0x1")),
                             signature="ssss"),
                 dbus.Array(mode_list, signature="(siiddada{sv})"),
                 dbus.Dictionary({"display-name": dbus.String(product)}, signature="sv")),
                signature="(ssss)a(siiddada{sv})a{sv}"))
        logical = []
        for connector, (x, y, scale, primary) in self.logical.items():
            vendor, product, modes, current = self.monitors[connector]
            logical.append(dbus.Struct(
                (dbus.Int32(x), dbus.Int32(y), dbus.Double(scale), dbus.UInt32(0), dbus.Boolean(primary),
                 dbus.Array([dbus.Struct((dbus.String(connector), dbus.String(vendor), dbus.String(product),
                                          dbus.String("0x1")), signature="ssss")], signature="(ssss)"),
                 dbus.Dictionary({}, signature="sv")),
                signature="iiduba(ssss)a{sv}"))
        return (dbus.UInt32(self.serial), dbus.Array(monitors, signature="((ssss)a(siiddada{sv})a{sv})"),
                dbus.Array(logical, signature="(iiduba(ssss)a{sv})"), dbus.Dictionary({}, signature="sv"))

    @dbus.service.method("org.gnome.Mutter.DisplayConfig", in_signature="uua(iiduba(ssa{sv}))a{sv}", out_signature="")
    def ApplyMonitorsConfig(self, serial, method, logical_monitors, properties):
        if int(serial) != self.serial:
            raise Failure("stale monitor configuration serial", INVALID_ARGS)
        entries = []
        self.logical = {}
        for (x, y, scale, transform, primary, monitors) in logical_monitors:
            for (connector, mode, props) in monitors:
                self.logical[str(connector)] = (int(x), int(y), float(scale), bool(primary))
                vendor, product, modes, current = self.monitors[str(connector)]
                self.monitors[str(connector)] = (vendor, product, modes, str(mode))
                entries.append(f"{connector}@{x},{y}{'*' if primary else ''}")
        self.serial += 1
        calls.append("ApplyMonitorsConfig method=%d %s" % (int(method), " ".join(entries)))


class Mock(dbus.service.Object):
    def __init__(self, bus, config):
        super().__init__(bus, "/org/farland/Mock")
        self.config = config
        self.sessions = {}
        self.names = []
        # The desktop's clipboard (MIME type -> bytes) and the pastes of the
        # session's clipboard (serial -> {"data", "done"}).
        self.local = {}
        if config.clipboard_owner_at_start:
            self.local = {"text/plain;charset=utf-8": b"at start"}
        self.transfers = {}
        self.serials = 0
        self.display_config = None

    def clipboard_sessions(self):
        return [s for s in self.sessions.values() if s.clipboard and not s.closed]

    @dbus.service.method(MOCK, in_signature="", out_signature="as")
    def Calls(self):
        return dbus.Array(calls, signature="s")

    @dbus.service.method(MOCK, in_signature="", out_signature="")
    def CloseSessions(self):
        for session in list(self.sessions.values()):
            session.close()

    @dbus.service.method(MOCK, in_signature="", out_signature="")
    def Vanish(self):
        for name in self.names:
            del name
        self.names.clear()
        bus = self.connection
        for name in (RD_NAME, SC_NAME, "org.gnome.Mutter.DisplayConfig"):
            bus.release_name(name)

    @dbus.service.method(MOCK, in_signature="au", out_signature="")
    def SetNodeIds(self, ids):
        node_ids.extend(int(i) for i in ids)

    @dbus.service.method(MOCK, in_signature="asas", out_signature="")
    def Copy(self, mime_types, contents):
        """Someone on the desktop copies: one content per MIME type; none clears the clipboard."""
        self.local = {str(m): str(c).encode() for m, c in zip(mime_types, contents)}
        for session in self.clipboard_sessions():
            session.announce([str(m) for m in mime_types] if mime_types else None, False)

    @dbus.service.method(MOCK, in_signature="s", out_signature="u")
    def Paste(self, mime_type):
        """Someone on the desktop pastes; returns the transfer's serial."""
        self.serials += 1
        serial = self.serials
        self.transfers[serial] = {"data": None, "done": None}
        for session in self.clipboard_sessions():
            session.SelectionTransfer(mime_type, dbus.UInt32(serial))
        return dbus.UInt32(serial)

    @dbus.service.method(MOCK, in_signature="u", out_signature="bbs")
    def Written(self, serial):
        """(finished, success, data) of a paste: finished once SelectionWriteDone came and the data is in."""
        transfer = self.transfers.get(int(serial))
        if transfer is None or transfer["done"] is None or (transfer["done"] and transfer["data"] is None):
            return (dbus.Boolean(False), dbus.Boolean(False), "")
        data = (transfer["data"] or b"").decode("utf-8", "replace")
        return (dbus.Boolean(True), dbus.Boolean(transfer["done"]), data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", help="bus address (default: DBUS_SESSION_BUS_ADDRESS)")
    parser.add_argument("--rd-version", type=int, default=1)
    parser.add_argument("--sc-version", type=int, default=4)
    parser.add_argument("--device-types", type=int, default=7)
    parser.add_argument("--no-keymap", action="store_true")
    parser.add_argument("--empty-keymap-capabilities", action="store_true")
    parser.add_argument("--no-seat-monitor", action="store_true")
    parser.add_argument("--no-mapping-id", action="store_true")
    parser.add_argument("--names-after-ms", type=int, default=0)
    parser.add_argument("--clipboard-owner-at-start", action="store_true")
    config = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    address = config.address or os.environ.get("DBUS_SESSION_BUS_ADDRESS")
    bus = dbus.bus.BusConnection(address)
    mock = Mock(bus, config)
    mock.display_config = DisplayConfig(bus, mock)
    objects = [RemoteDesktop(bus, mock), ScreenCast(bus, mock), mock.display_config]

    def take_names():
        for name in (RD_NAME, SC_NAME, "org.gnome.Mutter.DisplayConfig"):
            mock.names.append(dbus.service.BusName(name, bus, do_not_queue=True))
        return False

    if config.names_after_ms > 0:
        GLib.timeout_add(config.names_after_ms, take_names)
    else:
        take_names()
    print("ready", flush=True)
    try:
        GLib.MainLoop().run()
    finally:
        del objects, mock


if __name__ == "__main__":
    main()
