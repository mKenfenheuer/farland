// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farlandctl: administration for farland-server. M2 manages the NLA user
// store; the file holds NT hashes, never passwords.

#include <farland/auth/credential_store.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/base/text.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

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
                 "  passwd USER [--domain DOMAIN] [--local-account ACCOUNT] [--stdin]\n"
                 "                                            add a user or change the password\n"
                 "  remove USER [--domain DOMAIN]             remove a user\n"
                 "  users                                     list users\n"
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
    std::vector<std::string> positional;
    std::string domain;
    std::string local_account;
    bool from_stdin = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--file" && i + 1 < args.size()) {
            file = args[++i];
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
    if (command == "users" && positional.size() == 1) {
        return users(file);
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
