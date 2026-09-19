// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farlandctl: administration for farland-server and farlandd. It manages the
// NLA user store (the file holds NT hashes, never passwords) and enrols the
// calling user with farlandd (self-enrolment, docs/PLAN.md §3.5).

#include <farland/auth/credential_store.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/base/text.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

#ifdef FARLAND_HAVE_LIBSYSTEMD
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX leaves its declaration to the caller
#endif

namespace {

using farland::SecretString;
using farland::auth::CredentialStore;

std::filesystem::path default_store()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "farland" / "users";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "farland" / "users";
    }
    return std::filesystem::current_path() / ".farland" / "users";
}

void usage()
{
    std::cout << "usage: farlandctl [--file FILE] COMMAND\n"
                 "  passwd [--domain DOMAIN] [--stdin]         enrol yourself for remote login with farlandd:\n"
                 "                                            it checks your account password (polkit and\n"
                 "                                            PAM) and stores what NLA needs\n"
                 "  passwd USER [--domain DOMAIN] [--local-account ACCOUNT] [--stdin]\n"
                 "                                            add a user to FILE or change the password\n"
                 "  remove USER [--domain DOMAIN]             remove a user\n"
                 "  users                                     list users\n"
                 "  sessions                                  list the sessions farlandd runs\n"
                 "  terminate ID | --user USER                end a session, or every session of a user\n"
                 "\n"
                 "FILE defaults to $XDG_CONFIG_HOME/farland/users, which farland-server reads.\n"
                 "An empty domain (the default) matches any domain the client sends.\n"
                 "--local-account names the local account a multi-session login runs as\n"
                 "(default: the user name); changing only the password keeps it.\n"
                 "--stdin reads the password from the first line of standard input.\n";
}

/// Reads one line from standard input with terminal echo off.
std::optional<SecretString> read_password(std::string_view prompt, bool from_stdin)
{
    const bool tty = !from_stdin && ::isatty(STDIN_FILENO) != 0;
    termios saved{};
    if (tty) {
        std::cerr << prompt << std::flush;
        ::tcgetattr(STDIN_FILENO, &saved);
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    std::string line;
    const bool ok = static_cast<bool>(std::getline(std::cin, line));
    if (tty) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        std::cerr << "\n";
    }
    if (!ok) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return SecretString(std::move(line));
}

int passwd(const std::filesystem::path& file, const std::string& user, const std::string& domain,
           const std::string& local_account, bool from_stdin)
{
    auto store = CredentialStore::load(file);
    if (!store) {
        std::cerr << "farlandctl: cannot load " << file.string() << ": " << store.error().message() << "\n";
        return 1;
    }
    auto password = read_password("Password for " + user + ": ", from_stdin);
    if (!password || password->view().empty()) {
        std::cerr << "farlandctl: no password given\n";
        return 1;
    }
    if (!from_stdin && ::isatty(STDIN_FILENO) != 0) {
        auto again = read_password("Repeat password: ", false);
        if (!again || again->view() != password->view()) {
            std::cerr << "farlandctl: passwords do not match\n";
            return 1;
        }
    }
    auto hash = farland::auth::ntlm::nt_hash(password->view());
    store->set(user, domain, hash);
    farland::secure_zero(hash);
    if (!local_account.empty()) {
        static_cast<void>(store->set_local_account(user, domain, local_account));
    }
    if (auto saved = store->save(file); !saved) {
        std::cerr << "farlandctl: " << saved.error().message() << "\n";
        return 1;
    }
    std::cout << "farlandctl: password set for " << (domain.empty() ? user : domain + "\\" + user) << "\n";
    return 0;
}

#ifdef FARLAND_HAVE_LIBSYSTEMD

