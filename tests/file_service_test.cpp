#include "faststore/file_service.h"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using faststore::FileError;
using faststore::FileService;
using faststore::UploadFile;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

class TemporaryTree {
public:
    TemporaryTree() {
        std::string pattern = "/tmp/faststore-file-service-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');

        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }

        root_ = created;
        data_ = root_ / "data";
        if (!std::filesystem::create_directory(data_)) {
            throw std::runtime_error("unable to create test data directory");
        }
    }

    ~TemporaryTree() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]] const std::filesystem::path& data() const noexcept {
        return data_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path data_;
};

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) {
        throw std::runtime_error("unable to write test file");
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

FileService makeService(const std::filesystem::path& data_path) {
    auto result = faststore::openFileService(data_path);
    expect(result.ok(), "FileService initialization failed");
    return result.takeValue();
}

std::size_t countTemporaryFiles(const std::filesystem::path& data_path) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(data_path)) {
        const std::string filename = entry.path().filename().string();
        if (filename.rfind(".faststore-upload-", 0) == 0U) {
            ++count;
        }
    }
    return count;
}

void testFilenameValidation() {
    for (const std::string_view valid : {"a.txt", "hello-world", "123",
                                         "%2e%2e"}) {
        expect(faststore::validateFilename(valid) == FileError::kNone,
               "valid filename was rejected");
    }

    for (const std::string_view invalid : {"", ".", "..", "../a",
                                           "a/b", "a\\b", "a..b",
                                           ".faststore-upload-user"}) {
        expect(faststore::validateFilename(invalid) ==
                   FileError::kInvalidName,
               "invalid filename was accepted");
    }

    const std::string embedded_nul{"a\0b", 3};
    expect(faststore::validateFilename(embedded_nul) ==
               FileError::kInvalidName,
           "embedded NUL was accepted");
    expect(faststore::validateFilename(std::string(256, 'a')) ==
               FileError::kInvalidName,
           "oversized filename was accepted");
}

void testInitializationFailures() {
    TemporaryTree tree;

    const auto missing =
        faststore::openFileService(tree.root() / "missing");
    expect(!missing.ok() && missing.error() == FileError::kNotFound,
           "missing data root should fail with kNotFound");

    const auto regular_path = tree.root() / "regular";
    writeFile(regular_path, "file");
    const auto regular = faststore::openFileService(regular_path);
    expect(!regular.ok() && regular.error() == FileError::kNotRegularFile,
           "regular file data root should be rejected");

    const auto link_path = tree.root() / "data-link";
    expect(::symlink(tree.data().c_str(), link_path.c_str()) == 0,
           "unable to create root symlink");
    const auto link = faststore::openFileService(link_path);
    expect(!link.ok() && link.error() == FileError::kNotRegularFile,
           "symlink data root should be rejected");
}

void testMetadataReadAndRemove() {
    TemporaryTree tree;
    writeFile(tree.data() / "hello.txt", "hello");
    FileService service = makeService(tree.data());

    const auto metadata = service.metadata("hello.txt");
    expect(metadata.ok() && metadata.value().size == 5U,
           "metadata size should be five");

    const auto missing = service.metadata("missing.txt");
    expect(!missing.ok() && missing.error() == FileError::kNotFound,
           "missing metadata should return kNotFound");

    auto read_result = service.openRead("hello.txt");
    expect(read_result.ok(), "openRead failed");
    auto read_file = read_result.takeValue();
    expect(read_file.fd() >= 0 && read_file.size() == 5U,
           "ReadFile metadata is wrong");

    std::array<char, 5> buffer{};
    const ssize_t count = ::read(read_file.fd(), buffer.data(), buffer.size());
    expect(count == 5 && std::string(buffer.data(), buffer.size()) == "hello",
           "ReadFile returned wrong content");

    const auto removed = service.remove("hello.txt");
    expect(removed.ok(), "regular file removal failed");
    expect(!std::filesystem::exists(tree.data() / "hello.txt"),
           "removed file still exists");
}

void testSymlinkDirectoryAndFifoRejection() {
    TemporaryTree tree;
    const auto outside = tree.root() / "outside.txt";
    writeFile(outside, "outside");

    const auto link = tree.data() / "link";
    expect(::symlink(outside.c_str(), link.c_str()) == 0,
           "unable to create file symlink");
    expect(std::filesystem::create_directory(tree.data() / "subdir"),
           "unable to create subdirectory");
    expect(::mkfifo((tree.data() / "pipe").c_str(), 0600) == 0,
           "unable to create FIFO");

    FileService service = makeService(tree.data());
    for (const std::string_view name : {"link", "subdir", "pipe"}) {
        const auto metadata = service.metadata(name);
        expect(!metadata.ok() &&
                   metadata.error() == FileError::kNotRegularFile,
               "metadata accepted a non-regular file");

        const auto opened = service.openRead(name);
        expect(!opened.ok() &&
                   opened.error() == FileError::kNotRegularFile,
               "openRead accepted a non-regular file");

        const auto removed = service.remove(name);
        expect(!removed.ok() &&
                   removed.error() == FileError::kNotRegularFile,
               "remove accepted a non-regular file");
    }

    expect(readFile(outside) == "outside",
           "symlink target was modified");
    expect(std::filesystem::is_symlink(link),
           "symlink itself should not be deleted");
}

