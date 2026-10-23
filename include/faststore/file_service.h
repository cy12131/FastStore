#ifndef FASTSTORE_FILE_SERVICE_H
#define FASTSTORE_FILE_SERVICE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace faststore {

enum class FileError {
    kNone,
    kInvalidName,
    kNotFound,
    kAlreadyExists,
    kPermissionDenied,
    kNotRegularFile,
    kTooLarge,
    kInvalidState,
    kIoError
};

template <typename T>
class FileResult {
public:
    static FileResult success(T value) {
        FileResult result;
        result.value_.emplace(std::move(value));
        return result;
    }

    static FileResult failure(FileError error) {
        FileResult result;
        result.error_ = error;
        return result;
    }

    [[nodiscard]] bool ok() const noexcept {
        return error_ == FileError::kNone && value_.has_value();
    }

    [[nodiscard]] FileError error() const noexcept {
        return error_;
    }

    T& value() {
        return value_.value();
    }

    const T& value() const {
        return value_.value();
    }

    T takeValue() {
        return std::move(value_.value());
    }

private:
    FileError error_{FileError::kNone};
    std::optional<T> value_;
};

template <>
class FileResult<void> {
public:
    static FileResult success() noexcept {
        return FileResult{FileError::kNone};
    }

    static FileResult failure(FileError error) noexcept {
        return FileResult{error};
    }

    [[nodiscard]] bool ok() const noexcept {
        return error_ == FileError::kNone;
    }

    [[nodiscard]] FileError error() const noexcept {
        return error_;
    }

private:
    explicit FileResult(FileError error) noexcept : error_(error) {}

    FileError error_;
};

struct FileMetadata {
    std::uint64_t size{0};
};

struct FileEntry {
    std::string name;
    std::uint64_t size{0};
};

class FileService;

class ReadFile {
public:
    ~ReadFile();

    ReadFile(const ReadFile&) = delete;
    ReadFile& operator=(const ReadFile&) = delete;
    ReadFile(ReadFile&& other) noexcept;
    ReadFile& operator=(ReadFile&& other) noexcept;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept;

private:
    friend class FileService;

    ReadFile(int fd, std::uint64_t size) noexcept;
    void closeFd() noexcept;

    int fd_{-1};
    std::uint64_t size_{0};
};

class UploadFile {
public:
    ~UploadFile();

    UploadFile(const UploadFile&) = delete;
    UploadFile& operator=(const UploadFile&) = delete;
    UploadFile(UploadFile&& other) noexcept;
    UploadFile& operator=(UploadFile&& other) noexcept;

    FileResult<void> writeAll(std::string_view data);
    FileResult<void> commit(std::string_view final_filename);
    FileResult<void> abort() noexcept;

    [[nodiscard]] std::string_view temporaryName() const noexcept;
    [[nodiscard]] bool committed() const noexcept;

private:
    friend class FileService;

    UploadFile(int directory_fd, int file_fd, std::string temporary_name);
    void cleanupNoexcept() noexcept;

    int directory_fd_{-1};
    int file_fd_{-1};
    std::string temporary_name_;
    bool active_{false};
    bool committed_{false};
};

class FileService {
public:
    ~FileService();

    FileService(const FileService&) = delete;
    FileService& operator=(const FileService&) = delete;
    FileService(FileService&& other) noexcept;
    FileService& operator=(FileService&& other) noexcept;

    FileResult<FileMetadata> metadata(std::string_view filename) const;
    FileResult<std::vector<FileEntry>> listFiles() const;
    FileResult<ReadFile> openRead(std::string_view filename) const;
    FileResult<void> remove(std::string_view filename) const;
    FileResult<UploadFile> createTemp() const;

private:
    friend FileResult<FileService> openFileService(
        const std::filesystem::path& data_path);

    explicit FileService(int directory_fd) noexcept;
    void closeDirectoryFd() noexcept;

    int directory_fd_{-1};
};

[[nodiscard]] FileError validateFilename(std::string_view filename) noexcept;

[[nodiscard]] FileResult<FileService> openFileService(
    const std::filesystem::path& data_path);

}  // namespace faststore

#endif  // FASTSTORE_FILE_SERVICE_H
