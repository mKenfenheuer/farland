// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "sandbox.hpp"

#include <farland/base/log.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <cstddef>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

// Sanitizer runtimes probe whether memory is readable by writing it into a
// pipe (UBSan's vptr check, for one); in those builds the filter allows it.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer) ||          \
    __has_feature(undefined_behavior_sanitizer)
#define FARLAND_SANITIZED 1
#endif
#endif
#if !defined(FARLAND_SANITIZED) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#define FARLAND_SANITIZED 1
#endif

namespace farland::app {

namespace {

constexpr std::string_view log_component = "app.sandbox";

bool drop_root()
{
    if (::geteuid() != 0) {
        return true;
    }
    const passwd* nobody = ::getpwnam("nobody");
    if (nobody == nullptr) {
        log::error(log_component, "running as root, but there is no user 'nobody' to switch to");
        return false;
    }
    const uid_t uid = nobody->pw_uid;
    const gid_t gid = nobody->pw_gid;
    if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0) {
        log::error(log_component, "cannot switch to user 'nobody': {}", std::strerror(errno));
        return false;
    }
    if (::setuid(0) == 0) {
        log::error(log_component, "root privileges could be regained after switching users");
        return false;
    }
    return true;
}

#ifdef __linux__

// Landlock ([Documentation/userspace-api/landlock.rst]). Declared here so the
// build does not depend on the kernel headers' Landlock version; the system
// call numbers are the same on every architecture.
constexpr long sys_landlock_create_ruleset = 444;
constexpr long sys_landlock_restrict_self = 446;
constexpr std::uint32_t landlock_create_ruleset_version = 1U << 0;

struct LandlockRulesetAttr {
    std::uint64_t handled_access_fs = 0;
    std::uint64_t handled_access_net = 0;  // ABI 4
    std::uint64_t scoped = 0;              // ABI 6
};

void apply_landlock()
{
    const long abi = ::syscall(sys_landlock_create_ruleset, nullptr, 0, landlock_create_ruleset_version);
    if (abi < 1) {
        log::info(log_component, "Landlock is not available ({}); relying on seccomp", std::strerror(errno));
        return;
    }
    LandlockRulesetAttr attr;
    std::size_t size = sizeof(std::uint64_t);
    attr.handled_access_fs = (1ULL << 13) - 1;  // ABI 1: execute .. make_sym
    if (abi >= 2) {
        attr.handled_access_fs |= 1ULL << 13;  // refer
    }
    if (abi >= 3) {
        attr.handled_access_fs |= 1ULL << 14;  // truncate
    }
    if (abi >= 4) {
        attr.handled_access_net = (1ULL << 0) | (1ULL << 1);  // bind_tcp, connect_tcp
        size = 2 * sizeof(std::uint64_t);
    }
    if (abi >= 5) {
        attr.handled_access_fs |= 1ULL << 15;  // ioctl_dev
    }
    if (abi >= 6) {
        attr.scoped = (1ULL << 0) | (1ULL << 1);  // abstract unix sockets, signals
        size = sizeof(LandlockRulesetAttr);
    }
    const long ruleset = ::syscall(sys_landlock_create_ruleset, &attr, size, 0U);
    if (ruleset < 0) {
        log::warn(log_component, "cannot create a Landlock ruleset: {}", std::strerror(errno));
        return;
    }
    // No rules: nothing in the file system is accessible any more.
    if (::syscall(sys_landlock_restrict_self, ruleset, 0U) != 0) {
        log::warn(log_component, "cannot enter the Landlock domain: {}", std::strerror(errno));
    } else {
        log::debug(log_component, "Landlock ABI {} applied", abi);
    }
    ::close(static_cast<int>(ruleset));
}

#if defined(__x86_64__)
constexpr std::uint32_t audit_arch = AUDIT_ARCH_X86_64;
#define FARLAND_HAVE_SECCOMP 1
#elif defined(__aarch64__)
constexpr std::uint32_t audit_arch = AUDIT_ARCH_AARCH64;
#define FARLAND_HAVE_SECCOMP 1
#endif

#ifdef FARLAND_HAVE_SECCOMP

sock_filter statement(std::uint16_t code, std::uint32_t k)
{
    return sock_filter{code, 0, 0, k};
}

sock_filter jump(std::uint16_t code, std::uint32_t k, std::uint8_t if_true, std::uint8_t if_false)
{
    return sock_filter{code, if_true, if_false, k};
}

std::vector<long> allowed_syscalls()
{
    std::vector<long> allowed{// Socket I/O on the inherited descriptors.
                              SYS_read, SYS_write, SYS_readv, SYS_writev, SYS_recvfrom, SYS_sendto, SYS_recvmsg,
                              SYS_sendmsg, SYS_ppoll, SYS_close, SYS_shutdown, SYS_fcntl, SYS_lseek,
                              // Memory.
                              SYS_brk, SYS_mmap, SYS_munmap, SYS_mremap, SYS_madvise, SYS_mprotect,
                              // Time, randomness, threads' building blocks the C and C++ runtimes use.
                              SYS_clock_gettime, SYS_clock_nanosleep, SYS_nanosleep, SYS_getrandom, SYS_futex,
                              SYS_sched_yield, SYS_getpid, SYS_gettid, SYS_getuid, SYS_geteuid, SYS_getgid, SYS_getegid,
                              // Signals, exit and abort().
                              SYS_rt_sigreturn, SYS_rt_sigprocmask, SYS_rt_sigaction, SYS_restart_syscall, SYS_tgkill,
                              SYS_exit, SYS_exit_group,
                              // fstat() from stdio.
                              SYS_fstat, SYS_newfstatat};
#ifdef SYS_poll
    allowed.push_back(SYS_poll);
#endif
#ifdef SYS_statx
    allowed.push_back(SYS_statx);
#endif
#ifdef SYS_rseq
    allowed.push_back(SYS_rseq);
#endif
#ifdef FARLAND_SANITIZED
    allowed.push_back(SYS_pipe2);
#ifdef SYS_pipe
    allowed.push_back(SYS_pipe);
#endif
#endif
    return allowed;
}

void apply_seccomp()
{
    std::vector<sock_filter> filter;
    filter.push_back(statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    filter.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, audit_arch, 1, 0));
    filter.push_back(statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    filter.push_back(statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
#ifdef __x86_64__
    constexpr std::uint32_t x32_syscall_bit = 0x40000000;
    filter.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, x32_syscall_bit, 0, 1));
    filter.push_back(statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#endif
    for (const long nr : allowed_syscalls()) {
        filter.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(nr), 0, 1));
        filter.push_back(statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    // Everything else fails with EPERM rather than killing the process, so a
    // library that probes for a feature degrades instead of crashing.
    filter.push_back(statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));

    const sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        log::warn(log_component, "cannot install the seccomp filter: {}", std::strerror(errno));
    } else {
        log::debug(log_component, "seccomp filter installed ({} system calls allowed)", allowed_syscalls().size());
    }
}

#else

void apply_seccomp()
{
    log::info(log_component, "no seccomp filter for this architecture");
}

#endif  // FARLAND_HAVE_SECCOMP

#endif  // __linux__

}  // namespace

bool enter_network_sandbox()
{
    if (!drop_root()) {
        return false;
    }
#ifdef __linux__
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        log::warn(log_component, "cannot set no_new_privs: {}", std::strerror(errno));
        return true;  // Landlock and seccomp both need it
    }
    apply_landlock();
    apply_seccomp();
#else
    log::debug(log_component, "no sandbox on this platform beyond the separate process");
#endif
    return true;
}

}  // namespace farland::app
