// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/text.hpp>
#include <farland/server/clipboard_files.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace farland::server::clipboard_files {

namespace {

namespace cliprdr = channels::cliprdr;
constexpr std::string_view log_component = "server.clipboard";
constexpr std::size_t max_component_bytes = 255;
constexpr std::size_t max_levels = 32;
/// UTF-16 code units of CLIPRDR_FILEDESCRIPTOR fileName, without the terminator.
constexpr std::size_t max_descriptor_name = 259;
/// Seconds from 1601-01-01 (FILETIME) to 1970-01-01.
constexpr std::uint64_t filetime_epoch_offset = 11644473600ULL;
constexpr const char* directory_prefix = "clipboard-";

/// st_ctim as one number. A file is created with a change time, and nothing
/// a writer does moves that time backwards, so it tells a replacement from
/// the original even where the inode number comes round again -- which it
/// does: delete a file and write another in its place and ext4 hands the
/// new one the very same inode.
std::uint64_t change_time_ns(const struct stat& info)
{
    return (static_cast<std::uint64_t>(info.st_ctim.tv_sec) * 1000000000ULL) +
           static_cast<std::uint64_t>(info.st_ctim.tv_nsec);
}

void close_fd(int& fd) noexcept
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// open(2) and openat(2) are variadic for the mode argument.
int open_at(int dir, const std::string& name, int flags, mode_t mode = 0)
{
    return ::openat(dir, name.c_str(), flags | O_CLOEXEC, mode);  // NOLINT(cppcoreguidelines-pro-type-vararg)
}

Result<void> io_error(std::string_view what)
{
    return fail(Errc::io, what);
}

/// Removes staging directories of processes that are gone.
void remove_stale(const std::filesystem::path& root)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(directory_prefix)) {
            continue;
        }
        const std::string_view rest = std::string_view(name).substr(std::string_view(directory_prefix).size());
        const auto dash = rest.find('-');
        long pid = 0;
        for (const char c : rest.substr(0, dash)) {
            if (c < '0' || c > '9' || pid > 100'000'000) {
                pid = 0;
                break;
            }
            pid = (pid * 10) + (c - '0');
        }
        if (pid <= 0 || pid == ::getpid() || ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) {
            continue;  // ours, or a process that still runs (or that we may not signal)
        }
        std::filesystem::remove_all(entry.path(), ec);
        log::debug(log_component, "removed {}, left behind by process {}", entry.path().string(), pid);
    }
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::optional<std::string> percent_decode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size()) {
            return std::nullopt;
        }
        const int high = hex_value(text[i + 1]);
        const int low = hex_value(text[i + 2]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

std::uint64_t to_filetime(std::int64_t unix_seconds)
{
    if (unix_seconds < -static_cast<std::int64_t>(filetime_epoch_offset)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(unix_seconds + static_cast<std::int64_t>(filetime_epoch_offset))) *
           10'000'000ULL;
}

}  // namespace

// --- client names

Result<std::vector<std::string>> split_client_name(std::string_view name)
{
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = std::min(name.find('\\', start), name.size());
        const std::string_view component = name.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") {
            return fail(Errc::invalid_value, "file name with an empty, . or .. component");
        }
        if (component.size() > max_component_bytes) {
            return fail(Errc::limit_exceeded, "file name component longer than 255 bytes");
        }
        for (const char c : component) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F || c == '/' || c == ':') {
                return fail(Errc::invalid_value, "file name with a control character, slash or colon");
            }
        }
        components.emplace_back(component);
        if (components.size() > max_levels) {
            return fail(Errc::limit_exceeded, "file name nested more than 32 levels deep");
        }
        start = end + 1;
    }
    return components;
}

std::optional<std::filesystem::path> default_staging_root()
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");  // NOLINT(concurrency-mt-unsafe): read-only use
    if (runtime == nullptr || *runtime != '/') {
        return std::nullopt;
    }
    return std::filesystem::path(runtime) / "farland";
}