void testFileListing() {
    TemporaryTree empty_tree;
    FileService empty_service = makeService(empty_tree.data());
    const auto empty = empty_service.listFiles();
    expect(empty.ok() && empty.value().empty(),
           "empty directory should produce an empty listing");

    TemporaryTree tree;
    writeFile(tree.data() / "z-last.txt", "1234");
    writeFile(tree.data() / "a-first.txt", "x");
    writeFile(tree.data() / "quote\"and\ncontrol", "12");
    writeFile(tree.data() / ".faststore-upload-hidden", "partial");
    expect(std::filesystem::create_directory(tree.data() / "directory"),
           "unable to create listed directory fixture");
    expect(::symlink((tree.data() / "a-first.txt").c_str(),
                     (tree.data() / "symlink").c_str()) == 0,
           "unable to create listed symlink fixture");
    expect(::mkfifo((tree.data() / "fifo").c_str(), 0600) == 0,
           "unable to create listed FIFO fixture");

    FileService service = makeService(tree.data());
    const auto listed = service.listFiles();
    expect(listed.ok(), "file listing failed");
    const auto& entries = listed.value();
    expect(entries.size() == 3U,
           "listing included a non-regular or temporary entry");
    expect(entries[0].name == "a-first.txt" && entries[0].size == 1U,
           "first listed file or size is wrong");
    expect(entries[1].name == "quote\"and\ncontrol" &&
               entries[1].size == 2U,
           "escaped-name file or size is wrong");
    expect(entries[2].name == "z-last.txt" && entries[2].size == 4U,
           "listing is not sorted lexicographically");

    const auto listed_again = service.listFiles();
    expect(listed_again.ok() && listed_again.value().size() == entries.size(),
           "repeated listing did not restart at the first directory entry");

    const auto metadata = service.metadata("a-first.txt");
    expect(metadata.ok() && metadata.value().size == 1U,
           "listing closed FileService's owned directory fd");

    std::atomic<bool> concurrent_ok{true};
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 8; ++worker) {
        readers.emplace_back([&] {
            for (int iteration = 0; iteration < 50; ++iteration) {
                const auto concurrent = service.listFiles();
                if (!concurrent.ok() || concurrent.value().size() != 3U ||
                    concurrent.value()[0].name != "a-first.txt" ||
                    concurrent.value()[2].name != "z-last.txt") {
                    concurrent_ok.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    expect(concurrent_ok.load(std::memory_order_relaxed),
           "concurrent listings shared or corrupted directory iteration");
}

void testUploadCommitAndOverwrite() {
    TemporaryTree tree;
    writeFile(tree.data() / "old.txt", "OLD");
    FileService service = makeService(tree.data());

    auto upload_result = service.createTemp();
    expect(upload_result.ok(), "createTemp failed");
    UploadFile upload = upload_result.takeValue();
    const std::string temporary_name{upload.temporaryName()};
    expect(std::filesystem::exists(tree.data() / temporary_name),
           "temporary file was not created");

    expect(upload.writeAll("NEW").ok(), "upload write failed");
    expect(upload.commit("old.txt").ok(), "upload commit failed");
    expect(upload.committed(), "upload did not record committed state");
    expect(readFile(tree.data() / "old.txt") == "NEW",
           "commit did not atomically replace old content");
    expect(!std::filesystem::exists(tree.data() / temporary_name),
           "temporary file remains after commit");

    auto new_upload_result = service.createTemp();
    expect(new_upload_result.ok(), "createTemp for new file failed");
    UploadFile new_upload = new_upload_result.takeValue();
    expect(new_upload.writeAll("FRESH").ok(), "new upload write failed");
    expect(new_upload.commit("new.txt").ok(), "new file commit failed");
    expect(readFile(tree.data() / "new.txt") == "FRESH",
           "RENAME_NOREPLACE commit produced wrong content");
}

void testUploadAbort() {
    TemporaryTree tree;
    FileService service = makeService(tree.data());

    auto upload_result = service.createTemp();
    expect(upload_result.ok(), "createTemp failed");
    UploadFile upload = upload_result.takeValue();
    const std::string temporary_name{upload.temporaryName()};

    expect(upload.writeAll("PARTIAL").ok(), "upload write failed");
    expect(upload.abort().ok(), "upload abort failed");
    expect(upload.abort().ok(), "repeated abort must be safe");
    expect(!std::filesystem::exists(tree.data() / temporary_name),
           "temporary file remains after abort");
    expect(countTemporaryFiles(tree.data()) == 0U,
           "abort leaked temporary files");
}

void testUploadDestructorCleanup() {
    TemporaryTree tree;
    std::string temporary_name;
    {
        FileService service = makeService(tree.data());
        auto upload_result = service.createTemp();
        expect(upload_result.ok(), "createTemp failed");
        UploadFile upload = upload_result.takeValue();
        temporary_name = std::string{upload.temporaryName()};
        expect(upload.writeAll("PARTIAL").ok(), "upload write failed");
    }

    expect(!std::filesystem::exists(tree.data() / temporary_name),
           "UploadFile destructor did not remove temporary file");
    expect(countTemporaryFiles(tree.data()) == 0U,
           "destructor leaked temporary files");
}

void testFailedUploadPreservesFinalFile() {
    TemporaryTree tree;
    writeFile(tree.data() / "stable.txt", "STABLE");
    FileService service = makeService(tree.data());

    {
        auto upload_result = service.createTemp();
        expect(upload_result.ok(), "createTemp failed");
        UploadFile upload = upload_result.takeValue();
        expect(upload.writeAll("PARTIAL").ok(), "upload write failed");

        const auto committed = upload.commit("a..b");
        expect(!committed.ok() &&
                   committed.error() == FileError::kInvalidName,
               "invalid final filename should reject commit");
    }

    expect(readFile(tree.data() / "stable.txt") == "STABLE",
           "failed upload changed an existing file");
    expect(countTemporaryFiles(tree.data()) == 0U,
           "failed upload leaked temporary files");
}

void testCommitRejectsNonRegularTarget() {
    TemporaryTree tree;
    const auto outside = tree.root() / "outside.txt";
    writeFile(outside, "OUTSIDE");
    expect(::symlink(outside.c_str(), (tree.data() / "target").c_str()) == 0,
           "unable to create target symlink");
    expect(std::filesystem::create_directory(tree.data() / "target-dir"),
           "unable to create target directory");
    expect(::mkfifo((tree.data() / "target-fifo").c_str(), 0600) == 0,
           "unable to create target FIFO");

    FileService service = makeService(tree.data());
    for (const std::string_view target : {"target", "target-dir",
                                          "target-fifo"}) {
        std::string temporary_name;
        {
            auto upload_result = service.createTemp();
            expect(upload_result.ok(), "createTemp failed");
            UploadFile upload = upload_result.takeValue();
            temporary_name = std::string{upload.temporaryName()};
            expect(upload.writeAll("NEW").ok(), "upload write failed");

            const auto committed = upload.commit(target);
            expect(!committed.ok() &&
                       committed.error() == FileError::kNotRegularFile,
                   "commit should reject non-regular target");
            expect(std::filesystem::exists(tree.data() / temporary_name),
                   "failed commit should retain temp until RAII cleanup");
        }
        expect(!std::filesystem::exists(tree.data() / temporary_name),
               "failed commit leaked temporary file");
    }

    expect(readFile(outside) == "OUTSIDE",
           "commit modified symlink target");
    expect(std::filesystem::is_symlink(tree.data() / "target"),
           "commit replaced symlink target entry");
    expect(std::filesystem::is_directory(tree.data() / "target-dir"),
           "commit replaced directory target");

    struct stat fifo_status {};
    expect(::lstat((tree.data() / "target-fifo").c_str(), &fifo_status) == 0 &&
               S_ISFIFO(fifo_status.st_mode),
           "commit replaced FIFO target");
    expect(countTemporaryFiles(tree.data()) == 0U,
           "failed commits leaked temporary files");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"filename validation", testFilenameValidation},
        {"initialization failures", testInitializationFailures},
        {"metadata, read, and remove", testMetadataReadAndRemove},
        {"symlink, directory, and FIFO rejection",
         testSymlinkDirectoryAndFifoRejection},
        {"file listing", testFileListing},
        {"upload commit and overwrite", testUploadCommitAndOverwrite},
        {"upload abort", testUploadAbort},
        {"upload destructor cleanup", testUploadDestructorCleanup},
        {"failed upload preserves final file",
         testFailedUploadPreservesFinalFile},
        {"commit rejects non-regular target",
         testCommitRejectsNonRegularTarget},
    };

    std::size_t passed = 0;
    for (const TestCase& test : tests) {
        try {
            test.function();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": "
                      << error.what() << '\n';
            return 1;
        }
    }

    std::cout << passed << " file service test groups passed\n";
    return 0;
}
