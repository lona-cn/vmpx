#include "AppPackService.h"

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <pugixml.hpp>
#include <ranges>
#include <shared_mutex>

#include <zip.h>
#include <ylt/struct_yaml/yaml_reader.h>
#include <ylt/struct_yaml/yaml_writer.h>
#include <boost/asio.hpp>
#include <tobiaslocker_base64/base64.hpp>

#include "VMPX.h"
#include "Utils.h"

bool vmpx::app_pack::UnzipToDir(const std::filesystem::path& zip_path, const std::filesystem::path& output_dir)
{
    auto zip_path_str = zip_path.string();
    auto output_dir_str = output_dir.string();
    int err = 0;
    zip* za = zip_open(zip_path_str.c_str(), ZIP_RDONLY, &err);
    if (!za) return false;

    zip_int64_t num_entries = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < num_entries; ++i)
    {
        struct zip_stat st;
        zip_stat_init(&st);
        zip_stat_index(za, i, 0, &st);

        zip_file* zf = zip_fopen_index(za, i, 0);
        if (!zf) continue;

        std::vector<char> buffer(st.size);
        zip_fread(zf, buffer.data(), st.size);
#if defined(_WIN32)
        auto file_path = std::filesystem::path(output_dir_str) / U8ToWString(st.name);
#else
        auto file_path = std::filesystem::path(output_dir_str) / st.name;
#endif
        std::ofstream out(file_path, std::ios::binary);
        out.write(buffer.data(), static_cast<std::streamsize>(st.size));
        zip_fclose(zf);
    }

    return zip_close(za) == 0;
}

struct vmpx::app_pack::AppPackService::Impl
{
    std::shared_mutex mutex;
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
            throw std::runtime_error(std::format("could not open config file:{}", config_path_.string()));
        std::unique_lock lock(impl_->mutex);
        LoadConfig();
    }
}

vmpx::app_pack::AppPackService::~AppPackService() = default;

std::expected<vmpx::app_pack::AppInfo, std::string> vmpx::app_pack::AppPackService::Add(
    std::string_view name, std::span<uint8_t> zip_file_data, std::filesystem::path vmp_file_path)
{
    std::string name_str{name};
    // 删掉已经存在的项目
    Remove(name);
    std::filesystem::path zip_file_path = zip_dir_ / name += ".zip";
    std::filesystem::path app_unzip_dir_path = unzip_dir_ / name;
    if (!WriteFile(zip_file_data, zip_file_path))return std::unexpected{"unable to write zip data to file"};
    if (!std::filesystem::exists(app_unzip_dir_path))std::filesystem::create_directories(app_unzip_dir_path);
    if (!UnzipToDir(zip_file_path, app_unzip_dir_path)) return std::unexpected{"unable to unzip file"};
    if (vmp_file_path.empty()) //如果没设置.vmp文件则搜索
    {
        auto vmp_files = FindFiles(app_unzip_dir_path, [](const std::filesystem::path& filename)
        {
            return filename.extension() == ".vmp";
        });
        if (vmp_files.empty()) return std::unexpected{"unable to find .vmp project file"};
        vmp_file_path = vmp_files[0];
    }
    else
    {
        //如果vmp文件已设置，则调整为绝对路径
        vmp_file_path = app_unzip_dir_path / vmp_file_path;
    }
    if (!std::filesystem::exists(vmp_file_path) || std::filesystem::is_directory(vmp_file_path))
        return std::unexpected("vmp file is invalid");
    AppInfo app_info{vmp_file_path.string(), ""};
    std::unique_lock lock(impl_->mutex);
    config_.apps.emplace(name, AppInfo{app_info});
    SaveConfig();
    return app_info;
}

bool vmpx::app_pack::AppPackService::Remove(std::string_view name)
{
    std::unique_lock lock(impl_->mutex);
    auto it = config_.apps.find(std::string{name});
    if (it == config_.apps.end()) return false;
    auto& app_info = it->second;
    // 删除各种目录和文件
    std::filesystem::path zip_file_path = zip_dir_ / name += ".zip";
    std::filesystem::path app_unzip_dir_path = unzip_dir_ / name;
    std::filesystem::remove(zip_file_path);
    std::filesystem::remove_all(app_unzip_dir_path);
    std::filesystem::remove(app_info.packed_app_path);
    config_.apps.erase(it);
    SaveConfig();
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
    return it->second.packed_app_path;
}

std::expected<std::filesystem::path, std::string> vmpx::app_pack::AppPackService::Pack(std::string_view name)
{
    std::unique_lock lock(impl_->mutex);
    auto it = config_.apps.find(std::string{name});
    if (it == config_.apps.end()) return std::unexpected{"unable to find app"};
    auto packed_app_path = PackApp(vmp_console_app_path_, it->second.vmp_file_path, packed_dir_);
    if (!packed_app_path) return std::unexpected{std::format("unable to pack app:{}", packed_app_path.error())};
    it->second.packed_app_path = packed_app_path.value().string();
    SaveConfig();
    return it->second.packed_app_path;
}

bool vmpx::app_pack::AppPackService::Has(std::string_view name) const
{
    std::shared_lock lock(impl_->mutex);
    return this->config_.apps.contains(std::string{name});
}

std::expected<vmpx::ProductInfo, std::string> vmpx::app_pack::AppPackService::GetProductInfo(std::string_view name)
{
    using namespace pugi;
    auto it = config_.apps.find(std::string{name});
    if (it == config_.apps.end()) return std::unexpected{"unable to find app"};
    auto& app_info = it->second;
    xml_document doc;
    auto load_result = doc.load_file(app_info.vmp_file_path.c_str());
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
    try
    {
        std::string str;
        struct_yaml::to_yaml(config_, str);
        std::ofstream out(config_path_, std::ios::binary);
        out.write(str.data(), static_cast<std::streamsize>(str.size()));
    }
    catch (std::exception& e)
    {
        throw std::runtime_error(std::format("could not create config file:{}, error msg:{}",
                                             config_path_.string(), e.what()));
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
