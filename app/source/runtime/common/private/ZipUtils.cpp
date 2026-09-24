#include "../ZipUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include <zip.h>

#include "../Utils.h"

namespace
{
    bool IsSafeZipComponent(std::string_view component)
    {
        if (component.empty() || component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ')
            return false;
        auto stem = component.substr(0, component.find('.'));
        std::string device_name;
        device_name.reserve(stem.size());
        for (unsigned char ch : stem) device_name.push_back(static_cast<char>(std::toupper(ch)));
        if (device_name == "CON" || device_name == "PRN" || device_name == "AUX" || device_name == "NUL")
            return false;
        return !(device_name.size() == 4 &&
            (device_name.starts_with("COM") || device_name.starts_with("LPT")) &&
            device_name[3] >= '1' && device_name[3] <= '9');
    }

    bool IsSafeRelativePath(std::string_view value, std::filesystem::path& relative)
    {
        if (value.empty()) return false;
        std::string normalized{value};
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (normalized.find(':') != std::string::npos) return false;
        std::size_t start = 0;
        while (start < normalized.size())
        {
            auto end = normalized.find('/', start);
            auto component = std::string_view{normalized}.substr(start, end == std::string::npos
                ? normalized.size() - start : end - start);
            if (!component.empty() && !IsSafeZipComponent(component)) return false;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        try
        {
            relative = vmpx::PathFromUtf8(normalized);
        }
        catch (...)
        {
            return false;
        }
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) return false;
        for (const auto& component : relative)
            if (component == std::filesystem::path{".."} || component == std::filesystem::path{"."})
                return false;
        relative = relative.lexically_normal();
        return !relative.empty();
    }

    bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate)
    {
        auto relative = candidate.lexically_relative(root);
        return !relative.empty() && !relative.is_absolute() &&
            *relative.begin() != std::filesystem::path{".."};
    }
}

bool vmpx::ExtractZipSafely(const std::filesystem::path& zip_path, const std::filesystem::path& output_dir)
{
    auto zip_path_str = vmpx::PathToUtf8(zip_path);
    int err = 0;
    zip* raw_archive = zip_open(zip_path_str.c_str(), ZIP_RDONLY, &err);
    if (!raw_archive) return false;
    std::unique_ptr<zip, decltype(&zip_discard)> archive{raw_archive, &zip_discard};
    std::error_code fs_error;
    std::filesystem::create_directories(output_dir, fs_error);
    if (fs_error) return false;
    auto root = std::filesystem::absolute(output_dir, fs_error).lexically_normal();
    if (fs_error) return false;
    zip_int64_t num_entries = zip_get_num_entries(archive.get(), 0);
    if (num_entries < 0) return false;
    std::array<char, 64 * 1024> buffer{};
    for (zip_int64_t i = 0; i < num_entries; ++i)
    {
        struct zip_stat st;
        zip_stat_init(&st);
        if (zip_stat_index(archive.get(), i, 0, &st) != 0 || !st.name) return false;

        std::filesystem::path relative;
        if (!IsSafeRelativePath(st.name, relative)) return false;
        auto destination = (root / relative).lexically_normal();
        if (!IsWithin(root, destination)) return false;
        auto entry_name = std::string_view{st.name};
        if (entry_name.ends_with('/') || entry_name.ends_with('\\'))
        {
            std::filesystem::create_directories(destination, fs_error);
            if (fs_error) return false;
            continue;
        }
        std::filesystem::create_directories(destination.parent_path(), fs_error);
        if (fs_error) return false;
        auto canonical_root = std::filesystem::weakly_canonical(root, fs_error);
        if (fs_error) return false;
        auto canonical_parent = std::filesystem::weakly_canonical(destination.parent_path(), fs_error);
        if (fs_error || !IsWithin(canonical_root, canonical_parent)) return false;
        auto destination_status = std::filesystem::symlink_status(destination, fs_error);
        if (fs_error == std::errc::no_such_file_or_directory) fs_error.clear();
        if (fs_error || std::filesystem::is_symlink(destination_status)) return false;

        std::unique_ptr<zip_file, decltype(&zip_fclose)> input{
            zip_fopen_index(archive.get(), i, 0), &zip_fclose
        };
        if (!input) return false;
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        zip_uint64_t remaining = st.size;
        while (remaining > 0)
        {
            auto requested = static_cast<zip_uint64_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            auto count = zip_fread(input.get(), buffer.data(), requested);
            if (count <= 0 || static_cast<zip_uint64_t>(count) > requested) return false;
            output.write(buffer.data(), static_cast<std::streamsize>(count));
            if (!output) return false;
            remaining -= static_cast<zip_uint64_t>(count);
        }
        if (zip_fclose(input.release()) != 0) return false;
        output.close();
        if (!output) return false;
    }
    auto* finished_archive = archive.release();
    return zip_close(finished_archive) == 0;
}
