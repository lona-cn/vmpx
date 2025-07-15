#pragma once
#include <expected>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <memory>

#include "VMPX.h"

namespace vmpx
{
    namespace app_pack
    {
        bool UnzipToDir(const std::filesystem::path& zip_path, const std::filesystem::path& output_dir);

        struct AppInfo
        {
            // std::filesystem::path vmp_file_path;
            // std::filesystem::path packed_app_path;
            std::string vmp_file_path;
            std::string packed_app_path;
        };

        struct Config
        {
            std::unordered_map<std::string, AppInfo> apps;
        };

        /**
         * TODO：线程安全
         */
        class AppPackService
        {
        public:
            //还得是PImpl
            struct Impl;

            AppPackService(
                const std::filesystem::path& vmp_console_app_path,
                const std::filesystem::path& data_dir);
            ~AppPackService();
            AppPackService(const AppPackService& other) = delete;
            AppPackService(AppPackService&& other) noexcept = default;
            AppPackService& operator=(const AppPackService& other) = delete;
            AppPackService& operator=(AppPackService&& other) noexcept = default;

            /**
             * 添加或覆盖已有程序
             * @param name 程序名，必须全局唯一
             * @param zip_file_data 压缩包文件 
             * @param vmp_file_path .vmp文件在压缩包内的相对路径，默认则自动搜索
             * @return 如果成功则返回AppInfo，否则返回错误原因
             */
            std::expected<AppInfo, std::string> Add(std::string_view name, std::span<uint8_t> zip_file_data,
                                                    std::filesystem::path vmp_file_path = "");

            /**
             * 移除某个已添加的应用
             * @param name 
             * @return 
             */
            bool Remove(std::string_view name);

            /**
             * 列出所有程序
             * @return 
             */
            std::vector<std::string> List() const;

            /**
             * 
             * @param name 
             * @return 返回已打包程序的所在路径，如果不存在则返回empty
             */
            std::filesystem::path GetPacked(std::string_view name);

            /**
             * 打包程序，这个函数非常耗时，最好放在线程池
             * @param name 
             * @return 
             */
            std::expected<std::filesystem::path, std::string> Pack(std::string_view name);

            bool Has(std::string_view name) const;

            std::expected<ProductInfo, std::string> GetProductInfo(std::string_view name);

        private:
            /**
             * 保存配置文件
             */
            void SaveConfig();

            void LoadConfig();
            std::unique_ptr<Impl> impl_;
            std::filesystem::path vmp_console_app_path_;
            std::filesystem::path data_dir_;
            std::filesystem::path zip_dir_, unzip_dir_, packed_dir_;
            std::filesystem::path config_path_;
            Config config_;
        };
    }
}
