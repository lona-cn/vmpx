#pragma once

#include <cstdint>
#include <filesystem>

namespace vmpx
{
    struct ZipExtractionLimits
    {
        std::uint64_t max_archive_bytes{256ull * 1024 * 1024};
        std::uint64_t max_entries{10000};
        std::uint64_t max_entry_bytes{512ull * 1024 * 1024};
        std::uint64_t max_total_bytes{1024ull * 1024 * 1024};
    };

    bool ExtractZipSafely(const std::filesystem::path& zip_path, const std::filesystem::path& output_dir,
                          ZipExtractionLimits limits = {});
}