// --- StagingDirectory

Result<StagingDirectory> StagingDirectory::create(const std::filesystem::path& root)
{
    if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST) {
        return fail(Errc::io, "cannot create the clipboard staging root");
    }
    struct stat info{};
    if (::lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid() ||
        (info.st_mode & 077) != 0) {
        return fail(Errc::io, "the clipboard staging root is not a private directory of this user");
    }
    static std::atomic<bool> cleaned{false};
    if (!cleaned.exchange(true)) {
        remove_stale(root);
    }
    static std::atomic<std::uint32_t> counter{0};
    StagingDirectory staging;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto path = root / std::format("{}{}-{}", directory_prefix, ::getpid(), ++counter);
        if (::mkdir(path.c_str(), 0700) == 0) {
            staging.path_ = std::move(path);
            break;
        }
        if (errno != EEXIST) {
            return fail(Errc::io, "cannot create a clipboard staging directory");
        }
    }
    if (staging.path_.empty()) {
        return fail(Errc::io, "cannot create a clipboard staging directory");
    }
    staging.dir_fd_ = ::open(staging.path_.c_str(),  // NOLINT(cppcoreguidelines-pro-type-vararg): open(2)
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (staging.dir_fd_ < 0) {
        return fail(Errc::io, "cannot open the clipboard staging directory");
    }
    return staging;
}

StagingDirectory::StagingDirectory(StagingDirectory&& other) noexcept
    : path_(std::move(other.path_)), dir_fd_(std::exchange(other.dir_fd_, -1)),
      file_fd_(std::exchange(other.file_fd_, -1))
{
    other.path_.clear();
}

StagingDirectory& StagingDirectory::operator=(StagingDirectory&& other) noexcept
{
    if (this != &other) {
        release();
        path_ = std::move(other.path_);
        other.path_.clear();
        dir_fd_ = std::exchange(other.dir_fd_, -1);
        file_fd_ = std::exchange(other.file_fd_, -1);
    }
    return *this;
}

StagingDirectory::~StagingDirectory()
{
    release();
}

void StagingDirectory::release() noexcept
{
    close_fd(file_fd_);
    close_fd(dir_fd_);
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        path_.clear();
    }
}

