#pragma once

#include <filesystem>

namespace vmpx
{
    bool ExtractZipSafely(const std::filesystem::path& zip_path, const std::filesystem::path& output_dir);
}
