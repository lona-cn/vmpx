#include "AppPackService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <memory>
#include <ranges>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <utility>

#include <pugixml.hpp>
#include <tobiaslocker_base64/base64.hpp>
#include <utf8cpp/utf8.h>
#include <ylt/struct_yaml/yaml_reader.h>
#include <ylt/struct_yaml/yaml_writer.h>

#include "Utils.h"
#include "VMPX.h"
#include "ZipUtils.h"


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

    bool IsSafeAppName(std::string_view name)
    {
        if (!utf8::is_valid(name.begin(), name.end()) ||
            name.empty() || name == "." || name == ".." ||
            name.back() == '.' || name.back() == ' ')
            return false;
        for (unsigned char ch : name)
        {
            if (ch < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
                ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
                return false;
        }
        return IsSafeZipComponent(name);
    }

}

struct vmpx::app_pack::AppPackService::Impl
{
    std::shared_mutex mutex;
    std::unordered_set<std::string> packing;
};

vmpx::app_pack::AppPackService::AppPackService(const std::filesystem::path& vmp_console_app_path,
                                               const std::filesystem::path& data_dir):
    impl_(std::make_unique<Impl>()),
    vmp_console_app_path_(vmp_console_app_path), data_dir_(data_dir),
    zip_dir_(data_dir / "zip"), unzip_dir_(data_dir / "unzip"), packed_dir_(data_dir / "packed"),
    config_path_(data_dir / "config.yml")
{
    // 创建各个子目录
    if (!std::filesystem::exists(zip_dir_))std::filesystem::create_directories(zip_dir_);
    if (!std::filesystem::exists(unzip_dir_))std::filesystem::create_directories(unzip_dir_);
    if (!std::filesystem::exists(packed_dir_))std::filesystem::create_directories(packed_dir_);
    // 处理配置文件
    //如果是个目录，则删掉
    if (std::filesystem::is_directory(config_path_))std::filesystem::remove_all(config_path_);
    if (!std::filesystem::exists(config_path_)) //如果配置文件不存在，则保存一个空的配置文件
    {
        // SaveConfig();
    }
    else
    {
        if (!std::filesystem::exists(config_path_) || std::filesystem::is_directory(config_path_))
            throw std::runtime_error(std::format("could not open config file:{}", vmpx::PathToUtf8(config_path_)));
        std::unique_lock lock(impl_->mutex);
        LoadConfig();
        bool migrated_config = false;
        for (auto& [name, app_info] : config_.apps)
        {
            if (!IsSafeAppName(name))
            {
                app_info.packed_app_path.clear();
                migrated_config = true;
                continue;
            }
            if (app_info.packed_app_path.empty()) continue;
            auto app_output_dir = packed_dir_ / vmpx::PathFromUtf8(name);
            auto cached_output = vmpx::PathFromUtf8(app_info.packed_app_path);
            std::error_code ec;
            auto canonical_dir = std::filesystem::weakly_canonical(app_output_dir, ec);
            auto valid_cached = !ec;
            auto canonical_file = valid_cached ? std::filesystem::weakly_canonical(cached_output, ec)
                                               : std::filesystem::path{};
            valid_cached = valid_cached && !ec && IsWithin(canonical_dir, canonical_file);
            if (valid_cached) valid_cached = std::filesystem::is_regular_file(canonical_file, ec) && !ec;
            if (!valid_cached)
            {
                app_info.packed_app_path.clear();
                migrated_config = true;
            }
        }
        if (migrated_config) SaveConfig();
    }
}

vmpx::app_pack::AppPackService::~AppPackService() = default;

