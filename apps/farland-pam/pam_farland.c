// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

/// [policy] seat_takeover: the person at the machine logs in to take back a
/// session an RDP client is holding. The display manager authenticates them
/// and then switches to the session they already have, and nothing in that
/// asks the client that is using it — so this module does, from the account
/// stack of the display manager's PAM service:
///
///     account  required  pam_farland.so
///
/// It asks farlandd whether a client holds that account's session
/// (org.farland.Farland1.SeatTakeoverPending, answered at once), tells the
/// person at the screen that it is asking, and waits for the answer
/// (AwaitSeatTakeover). "No" refuses the login; "yes", a timeout and every
/// kind of trouble let it through.
///
/// Nobody is ever kept out or left waiting because farland is not there:
/// without farlandd on the bus, without a session of theirs being held, or
/// on any error at all, this returns PAM_SUCCESS straight away.

#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <stdint.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define FARLAND_BUS_NAME "org.farland.Farland1"
#define FARLAND_OBJECT_PATH "/org/farland/Farland1"
#define FARLAND_INTERFACE "org.farland.Farland1"

/// How long the first question may take. farlandd answers it without doing
/// anything else, so this only has to cover a busy bus.
#define PENDING_TIMEOUT_US (1000u * 1000u)
/// The longest this holds a login up while the client is asked. farlandd
/// answers as soon as its own countdown is over ([policy] takeover_timeout),
/// so this is only the last word.
#define ANSWER_TIMEOUT_MS (120u * 1000u)
#define ANSWER_CALL_TIMEOUT_US ((uint64_t) (ANSWER_TIMEOUT_MS + 5000u) * 1000u)

/// Whether a client holds `account`'s session, and the cookie to wait on.
/// Any trouble means "no", which lets the login through.
static int takeover_pending(sd_bus *bus, const char *account, uint64_t *cookie)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    sd_bus_message *call = NULL;
    int pending = 0;
    int rc;

    rc = sd_bus_message_new_method_call(bus, &call, FARLAND_BUS_NAME, FARLAND_OBJECT_PATH, FARLAND_INTERFACE,
                                        "SeatTakeoverPending");
    if (rc < 0) {
        goto done;
    }
    /* Never start farlandd for this: if it is not running, nothing holds the
     * session, and the login must not wait for a service to come up. */
    rc = sd_bus_message_set_auto_start(call, 0);
    if (rc < 0) {
        goto done;
    }
    rc = sd_bus_message_append(call, "s", account);
    if (rc < 0) {
        goto done;
    }
    rc = sd_bus_call(bus, call, PENDING_TIMEOUT_US, &error, &reply);
    if (rc < 0) {
        goto done;
    }
    rc = sd_bus_message_read(reply, "bt", &pending, cookie);
    if (rc < 0) {
        goto done;
    }

done:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(call);
    return rc >= 0 && pending;
}

/// Waits for the client's answer. Any trouble means "yes".
static int takeover_allowed(sd_bus *bus, uint64_t cookie)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    sd_bus_message *call = NULL;
    int allowed = 1;
    int rc;

    rc = sd_bus_message_new_method_call(bus, &call, FARLAND_BUS_NAME, FARLAND_OBJECT_PATH, FARLAND_INTERFACE,
                                        "AwaitSeatTakeover");
    if (rc < 0) {
        goto done;
    }
    rc = sd_bus_message_set_auto_start(call, 0);
    if (rc < 0) {
        goto done;
    }
    rc = sd_bus_message_append(call, "tu", cookie, (uint32_t) ANSWER_TIMEOUT_MS);
    if (rc < 0) {
        goto done;
    }
    rc = sd_bus_call(bus, call, ANSWER_CALL_TIMEOUT_US, &error, &reply);
    if (rc < 0) {
        goto done;
    }
    rc = sd_bus_message_read(reply, "b", &allowed);
    if (rc < 0) {
        allowed = 1;
    }

done:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(call);
    return allowed != 0;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *handle, int flags, int argc, const char **argv)
{
    const char *account = NULL;
    sd_bus *bus = NULL;
    uint64_t cookie = 0;
    int allowed;

    (void) flags;
    (void) argc;
    (void) argv;

    if (pam_get_user(handle, &account, NULL) != PAM_SUCCESS || account == NULL || *account == '\0') {
        return PAM_SUCCESS;
    }
    if (sd_bus_open_system(&bus) < 0) {
        return PAM_SUCCESS;
    }
    if (!takeover_pending(bus, account, &cookie)) {
        sd_bus_flush_close_unref(bus);
        return PAM_SUCCESS;
    }

    /* Said before the wait, so that the screen does not just sit there. */
    pam_info(handle, "%s", "Someone is using this session from another computer. Asking them to hand it over…");
    allowed = takeover_allowed(bus, cookie);
    sd_bus_flush_close_unref(bus);

    if (!allowed) {
        pam_error(handle, "%s", "The session is in use from another computer, and was not handed over.");
        return PAM_PERM_DENIED;
    }
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *handle, int flags, int argc, const char **argv)
{
    (void) handle;
    (void) flags;
    (void) argc;
    (void) argv;
    return PAM_SUCCESS;
}
