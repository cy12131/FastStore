#include "faststore/file_service.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

namespace faststore {
namespace {

constexpr std::string_view kUploadPrefix = ".faststore-upload-";
constexpr std::size_t kRandomByteCount = 16;
constexpr std::size_t kCreateAttempts = 128;

FileError mapErrno(int error) noexcept {
    switch (error) {
        case ENOENT:
            return FileError::kNotFound;
        case EEXIST:
            return FileError::kAlreadyExists;
        case EACCES:
        case EPERM:
        case EROFS:
            return FileError::kPermissionDenied;
        case ELOOP:
        case ENOTDIR:
        case EISDIR:
        case ENXIO:
            return FileError::kNotRegularFile;
        case EFBIG:
            return FileError::kTooLarge;
        case ENAMETOOLONG:
            return FileError::kInvalidName;
        default:
            return FileError::kIoError;
    }
}

FileError closeOwnedFd(int& fd) noexcept {
    if (fd < 0) {
        return FileError::kNone;
    }

    const int fd_to_close = fd;
    fd = -1;
    if (::close(fd_to_close) == -1) {
        return mapErrno(errno);
    }
    return FileError::kNone;
}

void closeOwnedFdNoexcept(int& fd, const char* description) noexcept {
    if (fd < 0) {
        return;
    }

    const int fd_to_close = fd;
    fd = -1;
    if (::close(fd_to_close) == -1) {
        const int error = errno;
        std::cerr << "Failed to close " << description << " fd="
                  << fd_to_close << ": " << std::strerror(error)
                  << std::endl;
    }
}

FileResult<FileMetadata> metadataFromStat(const struct stat& status) {
    if (!S_ISREG(status.st_mode)) {
        return FileResult<FileMetadata>::failure(
            FileError::kNotRegularFile);
    }
    if (status.st_size < 0) {
        return FileResult<FileMetadata>::failure(FileError::kIoError);
    }

    const auto size = static_cast<std::uint64_t>(status.st_size);
    return FileResult<FileMetadata>::success(FileMetadata{size});
}

bool fillRandomBytes(unsigned char* output, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result =
            ::getrandom(output + offset, size - offset, 0);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = EIO;
        }
        return false;
    }
    return true;
}

FileResult<std::string> makeTemporaryName() {
    std::array<unsigned char, kRandomByteCount> random_bytes{};
    if (!fillRandomBytes(random_bytes.data(), random_bytes.size())) {
        return FileResult<std::string>::failure(mapErrno(errno));
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string name{kUploadPrefix};
    name.reserve(kUploadPrefix.size() + random_bytes.size() * 2U);
    for (const unsigned char byte : random_bytes) {
        name.push_back(hex[byte >> 4U]);
        name.push_back(hex[byte & 0x0FU]);
    }
    return FileResult<std::string>::success(std::move(name));
}

int renameNoReplace(int directory_fd,
                    const char* old_name,
                    const char* new_name) noexcept {
    return static_cast<int>(::syscall(
        SYS_renameat2, directory_fd, old_name, directory_fd, new_name,
        RENAME_NOREPLACE));
}

}  // namespace

FileError validateFilename(std::string_view filename) noexcept {
    if (filename.empty() || filename.size() > 255U) {
        return FileError::kInvalidName;
    }
    if (filename == "." || filename == ".." ||
        filename.find("..") != std::string_view::npos) {
        return FileError::kInvalidName;
    }
    if (filename.find('/') != std::string_view::npos ||
        filename.find('\\') != std::string_view::npos ||
        filename.find('\0') != std::string_view::npos) {
        return FileError::kInvalidName;
    }
    if (filename.size() >= kUploadPrefix.size() &&
        filename.substr(0, kUploadPrefix.size()) == kUploadPrefix) {
        return FileError::kInvalidName;
    }
    return FileError::kNone;
}

ReadFile::ReadFile(int fd, std::uint64_t size) noexcept
    : fd_(fd), size_(size) {}

ReadFile::~ReadFile() {
    closeFd();
}