/// Opens the directory that holds the last component, walking (and with
/// `create`, making) the ones before it without following symlinks. The
/// caller closes the descriptor.
Result<int> StagingDirectory::open_parent(std::span<const std::string> components, bool create) const
{
    if (components.empty() || dir_fd_ < 0) {
        return fail(Errc::invalid_value, "empty staged path");
    }
    int current = ::dup(dir_fd_);
    if (current < 0) {
        return fail(Errc::io, "cannot duplicate the staging directory descriptor");
    }
    for (const auto& component : components.first(components.size() - 1)) {
        if (create && ::mkdirat(current, component.c_str(), 0700) != 0 && errno != EEXIST) {
            close_fd(current);
            return fail(Errc::io, "cannot create a staged directory");
        }
        const int next = open_at(current, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close_fd(current);
        if (next < 0) {
            return fail(Errc::io, "a staged path is not a directory");
        }
        current = next;
    }
    return current;
}

Result<void> StagingDirectory::make_directory(std::span<const std::string> components)
{
    FARLAND_TRY(int parent, open_parent(components, true));
    const bool made = ::mkdirat(parent, components.back().c_str(), 0700) == 0 || errno == EEXIST;
    const int check = open_at(parent, components.back(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    close_fd(parent);
    const bool ok = made && check >= 0;
    if (check >= 0) {
        ::close(check);
    }
    return ok ? Result<void>{} : io_error("cannot create a staged directory");
}

Result<void> StagingDirectory::create_file(std::span<const std::string> components)
{
    close_fd(file_fd_);
    FARLAND_TRY(int parent, open_parent(components, true));
    file_fd_ = open_at(parent, components.back(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file_fd_ < 0) {
        const bool exists = errno == EEXIST;
        close_fd(parent);
        return fail(exists ? Errc::invalid_value : Errc::io, "cannot create a staged file");
    }
    close_fd(parent);
    return {};
}

Result<void> StagingDirectory::append(std::span<const std::byte> data) const
{
    if (file_fd_ < 0) {
        return fail(Errc::invalid_value, "no staged file is open");
    }
    while (!data.empty()) {
        const auto written = ::write(file_fd_, data.data(), data.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return fail(Errc::io, "cannot write a staged file");
        }
        data = data.subspan(static_cast<std::size_t>(written));
    }
    return {};
}

void StagingDirectory::close_file() noexcept
{
    close_fd(file_fd_);
}

// --- URIs

std::vector<std::filesystem::path> parse_uri_list(std::string_view text)
{
    std::vector<std::filesystem::path> paths;
    std::size_t start = 0;
    bool first = true;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (std::exchange(first, false) && (line == "copy" || line == "cut")) {
            continue;  // x-special/gnome-copied-files
        }
        if (line.empty() || line.starts_with('#') || !line.starts_with("file://")) {
            continue;
        }
        line.remove_prefix(7);
        if (line.starts_with("localhost/")) {
            line.remove_prefix(9);
        }
        if (!line.starts_with('/')) {
            continue;  // another host
        }
        const auto decoded = percent_decode(line);
        if (decoded) {
            paths.emplace_back(*decoded);
        }
    }
    return paths;
}

std::string file_uri(const std::filesystem::path& path)
{
    std::string uri = "file://";
    for (const char c : path.string()) {
        const auto u = static_cast<unsigned char>(c);
        const bool plain = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '/' ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            uri.push_back(c);
        } else {
            uri += std::format("%{:02X}", u);
        }
    }
    return uri;
}

// --- LocalFileList

namespace {

struct Walker {
    LocalFileLimits limits;
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> names;
    std::vector<struct stat> infos;

    /// Adds `path` under `name`, and a directory's contents below it.
    Result<void> add(const std::filesystem::path& path, const std::string& name, const struct stat& info,
                     std::size_t depth)
    {
        if (paths.size() >= limits.max_entries) {
            return fail(Errc::limit_exceeded, "too many files on the clipboard");
        }
        if (path.filename().string().find('\\') != std::string::npos ||
            utf8_to_utf16le(name).size() / 2 > max_descriptor_name) {
            log::info(log_component, "not offering {}: its name does not fit a Windows file list", path.string());
            return {};
        }
        if (::access(path.c_str(), R_OK) != 0) {
            log::info(log_component, "not offering {}: not readable", path.string());
            return {};
        }
        paths.push_back(path);
        names.push_back(name);
        infos.push_back(info);
        if (!S_ISDIR(info.st_mode)) {
            return {};
        }
        if (depth >= limits.max_depth) {
            return fail(Errc::limit_exceeded, "clipboard directory nested too deeply");
        }
        std::error_code ec;
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            children.push_back(entry.path());
            if (children.size() > limits.max_entries) {
                return fail(Errc::limit_exceeded, "too many files on the clipboard");
            }
        }
        std::ranges::sort(children);
        for (const auto& child : children) {
            struct stat child_info{};
            if (::lstat(child.c_str(), &child_info) != 0 ||
                (!S_ISREG(child_info.st_mode) && !S_ISDIR(child_info.st_mode))) {
                continue;  // symlinks, sockets, devices: never followed or offered
            }
            FARLAND_TRY_VOID(add(child, name + "\\" + child.filename().string(), child_info, depth + 1));
        }
        return {};
    }
};

}  // namespace

Result<LocalFileList> LocalFileList::from_paths(std::span<const std::filesystem::path> paths,
                                                const LocalFileLimits& limits)
{
    Walker walker{limits, {}, {}, {}};
    for (const auto& path : paths) {
        struct stat info{};
        // The user picked this item: a symlink is resolved here, and only here.
        if (!path.is_absolute() || ::stat(path.c_str(), &info) != 0 ||
            (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
            log::info(log_component, "not offering {}: not a regular file or directory", path.string());
            continue;
        }
        std::error_code ec;
        const auto resolved = std::filesystem::canonical(path, ec);
        if (ec || resolved.filename().empty()) {
            continue;
        }
        FARLAND_TRY_VOID(walker.add(resolved, path.filename().string(), info, 0));
    }
    LocalFileList list;
    for (std::size_t i = 0; i < walker.paths.size(); ++i) {
        const auto& info = walker.infos[i];
        const bool directory = S_ISDIR(info.st_mode);
        list.entries_.push_back(Entry{walker.paths[i], directory, static_cast<std::uint64_t>(info.st_dev),
                                      static_cast<std::uint64_t>(info.st_ino), change_time_ns(info)});
        cliprdr::FileDescriptor descriptor;
        descriptor.flags = cliprdr::fd_flag::attributes | cliprdr::fd_flag::write_time |
                           cliprdr::fd_flag::show_progress_ui | (directory ? 0 : cliprdr::fd_flag::file_size);
        descriptor.attributes = directory ? cliprdr::file_attribute::directory : cliprdr::file_attribute::archive;
        if ((info.st_mode & S_IWUSR) == 0) {
            descriptor.attributes |= cliprdr::file_attribute::readonly;
        }
        descriptor.last_write_time = to_filetime(static_cast<std::int64_t>(info.st_mtime));
        descriptor.size = directory ? 0 : static_cast<std::uint64_t>(info.st_size);
        descriptor.name = walker.names[i];
        list.descriptors_.push_back(std::move(descriptor));
    }
    return list;
}

Result<const LocalFileList::Entry*> LocalFileList::file(std::int32_t index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= entries_.size() ||
        entries_[static_cast<std::size_t>(index)].directory) {
        return fail(Errc::invalid_value, "no such file on the clipboard");
    }
    return &entries_[static_cast<std::size_t>(index)];
}

namespace {

/// Opens a listed file again, checking that it still is that file.
Result<int> open_listed(const std::filesystem::path& path, std::uint64_t device, std::uint64_t inode,
                        std::uint64_t change_time)
{
    const int fd =
        ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg): open(2)
    if (fd < 0) {
        return fail(Errc::io, "cannot open a file on the clipboard");
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::uint64_t>(info.st_dev) != device ||
        static_cast<std::uint64_t>(info.st_ino) != inode || change_time_ns(info) != change_time) {
        ::close(fd);
        return fail(Errc::io, "a file on the clipboard was replaced");
    }
    return fd;
}

}  // namespace

