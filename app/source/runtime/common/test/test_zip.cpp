#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <zip.h>

#include "Utils.h"
#include "ZipUtils.h"


namespace
{
    bool AddEntry(zip_t* archive, const char* name, const std::string& contents)
    {
        auto* source = zip_source_buffer(archive, contents.data(), contents.size(), 0);
        if (!source) return false;
        if (zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8) < 0)
        {
            zip_source_free(source);
            return false;
        }
        return true;
    }

    bool CreateArchive(const std::filesystem::path& path, const char* name, const std::string& contents)
    {
        int error = 0;
        auto archive_path = vmpx::PathToUtf8(path);
        auto* archive = zip_open(archive_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
        if (!archive) return false;
        if (!AddEntry(archive, name, contents))
        {
            zip_discard(archive);
            return false;
        }
        if (zip_close(archive) == 0) return true;
        zip_discard(archive);
        return false;
    }
}
bool CreateArchive(const std::filesystem::path& path,
                   const std::vector<std::pair<std::string, std::string>>& entries)
{
    int error = 0;
    auto archive_path = vmpx::PathToUtf8(path);
    auto* archive = zip_open(archive_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive) return false;
    for (const auto& [name, contents] : entries)
    {
        if (!AddEntry(archive, name.c_str(), contents))
        {
            zip_discard(archive);
            return false;
        }
    }
    if (zip_close(archive) == 0) return true;
    zip_discard(archive);
    return false;
}


int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto test_dir = std::filesystem::temp_directory_path() /
        ("vmpx-zip-test-" + std::to_string(unique));
    const auto output_dir = test_dir / "output";
    const auto archive_path = test_dir / "valid.zip";
    const auto traversal_path = test_dir / "traversal.zip";
    const auto device_path = test_dir / "device.zip";
    const auto symlink_path = test_dir / "symlink.zip";
    const auto unicode_path = test_dir / "unicode.zip";
    const auto entries_path = test_dir / "entries.zip";
    const auto expanded_path = test_dir / "expanded.zip";
    const auto total_path = test_dir / "total.zip";
    std::filesystem::create_directories(output_dir);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{test_dir};

    if (!CreateArchive(archive_path, "nested/app.vmp", "project") ||
        !vmpx::ExtractZipSafely(archive_path, output_dir))
        return 1;
    vmpx::ZipExtractionLimits limits{};
    limits.max_archive_bytes = 1;
    if (vmpx::ExtractZipSafely(archive_path, test_dir / "oversized", limits))
        return 8;

    if (!CreateArchive(entries_path, std::vector<std::pair<std::string, std::string>>{
            {"one.txt", "1"}, {"two.txt", "2"}}))
        return 9;
    limits = {};
    limits.max_entries = 1;
    if (vmpx::ExtractZipSafely(entries_path, test_dir / "too-many", limits))
        return 10;

    if (!CreateArchive(expanded_path, "large.txt", "12345"))
        return 11;
    limits = {};
    limits.max_entry_bytes = 4;
    if (vmpx::ExtractZipSafely(expanded_path, test_dir / "too-large-entry", limits))
        return 12;

    if (!CreateArchive(total_path, std::vector<std::pair<std::string, std::string>>{
            {"one.txt", "123"}, {"two.txt", "456"}}))
        return 13;
    limits = {};
    limits.max_total_bytes = 5;
    if (vmpx::ExtractZipSafely(total_path, test_dir / "too-large-total", limits))
        return 14;
    std::ifstream extracted(output_dir / "nested" / "app.vmp", std::ios::binary);
    std::string contents{std::istreambuf_iterator<char>{extracted}, {}};
    if (!CreateArchive(unicode_path, "\xE8\xB5\x84\xE6\x96\x99/\xE7\xA8\x8B\xE5\xBA\x8F.vmp", "unicode") ||
        !vmpx::ExtractZipSafely(unicode_path, output_dir))
        return 6;
    std::ifstream unicode_file(output_dir /
        vmpx::PathFromUtf8("\xE8\xB5\x84\xE6\x96\x99/\xE7\xA8\x8B\xE5\xBA\x8F.vmp"), std::ios::binary);
    std::string unicode_contents{std::istreambuf_iterator<char>{unicode_file}, {}};
    if (unicode_contents != "unicode") return 7;
    if (contents != "project") return 2;

    if (!CreateArchive(traversal_path, "../escape.txt", "outside") ||
        vmpx::ExtractZipSafely(traversal_path, output_dir) ||
        std::filesystem::exists(test_dir / "escape.txt"))
        return 3;

    if (!CreateArchive(device_path, "CON.txt", "device") ||
        vmpx::ExtractZipSafely(device_path, output_dir))
        return 4;
    const auto outside_dir = test_dir / "outside";
    std::filesystem::create_directories(outside_dir);
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(outside_dir, output_dir / "linked", symlink_error);
    if (!symlink_error &&
        (!CreateArchive(symlink_path, "linked/escape.txt", "outside") ||
         vmpx::ExtractZipSafely(symlink_path, output_dir) ||
         std::filesystem::exists(outside_dir / "escape.txt")))
        return 5;
    return 0;
}