ReadFile::ReadFile(ReadFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      size_(std::exchange(other.size_, 0)) {}

ReadFile& ReadFile::operator=(ReadFile&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    closeFd();
    fd_ = std::exchange(other.fd_, -1);
    size_ = std::exchange(other.size_, 0);
    return *this;
}

int ReadFile::fd() const noexcept {
    return fd_;
}

std::uint64_t ReadFile::size() const noexcept {
    return size_;
}

void ReadFile::closeFd() noexcept {
    closeOwnedFdNoexcept(fd_, "read file");
}

UploadFile::UploadFile(int directory_fd,
                       int file_fd,
                       std::string temporary_name)
    : directory_fd_(directory_fd),
      file_fd_(file_fd),
      temporary_name_(std::move(temporary_name)),
      active_(true) {}

UploadFile::~UploadFile() {
    cleanupNoexcept();
}

UploadFile::UploadFile(UploadFile&& other) noexcept
    : directory_fd_(std::exchange(other.directory_fd_, -1)),
      file_fd_(std::exchange(other.file_fd_, -1)),
      temporary_name_(std::move(other.temporary_name_)),
      active_(std::exchange(other.active_, false)),
      committed_(std::exchange(other.committed_, false)) {}

UploadFile& UploadFile::operator=(UploadFile&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    cleanupNoexcept();
    directory_fd_ = std::exchange(other.directory_fd_, -1);
    file_fd_ = std::exchange(other.file_fd_, -1);
    temporary_name_ = std::move(other.temporary_name_);
    active_ = std::exchange(other.active_, false);
    committed_ = std::exchange(other.committed_, false);
    return *this;
}

FileResult<void> UploadFile::writeAll(std::string_view data) {
    if (!active_ || committed_ || file_fd_ < 0) {
        return FileResult<void>::failure(FileError::kInvalidState);
    }

    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written =
            ::write(file_fd_, data.data() + offset, data.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written == 0) {
            return FileResult<void>::failure(FileError::kIoError);
        }
        return FileResult<void>::failure(mapErrno(errno));
    }
    return FileResult<void>::success();
}

FileResult<void> UploadFile::commit(std::string_view final_filename) {
    const FileError validation = validateFilename(final_filename);
    if (validation != FileError::kNone) {
        return FileResult<void>::failure(validation);
    }
    if (!active_ || committed_ || directory_fd_ < 0 || file_fd_ < 0) {
        return FileResult<void>::failure(FileError::kInvalidState);
    }

    const FileError close_error = closeOwnedFd(file_fd_);
    if (close_error != FileError::kNone) {
        return FileResult<void>::failure(close_error);
    }

    const std::string final_name{final_filename};
    if (renameNoReplace(directory_fd_, temporary_name_.c_str(),
                        final_name.c_str()) == 0) {
        active_ = false;
        committed_ = true;
        temporary_name_.clear();
        closeOwnedFdNoexcept(directory_fd_, "upload directory");
        return FileResult<void>::success();
    }

    const int no_replace_error = errno;
    if (no_replace_error != EEXIST) {
        return FileResult<void>::failure(mapErrno(no_replace_error));
    }

    struct stat target_status {};
    if (::fstatat(directory_fd_, final_name.c_str(), &target_status,
                  AT_SYMLINK_NOFOLLOW) == -1) {
        return FileResult<void>::failure(mapErrno(errno));
    }
    if (!S_ISREG(target_status.st_mode)) {
        return FileResult<void>::failure(FileError::kNotRegularFile);
    }

    // Linux has no renameat2 flag meaning "replace newpath only if it still
    // names the regular file just inspected". Another process with write
    // access to the data directory can therefore replace this directory entry
    // between fstatat() and renameat(). renameat() never follows a destination
    // symlink, but it may replace that raced-in symlink or another non-regular
    // entry. Avoiding this remaining race requires exclusive directory
    // ownership or a more complex coordination/transaction scheme.
    if (::renameat(directory_fd_, temporary_name_.c_str(),
                   directory_fd_, final_name.c_str()) == -1) {
        return FileResult<void>::failure(mapErrno(errno));
    }

    active_ = false;
    committed_ = true;
    temporary_name_.clear();
    closeOwnedFdNoexcept(directory_fd_, "upload directory");
    return FileResult<void>::success();
}

