// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/cliprdr.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Files on the clipboard, both ways ([MS-RDPECLIP] 1.3.1.1.4, 1.3.1.1.5):
/// files the client copied are staged in a private directory for the
/// desktop to paste from, and local files the desktop copied are listed and
/// read for the client. Nothing the client sends can name a path outside the
/// staging directory, and nothing outside what the desktop named is served
/// (docs/PLAN.md §6).
namespace farland::server::clipboard_files {

/// Splits a CLIPRDR_FILEDESCRIPTOR name ("dir\file.txt") into components.
/// Refuses anything that could leave the directory it is staged in or is no
/// portable name: empty components, "." and "..", colons (drive letters,
/// streams), slashes, control characters, components over 255 bytes and more
/// than 32 levels.
[[nodiscard]] Result<std::vector<std::string>> split_client_name(std::string_view name);

/// $XDG_RUNTIME_DIR/farland; nullopt without XDG_RUNTIME_DIR.
[[nodiscard]] std::optional<std::filesystem::path> default_staging_root();

/// A private directory (mode 0700) for the files of one paste, below a
/// staging root that must be a directory of this user and not a symlink.
/// The directory goes away with the object. Creating one removes those that
/// exited processes left behind (they carry the process ID in their name).
class StagingDirectory {
public:
    [[nodiscard]] static Result<StagingDirectory> create(const std::filesystem::path& root);

    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    StagingDirectory(StagingDirectory&& other) noexcept;
    StagingDirectory& operator=(StagingDirectory&& other) noexcept;
    ~StagingDirectory();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    /// Creates a directory (and missing parents) below this one.
    [[nodiscard]] Result<void> make_directory(std::span<const std::string> components);
    /// Creates a new file (mode 0600) whose parents exist; it must not exist
    /// yet. Following writes go there.
    [[nodiscard]] Result<void> create_file(std::span<const std::string> components);
    [[nodiscard]] Result<void> append(std::span<const std::byte> data) const;
    void close_file() noexcept;

private:
    StagingDirectory() = default;
    [[nodiscard]] Result<int> open_parent(std::span<const std::string> components, bool create) const;
    void release() noexcept;

    std::filesystem::path path_;
    int dir_fd_ = -1;
    int file_fd_ = -1;
};

/// The local paths of a text/uri-list or x-special/gnome-copied-files (whose
/// first line is "copy" or "cut"): file:// URIs of this host, decoded.
/// Comments, other schemes and hosts, and relative or NUL-carrying paths are
/// dropped.
[[nodiscard]] std::vector<std::filesystem::path> parse_uri_list(std::string_view text);
/// A file:// URI for an absolute path, percent-encoded as RFC 8089 wants.
[[nodiscard]] std::string file_uri(const std::filesystem::path& path);

struct LocalFileLimits {
    std::size_t max_entries = 4096;
    std::size_t max_depth = 32;
};

/// Local files the desktop copied, flattened into a CLIPRDR_FILELIST:
/// directories are walked (sorted by name), each entry named relative to
/// its top-level item. Only readable regular files and directories are
/// listed. Symlinks inside directories are skipped, never followed; a
/// top-level symlink the user picked is resolved. Names a Windows client
/// cannot take (backslashes, longer than 259 UTF-16 units) are skipped.
class LocalFileList {
public:
    [[nodiscard]] static Result<LocalFileList> from_paths(std::span<const std::filesystem::path> paths,
                                                          const LocalFileLimits& limits = {});

    [[nodiscard]] const std::vector<channels::cliprdr::FileDescriptor>& descriptors() const noexcept
    {
        return descriptors_;
    }
    /// The current size of the file at `index`.
    [[nodiscard]] Result<std::uint64_t> size(std::int32_t index) const;
    /// Up to `length` bytes at `offset` (fewer at the end of the file). The
    /// file must still be the one that was listed -- same device, inode and
    /// inode change time -- and is opened without following a symlink.
    [[nodiscard]] Result<std::vector<std::byte>> read(std::int32_t index, std::uint64_t offset,
                                                      std::size_t length) const;

private:
    struct Entry {
        std::filesystem::path path;
        bool directory = false;
        std::uint64_t device = 0;
        std::uint64_t inode = 0;
        /// st_ctim at listing time, nanoseconds. The device and inode alone
        /// do not identify a file: delete it and write another in its
        /// place and the filesystem hands the new one the very same inode
        /// number (ext4 does, reliably). The change time is what separates
        /// them -- the replacement was created after we looked.
        std::uint64_t change_time_ns = 0;
    };
    [[nodiscard]] Result<const Entry*> file(std::int32_t index) const;

    std::vector<Entry> entries_;
    std::vector<channels::cliprdr::FileDescriptor> descriptors_;
};

}  // namespace farland::server::clipboard_files