std::expected<vmpx::app_pack::AppInfo, std::string> vmpx::app_pack::AppPackService::Add(
    std::string_view name, std::span<uint8_t> zip_file_data, std::filesystem::path vmp_file_path)
{
    if (!IsSafeAppName(name)) return std::unexpected{"invalid application name"};
    if (zip_file_data.size() > vmpx::ZipExtractionLimits{}.max_archive_bytes)
        return std::unexpected{"uploaded archive exceeds the maximum compressed size"};

    static std::atomic_uint64_t stage_sequence{};
    const auto stage_id = std::to_string(stage_sequence.fetch_add(1, std::memory_order_relaxed));
    const std::string name_str{name};
    const auto stage_name = name_str + ".staging-" + stage_id;
    const auto staged_zip = zip_dir_ / vmpx::PathFromUtf8(stage_name + ".zip");
    const auto staged_unzip = unzip_dir_ / vmpx::PathFromUtf8(stage_name);
    const auto zip_file_path = zip_dir_ / vmpx::PathFromUtf8(name_str + ".zip");
    const auto app_unzip_dir_path = unzip_dir_ / vmpx::PathFromUtf8(name_str);
    struct StageCleanup
    {
        std::filesystem::path zip;
        std::filesystem::path directory;
        ~StageCleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(zip, ec);
            std::filesystem::remove_all(directory, ec);
        }
    } cleanup{staged_zip, staged_unzip};

    if (!WriteFile(zip_file_data, staged_zip))
        throw std::runtime_error{"unable to write staged zip data"};
    std::error_code fs_error;
    std::filesystem::create_directories(staged_unzip, fs_error);
    if (fs_error) throw std::system_error(fs_error, "unable to create staged application directory");
    if (!vmpx::ExtractZipSafely(staged_zip, staged_unzip))
        return std::unexpected{"unable to unzip file"};

    std::filesystem::path staged_vmp_file;
    if (vmp_file_path.empty())
    {
        auto vmp_files = FindFiles(staged_unzip, [](const std::filesystem::path& filename)
        {
            return filename.extension() == ".vmp";
        });
        if (vmp_files.empty()) return std::unexpected{"unable to find .vmp project file"};
        staged_vmp_file = vmp_files.front();
    }
    else
    {
        std::filesystem::path relative_vmp_path;
        if (!IsSafeRelativePath(vmpx::PathToUtf8(vmp_file_path), relative_vmp_path))
            return std::unexpected{"vmp file path must be relative to the uploaded archive"};
        staged_vmp_file = staged_unzip / relative_vmp_path;
    }
    auto canonical_staged_root = std::filesystem::weakly_canonical(staged_unzip, fs_error);
    if (fs_error) throw std::system_error(fs_error, "unable to resolve staged application directory");
    auto canonical_vmp_file = std::filesystem::weakly_canonical(staged_vmp_file, fs_error);
    if (fs_error) throw std::system_error(fs_error, "unable to resolve VMP project file");
    auto is_regular_vmp_file = std::filesystem::is_regular_file(canonical_vmp_file, fs_error);
    if (fs_error) throw std::system_error(fs_error, "unable to inspect VMP project file");
    if (!IsWithin(canonical_staged_root, canonical_vmp_file) || !is_regular_vmp_file)
        return std::unexpected{"vmp file is invalid"};
    const auto vmp_relative_path = canonical_vmp_file.lexically_relative(canonical_staged_root);

    std::unique_lock lock(impl_->mutex);
    if (impl_->packing.contains(name_str))
        return std::unexpected{"application is currently being packed"};
    AppInfo app_info{vmpx::PathToUtf8(app_unzip_dir_path / vmp_relative_path), ""};
    auto existing = config_.apps.find(name_str);
    const bool had_config = existing != config_.apps.end();
    AppInfo previous_info;
    if (had_config) previous_info = existing->second;
    const auto backup_suffix = ".backup-" + stage_id;
    const auto backup_zip = zip_dir_ / vmpx::PathFromUtf8(name_str + backup_suffix + ".zip");
    const auto backup_unzip = unzip_dir_ / vmpx::PathFromUtf8(name_str + backup_suffix);
    bool backed_up_zip = false;
    bool backed_up_unzip = false;
    auto restore_backups = [&]
    {
        std::error_code ignored;
        if (backed_up_unzip) std::filesystem::rename(backup_unzip, app_unzip_dir_path, ignored);
        if (backed_up_zip) std::filesystem::rename(backup_zip, zip_file_path, ignored);
    };
    const bool existing_zip = std::filesystem::exists(zip_file_path, fs_error);
    if (fs_error) throw std::system_error(fs_error, "unable to inspect existing archive");
    if (existing_zip)
    {
        std::filesystem::rename(zip_file_path, backup_zip, fs_error);
        if (fs_error) throw std::system_error(fs_error, "unable to stage existing archive");
        backed_up_zip = true;
    }
    const bool existing_unzip = std::filesystem::exists(app_unzip_dir_path, fs_error);
    if (fs_error)
    {
        restore_backups();
        throw std::system_error(fs_error, "unable to inspect existing application");
    }
    if (existing_unzip)
    {
        std::filesystem::rename(app_unzip_dir_path, backup_unzip, fs_error);
        if (fs_error)
        {
            restore_backups();
            throw std::system_error(fs_error, "unable to stage existing application");
        }
        backed_up_unzip = true;
    }
    std::filesystem::rename(staged_zip, zip_file_path, fs_error);
    if (fs_error)
    {
        restore_backups();
        throw std::system_error(fs_error, "unable to install replacement archive");
    }
    std::filesystem::rename(staged_unzip, app_unzip_dir_path, fs_error);
    if (fs_error)
    {
        std::error_code ignored;
        std::filesystem::remove(zip_file_path, ignored);
        restore_backups();
        throw std::system_error(fs_error, "unable to install replacement application");
    }

    try
    {
        config_.apps.insert_or_assign(name_str, app_info);
        SaveConfig();
    }
    catch (...)
    {
        if (had_config) config_.apps.insert_or_assign(name_str, std::move(previous_info));
        else config_.apps.erase(name_str);
        std::error_code ignored;
        std::filesystem::remove(zip_file_path, ignored);
        std::filesystem::remove_all(app_unzip_dir_path, ignored);
        restore_backups();
        throw;
    }
    std::filesystem::remove_all(backup_zip, fs_error);
    std::filesystem::remove_all(backup_unzip, fs_error);
    std::filesystem::remove_all(packed_dir_ / vmpx::PathFromUtf8(name_str), fs_error);
    return app_info;
}
bool vmpx::app_pack::AppPackService::Remove(std::string_view name)
{
    if (!IsSafeAppName(name)) return false;

    std::unique_lock lock(impl_->mutex);
    const std::string app_name{name};
    if (impl_->packing.contains(app_name)) return false;
    auto it = config_.apps.find(app_name);
    if (it == config_.apps.end()) return false;
    auto app_path = vmpx::PathFromUtf8(app_name);
    auto zip_file_path = zip_dir_ / app_path;
    zip_file_path += ".zip";
    auto app_unzip_dir_path = unzip_dir_ / app_path;
    auto app_packed_dir_path = packed_dir_ / app_path;
    static std::atomic_uint64_t removal_sequence{};
    auto removal_suffix = ".removing-" +
        std::to_string(removal_sequence.fetch_add(1, std::memory_order_relaxed));
    const std::array<std::pair<std::filesystem::path, std::filesystem::path>, 3> paths{
        std::pair{zip_file_path, zip_dir_ / vmpx::PathFromUtf8(app_name + removal_suffix + ".zip")},
        std::pair{app_unzip_dir_path, unzip_dir_ / vmpx::PathFromUtf8(app_name + removal_suffix)},
        std::pair{app_packed_dir_path, packed_dir_ / vmpx::PathFromUtf8(app_name + removal_suffix)}
    };
    std::array<bool, 3> moved{};
    auto restore_files = [&]
    {
        std::error_code ignored;
        for (std::size_t i = paths.size(); i > 0; --i)
            if (moved[i - 1]) std::filesystem::rename(paths[i - 1].second, paths[i - 1].first, ignored);
    };
    for (std::size_t i = 0; i < paths.size(); ++i)
    {
        std::error_code fs_error;
        if (!std::filesystem::exists(paths[i].first, fs_error) && !fs_error) continue;
        if (!fs_error) std::filesystem::rename(paths[i].first, paths[i].second, fs_error);
        if (fs_error)
        {
            restore_files();
            throw std::system_error(fs_error, "unable to stage application removal");
        }
        moved[i] = true;
    }

    auto removed_record = config_.apps.extract(it);
    try
    {
        SaveConfig();
    }
    catch (...)
    {
        config_.apps.insert(std::move(removed_record));
        restore_files();
        throw;
    }
    for (const auto& path : paths)
    {
        std::error_code ignored;
        std::filesystem::remove_all(path.second, ignored);
    }
    return true;
}