Result<std::uint64_t> LocalFileList::size(std::int32_t index) const
{
    FARLAND_TRY(const auto* entry, file(index));
    FARLAND_TRY(int fd, open_listed(entry->path, entry->device, entry->inode, entry->change_time_ns));
    struct stat info{};
    const bool ok = ::fstat(fd, &info) == 0;
    close_fd(fd);
    if (!ok) {
        return fail(Errc::io, "cannot stat a file on the clipboard");
    }
    return static_cast<std::uint64_t>(info.st_size);
}

Result<std::vector<std::byte>> LocalFileList::read(std::int32_t index, std::uint64_t offset, std::size_t length) const
{
    FARLAND_TRY(const auto* entry, file(index));
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return fail(Errc::invalid_value, "file offset out of range");
    }
    FARLAND_TRY(int fd, open_listed(entry->path, entry->device, entry->inode, entry->change_time_ns));
    std::vector<std::byte> data(length);
    std::size_t done = 0;
    while (done < length) {
        const auto n =
            ::pread(fd, std::span(data).subspan(done).data(), length - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            close_fd(fd);
            return fail(Errc::io, "cannot read a file on the clipboard");
        }
        if (n == 0) {
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    close_fd(fd);
    data.resize(done);
    return data;
}

}  // namespace farland::server::clipboard_files