/// polkit asks for a password through an agent; on a terminal that is
/// pkttyagent for this process, as systemctl does. Returns its pid, or -1
/// when there is no terminal or it could not be started.
pid_t start_polkit_agent()
{
    if (::isatty(STDIN_FILENO) == 0) {
        return -1;
    }
    std::array<int, 2> notify{-1, -1};
    if (::pipe(notify.data()) != 0) {
        return -1;
    }
    pid_t agent = -1;
    const std::string pid = std::to_string(::getpid());
    const std::string fd = std::to_string(notify[1]);
    std::array<std::string, 6> args{"pkttyagent", "--process", pid, "--notify-fd", fd, "--fallback"};
    std::array<char*, 7> argv{};
    for (std::size_t i = 0; i < args.size(); ++i) {
        argv.at(i) = args.at(i).data();
    }
    if (::posix_spawnp(&agent, "pkttyagent", nullptr, nullptr, argv.data(), environ) != 0) {
        agent = -1;
    }
    ::close(notify[1]);
    pollfd pfd{notify[0], POLLIN, 0};
    static_cast<void>(::poll(&pfd, 1, 5000));  // closed once the agent is registered
    ::close(notify[0]);
    return agent;
}

void stop_polkit_agent(pid_t agent)
{
    if (agent > 0) {
        ::kill(agent, SIGTERM);
        int status = 0;
        ::waitpid(agent, &status, 0);
    }
}

#endif

/// Self-enrolment through farlandd (org.farland.Farland1.EnrolSelf).
int enrol_self(const std::string& domain, bool from_stdin)
{
    auto password = read_password("Your account password: ", from_stdin);
    if (!password || password->view().empty()) {
        std::cerr << "farlandctl: no password given\n";
        return 1;
    }
#ifdef FARLAND_HAVE_LIBSYSTEMD
    sd_bus* bus = nullptr;
    if (const int rc = sd_bus_open_system(&bus); rc < 0) {
        std::cerr << "farlandctl: cannot reach the system bus: " << std::strerror(-rc) << "\n";
        return 1;
    }
    const pid_t agent = from_stdin ? -1 : start_polkit_agent();
    sd_bus_set_method_call_timeout(bus, std::uint64_t{300} * 1'000'000);
    sd_bus_error error = {};
    const int rc =
        sd_bus_call_method(bus, "org.farland.Farland1", "/org/farland/Farland1", "org.farland.Farland1", "EnrolSelf",
                           &error, nullptr, "ss", std::string(password->view()).c_str(), domain.c_str());
    stop_polkit_agent(agent);
    int result = 0;
    if (rc < 0) {
        std::cerr << "farlandctl: enrolment failed: " << (error.message != nullptr ? error.message : std::strerror(-rc))
                  << "\n";
        result = 1;
    } else {
        std::cout << "farlandctl: you can now log in over RDP with your account and this password\n";
    }
    sd_bus_error_free(&error);
    sd_bus_flush_close_unref(bus);
    return result;
#else
    static_cast<void>(domain);
    std::cerr << "farlandctl: this build cannot talk to farlandd (no libsystemd); use passwd USER with --file\n";
    return 1;
#endif
}

#ifdef FARLAND_HAVE_LIBSYSTEMD

/// Opens the system bus and names farlandd, or explains why not.
sd_bus* open_daemon_bus()
{
    sd_bus* bus = nullptr;
    if (const int rc = sd_bus_open_system(&bus); rc < 0) {
        std::cerr << "farlandctl: cannot reach the system bus: " << std::strerror(-rc) << "\n";
        return nullptr;
    }
    return bus;
}

/// A polkit agent on the terminal, so that a question can be answered where
/// farlandctl runs. Returns its pid, or -1.
std::string age(std::uint64_t seconds)
{
    if (seconds < 60) {
        return std::format("{}s", seconds);
    }
    if (seconds < 3600) {
        return std::format("{}m{:02}s", seconds / 60, seconds % 60);
    }
    return std::format("{}h{:02}m", seconds / 3600, (seconds % 3600) / 60);
}