std::vector<std::string> vmpx::app_pack::AppPackService::List() const
{
    std::vector<std::string> app_names;
    std::shared_lock lock(impl_->mutex);
    app_names.reserve(config_.apps.size());
    for (const auto& key : config_.apps | std::views::keys)
    {
        app_names.push_back(key);
    }
    return app_names;
}

std::filesystem::path vmpx::app_pack::AppPackService::GetPacked(std::string_view name)
{
    std::shared_lock lock(impl_->mutex);
    auto it = config_.apps.find(std::string{name});
    if (it == config_.apps.end()) return std::filesystem::path{};
    return vmpx::PathFromUtf8(it->second.packed_app_path);
}

std::expected<std::filesystem::path, std::string> vmpx::app_pack::AppPackService::Pack(std::string_view name)
{
    if (!IsSafeAppName(name)) return std::unexpected{"invalid application name"};
    const std::string app_name{name};
    std::string vmp_file_path;
    {
        std::unique_lock lock(impl_->mutex);
        auto it = config_.apps.find(app_name);
        if (it == config_.apps.end()) return std::unexpected{"unable to find app"};
        if (!impl_->packing.insert(app_name).second)
            return std::unexpected{"application is already being packed"};
        vmp_file_path = it->second.vmp_file_path;
    }
    struct PackGuard
    {
        Impl* impl;
        std::string name;
        ~PackGuard()
        {
            std::unique_lock lock(impl->mutex);
            impl->packing.erase(name);
        }
    } guard{impl_.get(), app_name};

    auto app_packed_dir = packed_dir_ / vmpx::PathFromUtf8(app_name);
    std::error_code fs_error;
    std::filesystem::create_directories(app_packed_dir, fs_error);
    if (fs_error) return std::unexpected{"unable to create application output directory"};
    auto packed_app_path = PackApp(vmp_console_app_path_, vmpx::PathFromUtf8(vmp_file_path), app_packed_dir);
    if (!packed_app_path) return std::unexpected{std::format("unable to pack app:{}", packed_app_path.error())};
    {
        std::unique_lock lock(impl_->mutex);
        auto it = config_.apps.find(app_name);
        if (it == config_.apps.end() || it->second.vmp_file_path != vmp_file_path)
            return std::unexpected{"application changed while packing"};
        it->second.packed_app_path = vmpx::PathToUtf8(*packed_app_path);
        SaveConfig();
        return packed_app_path.value();
    }
}
bool vmpx::app_pack::AppPackService::Has(std::string_view name) const
{
    std::shared_lock lock(impl_->mutex);
    return this->config_.apps.contains(std::string{name});
}

