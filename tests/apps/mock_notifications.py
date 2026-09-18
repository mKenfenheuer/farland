#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

"""A fake notification service for the takeover prompt tests.

Implements org.freedesktop.Notifications as KDE and GNOME serve it: Notify
with actions, CloseNotification, GetCapabilities and GetServerInformation,
with the argument types checked strictly. --answer says what the user does
with the first notification: press one of its buttons, dismiss it, or
nothing at all.

org.farland.Mock at /org/farland/Mock returns the calls it saw, one line
each: the method and its arguments as key=value pairs.

Prints "ready" once it owns org.freedesktop.Notifications. Exits with 77
when python3-dbus or PyGObject is missing, so the tests can skip.
"""

import argparse
import sys

try:
    import dbus
    import dbus.mainloop.glib
    import dbus.service
    from gi.repository import GLib
except ImportError as error:
    print(f"mock_notifications: {error}", file=sys.stderr)
    sys.exit(77)

NOTIFICATIONS = "org.freedesktop.Notifications"
PATH = "/org/freedesktop/Notifications"
MOCK = "org.farland.Mock"

INVALID_ARGS = "org.freedesktop.DBus.Error.InvalidArgs"

calls = []


class Failure(dbus.exceptions.DBusException):
    def __init__(self, message, name=INVALID_ARGS):
        super().__init__(message, name=name)


class Notifications(dbus.service.Object):
    """The service. Notification ids count from 1; replaces_id keeps one."""

    def __init__(self, bus, config):
        super().__init__(bus, PATH)
        self.config = config
        self.next_id = 1
        self.answered = False

    @dbus.service.method(NOTIFICATIONS, in_signature="", out_signature="as")
    def GetCapabilities(self):
        return ["actions", "body", "body-markup", "persistence"]

    @dbus.service.method(NOTIFICATIONS, in_signature="", out_signature="ssss")
    def GetServerInformation(self):
        return ("mock-notifications", "farland", "1.0", "1.2")

    @dbus.service.method(NOTIFICATIONS, in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout):
        if not isinstance(replaces_id, dbus.UInt32) or not isinstance(expire_timeout, dbus.Int32):
            raise Failure("Notify takes u for replaces_id and i for expire_timeout")
        urgency = hints.get("urgency")
        if urgency is not None and not isinstance(urgency, dbus.Byte):
            raise Failure("the urgency hint is a byte")
        identifier = int(replaces_id)
        if identifier == 0:
            identifier = self.next_id
            self.next_id += 1
        calls.append(
            f"Notify app={app_name} replaces={int(replaces_id)} summary={summary} "
            f"body={body.replace(chr(10), ' | ')} actions={','.join(str(a) for a in actions)} "
            f"urgency={int(urgency) if urgency is not None else -1} expire={int(expire_timeout)}"
        )
        if not self.answered and self.config.answer != "none":
            self.answered = True
            GLib.timeout_add(self.config.delay_ms, self.answer, identifier)
        return dbus.UInt32(identifier)

    def answer(self, identifier):
        """What the user does, a moment after the prompt appears."""
        if self.config.answer == "dismiss":
            self.NotificationClosed(dbus.UInt32(identifier), dbus.UInt32(2))
        else:
            self.ActionInvoked(dbus.UInt32(identifier), self.config.answer)
        return False

    @dbus.service.method(NOTIFICATIONS, in_signature="u", out_signature="")
    def CloseNotification(self, identifier):
        calls.append(f"CloseNotification id={int(identifier)}")
        self.NotificationClosed(dbus.UInt32(identifier), dbus.UInt32(3))

    @dbus.service.signal(NOTIFICATIONS, signature="us")
    def ActionInvoked(self, identifier, action_key):
        pass

    @dbus.service.signal(NOTIFICATIONS, signature="uu")
    def NotificationClosed(self, identifier, reason):
        pass


class Mock(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/farland/Mock")

    @dbus.service.method(MOCK, in_signature="", out_signature="as")
    def Calls(self):
        return calls


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", required=True)
    # What the user does with the prompt: press a button by its action key,
    # dismiss the notification, or leave it standing.
    parser.add_argument("--answer", default="none")
    parser.add_argument("--delay-ms", type=int, default=200)
    config = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(config.address)
    service = Notifications(bus, config)
    mock = Mock(bus)
    name = dbus.service.BusName(NOTIFICATIONS, bus, do_not_queue=True)
    print("ready", flush=True)
    try:
        GLib.MainLoop().run()
    finally:
        del name, mock, service


if __name__ == "__main__":
    main()