FileResult<void> UploadFile::abort() noexcept {
    FileError result = closeOwnedFd(file_fd_);

    if (active_ && directory_fd_ >= 0 && !temporary_name_.empty()) {
        if (::unlinkat(directory_fd_, temporary_name_.c_str(), 0) == -1 &&
            errno != ENOENT) {
            if (result == FileError::kNone) {
                result = mapErrno(errno);
            }
            return FileResult<void>::failure(result);
        }
    }

    active_ = false;
    temporary_name_.clear();
    closeOwnedFdNoexcept(directory_fd_, "upload directory");
    if (result != FileError::kNone) {
        return FileResult<void>::failure(result);
    }
    return FileResult<void>::success();
}

std::string_view UploadFile::temporaryName() const noexcept {
    return temporary_name_;
}

bool UploadFile::committed() const noexcept {
    return committed_;
}

void UploadFile::cleanupNoexcept() noexcept {
    closeOwnedFdNoexcept(file_fd_, "temporary upload");

    if (active_ && directory_fd_ >= 0 && !temporary_name_.empty()) {
        if (::unlinkat(directory_fd_, temporary_name_.c_str(), 0) == -1 &&
            errno != ENOENT) {
            const int error = errno;
            std::cerr << "Failed to remove temporary upload "
                      << temporary_name_ << ": " << std::strerror(error)
                      << std::endl;
        }
    }

    active_ = false;
    temporary_name_.clear();
    closeOwnedFdNoexcept(directory_fd_, "upload directory");
}

FileService::FileService(int directory_fd) noexcept
    : directory_fd_(directory_fd) {}

FileService::~FileService() {
    closeDirectoryFd();
}

FileService::FileService(FileService&& other) noexcept
    : directory_fd_(std::exchange(other.directory_fd_, -1)) {}

FileService& FileService::operator=(FileService&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    closeDirectoryFd();
    directory_fd_ = std::exchange(other.directory_fd_, -1);
    return *this;
}

FileResult<FileMetadata> FileService::metadata(
    std::string_view filename) const {
    const FileError validation = validateFilename(filename);
    if (validation != FileError::kNone) {
        return FileResult<FileMetadata>::failure(validation);
    }

    const std::string name{filename};
    struct stat status {};
    if (::fstatat(directory_fd_, name.c_str(), &status,
                  AT_SYMLINK_NOFOLLOW) == -1) {
        return FileResult<FileMetadata>::failure(mapErrno(errno));
    }
    return metadataFromStat(status);
}

FileResult<std::vector<FileEntry>> FileService::listFiles() const {
    // openat(".") 创建独立 open file description；普通 dup 会共享目录
    // offset，多个 worker 并发 readdir 时会互相干扰。
    const int enumeration_fd = ::openat(
        directory_fd_, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (enumeration_fd == -1) {
        return FileResult<std::vector<FileEntry>>::failure(mapErrno(errno));
    }

    DIR* directory = ::fdopendir(enumeration_fd);
    if (directory == nullptr) {
        const int error = errno;
        int owned_fd = enumeration_fd;
        closeOwnedFdNoexcept(owned_fd, "listing directory");
        return FileResult<std::vector<FileEntry>>::failure(mapErrno(error));
    }

    std::vector<FileEntry> entries;
    errno = 0;
    while (dirent* entry = ::readdir(directory)) {
        const std::string_view name{entry->d_name};
        if (validateFilename(name) != FileError::kNone) {
            errno = 0;
            continue;
        }

        struct stat status {};
        if (::fstatat(enumeration_fd, entry->d_name, &status,
                      AT_SYMLINK_NOFOLLOW) == -1) {
            const int error = errno;
            ::closedir(directory);
            return FileResult<std::vector<FileEntry>>::failure(
                mapErrno(error));
        }
        if (S_ISREG(status.st_mode) && status.st_size >= 0) {
            entries.push_back(FileEntry{
                std::string{name},
                static_cast<std::uint64_t>(status.st_size)});
        }
        errno = 0;
    }

    const int read_error = errno;
    if (::closedir(directory) == -1 && read_error == 0) {
        return FileResult<std::vector<FileEntry>>::failure(mapErrno(errno));
    }
    if (read_error != 0) {
        return FileResult<std::vector<FileEntry>>::failure(
            mapErrno(read_error));
    }

    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& left, const FileEntry& right) {
                  return left.name < right.name;
              });
    return FileResult<std::vector<FileEntry>>::success(std::move(entries));
}