std::expected<vmpx::ProductInfo, std::string> vmpx::app_pack::AppPackService::GetProductInfo(std::string_view name)
{
    using namespace pugi;
    std::shared_lock lock(impl_->mutex);
    auto it = config_.apps.find(std::string{name});
    if (it == config_.apps.end()) return std::unexpected{"unable to find app"};
    auto& app_info = it->second;
    xml_document doc;
    auto vmp_file_path = vmpx::PathFromUtf8(app_info.vmp_file_path);
    auto load_result = doc.load_file(vmp_file_path.c_str());
    if (!load_result)
        return std::unexpected{
            std::format("unable to parse vmp file:{},{}",
                        app_info.vmp_file_path, load_result.description())
        };
    auto license_manager_node = doc.child("Document").child("LicenseManager");
    if (license_manager_node.empty())
        return std::unexpected{"unable to find Document.LicenseManager node"};
    auto product_code = license_manager_node.attribute("ProductCode").as_string();
    auto algorithm = license_manager_node.attribute("Algorithm").as_string();
    auto bits = license_manager_node.attribute("Bits").as_uint();
    auto public_exponent = license_manager_node.attribute("PublicExp").as_string();
    auto private_exponent = license_manager_node.attribute("PrivateExp").as_string();
    auto modulus = license_manager_node.attribute("Modulus").as_string();
    auto B642Vec = [](std::string_view data) -> std::vector<uint8_t>
    {
        return base64::decode_into<std::vector<uint8_t>>(data.begin(), data.end());
    };
    return ProductInfo{
        .key_size = (bits),
        .modulus = B642Vec(modulus),
        .public_exponent = B642Vec(public_exponent),
        .private_exponent = B642Vec(private_exponent),
        .product_code = B642Vec(product_code),
    };
}

void vmpx::app_pack::AppPackService::SaveConfig()
{
    auto temp_path = config_path_;
    temp_path += ".tmp";
    try
    {
        std::string str;
        struct_yaml::to_yaml(config_, str);
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error{"unable to open temporary config file"};
        out.write(str.data(), static_cast<std::streamsize>(str.size()));
        out.flush();
        if (!out) throw std::runtime_error{"unable to write temporary config file"};
        out.close();
        if (!out) throw std::runtime_error{"unable to close temporary config file"};
#if defined(_WIN32)
        if (!MoveFileExW(temp_path.c_str(), config_path_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "unable to replace application config");
#else
        std::error_code ec;
        std::filesystem::rename(temp_path, config_path_, ec);
        if (ec) throw std::system_error(ec, "unable to replace application config");
#endif
    }
    catch (const std::exception& e)
    {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        throw std::runtime_error(std::format("could not create config file:{}, error msg:{}",
                                             vmpx::PathToUtf8(config_path_), e.what()));
    }
}

void vmpx::app_pack::AppPackService::LoadConfig()
{
    auto data_opt = ReadFile(config_path_);
    if (!data_opt)throw std::runtime_error(std::format("read config file failed"));
    auto& data = data_opt.value();
    std::string str{data.begin(), data.end()};
    std::error_code ec;
    struct_yaml::from_yaml(config_, str, ec);
    if (ec)
        throw std::runtime_error(std::format("could not parse config file:{}, error msg:{}",
                                             config_path_.string(), ec.message()));
}
