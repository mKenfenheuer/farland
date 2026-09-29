// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdlib>
#include <cstring>
#include <security/pam_appl.h>
#include <span>
#include <string>

/// A PAM conversation that answers the password prompts with the password
/// in `data` (a `const std::string*`) and nothing else: an informational or
/// error message gets an empty answer, and a prompt that echoes -- a user
/// name, a question -- none. farlandd asks PAM for a password check in two
/// places: enrolment (enrol.cpp) and, with the password a client delegated,
/// opening a session (launcher.cpp).
extern "C" inline int farland_pam_password_conversation(int count, const pam_message** messages,
                                                        pam_response** responses, void* data)
{
    if (count <= 0 || count > PAM_MAX_NUM_MSG) {
        return PAM_CONV_ERR;
    }
    const auto* password = static_cast<const std::string*>(data);
    // PAM frees the responses with free(), so they come from calloc/strdup.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    auto* replies = static_cast<pam_response*>(std::calloc(static_cast<std::size_t>(count), sizeof(pam_response)));
    if (replies == nullptr) {
        return PAM_BUF_ERR;
    }
    const std::span message_list(messages, static_cast<std::size_t>(count));
    const std::span reply_list(replies, static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < message_list.size(); ++i) {
        if (message_list[i]->msg_style == PAM_PROMPT_ECHO_OFF) {
            reply_list[i].resp = ::strdup(password->c_str());
        }
    }
    *responses = replies;
    return PAM_SUCCESS;
}