FileResult<ReadFile> FileService::openRead(
    std::string_view filename) const {
    const FileError validation = validateFilename(filename);
    if (validation != FileError::kNone) {
        return FileResult<ReadFile>::failure(validation);
    }

    const std::string name{filename};
    const int fd = ::openat(directory_fd_, name.c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd == -1) {
        return FileResult<ReadFile>::failure(mapErrno(errno));
    }

    struct stat status {};
    if (::fstat(fd, &status) == -1) {
        const FileError error = mapErrno(errno);
        int owned_fd = fd;
        closeOwnedFdNoexcept(owned_fd, "invalid read file");
        return FileResult<ReadFile>::failure(error);
    }

    const auto metadata_result = metadataFromStat(status);
    if (!metadata_result.ok()) {
        int owned_fd = fd;
        closeOwnedFdNoexcept(owned_fd, "non-regular read file");
        return FileResult<ReadFile>::failure(metadata_result.error());
    }

    return FileResult<ReadFile>::success(
        ReadFile{fd, metadata_result.value().size});
}

FileResult<void> FileService::remove(std::string_view filename) const {
    const FileError validation = validateFilename(filename);
    if (validation != FileError::kNone) {
        return FileResult<void>::failure(validation);
    }

    const std::string name{filename};
    struct stat status {};
    if (::fstatat(directory_fd_, name.c_str(), &status,
                  AT_SYMLINK_NOFOLLOW) == -1) {
        return FileResult<void>::failure(mapErrno(errno));
    }
    if (!S_ISREG(status.st_mode)) {
        return FileResult<void>::failure(FileError::kNotRegularFile);
    }

    if (::unlinkat(directory_fd_, name.c_str(), 0) == -1) {
        return FileResult<void>::failure(mapErrno(errno));
    }
    return FileResult<void>::success();
}

FileResult<UploadFile> FileService::createTemp() const {
    const int upload_directory_fd =
        ::fcntl(directory_fd_, F_DUPFD_CLOEXEC, 0);
    if (upload_directory_fd == -1) {
        return FileResult<UploadFile>::failure(mapErrno(errno));
    }

    for (std::size_t attempt = 0; attempt < kCreateAttempts; ++attempt) {
        auto name_result = makeTemporaryName();
        if (!name_result.ok()) {
            int owned_directory_fd = upload_directory_fd;
            closeOwnedFdNoexcept(owned_directory_fd, "upload directory");
            return FileResult<UploadFile>::failure(name_result.error());
        }

        std::string name = name_result.takeValue();
        const int file_fd = ::openat(
            upload_directory_fd, name.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file_fd >= 0) {
            return FileResult<UploadFile>::success(
                UploadFile{upload_directory_fd, file_fd, std::move(name)});
        }
        if (errno != EEXIST) {
            const FileError error = mapErrno(errno);
            int owned_directory_fd = upload_directory_fd;
            closeOwnedFdNoexcept(owned_directory_fd, "upload directory");
            return FileResult<UploadFile>::failure(error);
        }
    }

    int owned_directory_fd = upload_directory_fd;
    closeOwnedFdNoexcept(owned_directory_fd, "upload directory");
    return FileResult<UploadFile>::failure(FileError::kAlreadyExists);
}

void FileService::closeDirectoryFd() noexcept {
    closeOwnedFdNoexcept(directory_fd_, "data directory");
}

FileResult<FileService> openFileService(
    const std::filesystem::path& data_path) {
    const int directory_fd = ::open(
        data_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd == -1) {
        return FileResult<FileService>::failure(mapErrno(errno));
    }
    return FileResult<FileService>::success(FileService{directory_fd});
}

}  // namespace faststore