int sessions()
{
    sd_bus* bus = open_daemon_bus();
    if (bus == nullptr) {
        return 1;
    }
    const pid_t agent = start_polkit_agent();
    sd_bus_error error = {};
    sd_bus_message* reply = nullptr;
    const int rc = sd_bus_call_method(bus, "org.farland.Farland1", "/org/farland/Farland1", "org.farland.Farland1",
                                      "ListSessions", &error, &reply, "");
    stop_polkit_agent(agent);
    if (rc < 0) {
        std::cerr << "farlandctl: cannot list sessions: "
                  << (error.message != nullptr ? error.message : std::strerror(-rc)) << "\n";
        sd_bus_error_free(&error);
        sd_bus_flush_close_unref(bus);
        return 1;
    }
    int result = 0;
    std::size_t shown = 0;
    if (sd_bus_message_enter_container(reply, 'a', "(usssbbsttu)") >= 0) {
        std::cout << std::format("{:>4}  {:<16} {:<9} {:<8} {:<22} {:>8} {:>8}\n", "ID", "ACCOUNT", "STATE", "DESKTOP",
                                 "CLIENT", "AGE", "IDLE");
        for (;;) {
            std::uint32_t id = 0;
            const char* account = nullptr;
            const char* state = nullptr;
            const char* desktop = nullptr;
            int attached = 0;
            int connected = 0;
            const char* peer = nullptr;
            std::uint64_t age_seconds = 0;
            std::uint64_t disconnected = 0;
            std::uint32_t idle = 0;
            const int got = sd_bus_message_read(reply, "(usssbbsttu)", &id, &account, &state, &desktop, &attached,
                                                &connected, &peer, &age_seconds, &disconnected, &idle);
            if (got <= 0) {
                break;
            }
            std::string client = connected != 0 ? std::string(peer != nullptr ? peer : "") : std::string();
            if (client.empty()) {
                client = connected != 0 ? "connected" : std::format("disconnected {}", age(disconnected));
            }
            if (attached != 0) {
                client += " (attached)";
            }
            std::cout << std::format("{:>4}  {:<16} {:<9} {:<8} {:<22} {:>8} {:>8}\n", id,
                                     account != nullptr ? account : "", state != nullptr ? state : "",
                                     desktop != nullptr ? desktop : "", client, age(age_seconds), age(idle));
            ++shown;
        }
        static_cast<void>(sd_bus_message_exit_container(reply));
    }
    if (shown == 0) {
        std::cout << "farlandctl: no sessions\n";
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    sd_bus_flush_close_unref(bus);
    return result;
}

int terminate(std::uint32_t id, const std::string& user)
{
    sd_bus* bus = open_daemon_bus();
    if (bus == nullptr) {
        return 1;
    }
    const pid_t agent = start_polkit_agent();
    sd_bus_set_method_call_timeout(bus, std::uint64_t{300} * 1'000'000);
    sd_bus_error error = {};
    sd_bus_message* reply = nullptr;
    const int rc = sd_bus_call_method(bus, "org.farland.Farland1", "/org/farland/Farland1", "org.farland.Farland1",
                                      "TerminateSession", &error, &reply, "us", id, user.c_str());
    stop_polkit_agent(agent);
    int result = 0;
    if (rc < 0) {
        std::cerr << "farlandctl: cannot end the session: "
                  << (error.message != nullptr ? error.message : std::strerror(-rc)) << "\n";
        result = 1;
    } else {
        std::uint32_t ended = 0;
        static_cast<void>(sd_bus_message_read(reply, "u", &ended));
        if (ended == 0) {
            std::cerr << "farlandctl: no such session\n";
            result = 1;
        } else {
            std::cout << std::format("farlandctl: ending {} session{}\n", ended, ended == 1 ? "" : "s");
        }
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    sd_bus_flush_close_unref(bus);
    return result;
}

#else

int sessions()
{
    std::cerr << "farlandctl: this build cannot talk to farlandd (no libsystemd)\n";
    return 1;
}

int terminate(std::uint32_t /*id*/, const std::string& /*user*/)
{
    std::cerr << "farlandctl: this build cannot talk to farlandd (no libsystemd)\n";
    return 1;
}

#endif

int remove(const std::filesystem::path& file, const std::string& user, const std::string& domain)
{
    auto store = CredentialStore::load(file);
    if (!store) {
        std::cerr << "farlandctl: cannot load " << file.string() << ": " << store.error().message() << "\n";
        return 1;
    }
    if (!store->remove(user, domain)) {
        std::cerr << "farlandctl: no such user\n";
        return 1;
    }
    if (auto saved = store->save(file); !saved) {
        std::cerr << "farlandctl: " << saved.error().message() << "\n";
        return 1;
    }
    return 0;
}

int users(const std::filesystem::path& file)
{
    const auto store = CredentialStore::load(file);
    if (!store) {
        std::cerr << "farlandctl: cannot load " << file.string() << ": " << store.error().message() << "\n";
        return 1;
    }
    for (const auto& entry : store->entries()) {
        std::cout << (entry.domain.empty() ? "*" : entry.domain) << "\\" << entry.user;
        if (!entry.local_account.empty()) {
            std::cout << " -> " << entry.local_account;
        }
        std::cout << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::span args(argv, static_cast<std::size_t>(argc));
    std::filesystem::path file = default_store();
    bool file_given = false;
    std::vector<std::string> positional;
    std::string domain;
    std::string local_account;
    std::string user_filter;
    bool from_stdin = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--file" && i + 1 < args.size()) {
            file = args[++i];
            file_given = true;
        } else if (arg == "--domain" && i + 1 < args.size()) {
            domain = args[++i];
        } else if (arg == "--local-account" && i + 1 < args.size()) {
            local_account = args[++i];
            if (!CredentialStore::valid_local_account(local_account)) {
                std::cerr << "farlandctl: invalid local account name\n";
                return 2;
            }
        } else if (arg == "--stdin") {
            from_stdin = true;
        } else if (arg == "--user" && i + 1 < args.size()) {
            user_filter = args[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg.starts_with("--")) {
            std::cerr << "farlandctl: unknown option " << arg << "\n";
            usage();
            return 2;
        } else {
            positional.emplace_back(arg);
        }
    }
    if (positional.empty()) {
        usage();
        return 2;
    }
    const std::string& command = positional[0];
    if (command == "passwd" && positional.size() == 1 && !file_given && local_account.empty()) {
        if (!CredentialStore::valid_name(domain, true)) {
            std::cerr << "farlandctl: names may not contain ':' or control characters\n";
            return 2;
        }
        return enrol_self(domain, from_stdin);
    }
    if (command == "users" && positional.size() == 1) {
        return users(file);
    }
    if (command == "sessions" && positional.size() == 1) {
        return sessions();
    }
    if (command == "terminate") {
        if (positional.size() == 2 && user_filter.empty()) {
            const std::string& id = positional[1];
            if (id.find_first_not_of("0123456789") != std::string::npos || id.empty() || id.size() > 9) {
                std::cerr << "farlandctl: a session id is a number; use --user to name an account\n";
                return 2;
            }
            return terminate(static_cast<std::uint32_t>(std::stoul(id)), "");
        }
        if (positional.size() == 1 && !user_filter.empty()) {
            return terminate(0, user_filter);
        }
        std::cerr << "farlandctl: give a session id or --user USER\n";
        return 2;
    }
    if ((command == "passwd" || command == "remove") && positional.size() == 2) {
        const std::string& user = positional[1];
        if (!CredentialStore::valid_name(user, false) || !CredentialStore::valid_name(domain, true)) {
            std::cerr << "farlandctl: names may not contain ':' or control characters\n";
            return 2;
        }
        if (command == "passwd") {
            return passwd(file, user, domain, local_account, from_stdin);
        }
        if (local_account.empty()) {
            return remove(file, user, domain);
        }
    }
    usage();
    return 2;
}
