<!--
SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
SPDX-License-Identifier: Apache-2.0
-->

# Asking before somebody at the machine takes a session back

A client that holds a session (`on_local_session = "attach"`) loses it the
moment the person at the machine logs in: the display manager authenticates
them and switches to the session they already have, and nothing in that asks
the client using it.

`pam_farland.so` puts the question in between. Name it in the **account**
stack of the display manager's PAM service, **before** the line that includes
`common-account`: that file ends with `account sufficient pam_localuser.so`,
which finishes the stack for a local user, so anything after the include
never runs.

GDM (`/etc/pam.d/gdm-password`, and `gdm-fingerprint` and `gdm-smartcard`
where those are used):

```
account required        pam_farland.so
@include common-account
```

SDDM (`/etc/pam.d/sddm`) takes the same line in the same place.

What then happens follows `[policy] seat_takeover`:

- `ask` (the default): the client is asked, as `takeover` asks another
  client, and the login waits for the answer. The countdown is
  `takeover_timeout` and `takeover_on_timeout` says what silence means.
  Whoever is logging in is told that farland is asking, so the screen does
  not just sit there.
- `always`: the login goes ahead without asking, which is what happens with
  no module installed at all.
- `never`: the login is refused while a client holds the session.

A refused login is refused twice over. PAM keeps whoever is at the screen
out, and the display manager then gives up the login screen it was showing on
the seat — which hands the seat back to the very session the client is using.
farlandd tells the agent how the login went before it lets PAM answer, so the
agent knows that return is the refusal's doing and not somebody taking the
session: it puts a login screen back on the seat and the client keeps what it
has. Without that, the session would go to the screen at the machine although
nobody was let in.

Nobody is kept out or left waiting by farland going wrong. Without farlandd
on the bus, with no session of theirs held, or on any error, the module
returns at once and the login goes ahead — including when farlandd is not
installed any more. Only `seat_takeover = "never"` and a client answering
"no" ever refuse a login.
