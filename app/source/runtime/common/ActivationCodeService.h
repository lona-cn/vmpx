#pragma once

#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "VMPX.h"

namespace vmpx
{
    struct ActivationFailure
    {
        enum class Code
        {
            invalid_request,
            not_found,
            expired,
            internal
        };

        Code code;
        std::string message;
    };

    class ActivationCodeService
    {
    public:
        explicit ActivationCodeService(const std::filesystem::path& data_dir);

        std::expected<std::string, ActivationFailure> Create(
            const ProductInfoEntity& product_info, const SerialInfo& serial_info);
        std::expected<SerialNumberInfo, ActivationFailure> Activate(
            std::string_view activation_code, std::string_view hwid);

    private:
        struct Record
        {
            ProductInfoEntity product_info;
            std::string user_name;
            std::string email;
            int exp_year{};
            int exp_month{};
            int exp_day{};
        };
        struct Config
        {
            std::unordered_map<std::string, Record> records;
        };


        void SaveConfig();
        void LoadConfig();

        std::mutex mutex_;
        std::filesystem::path config_path_;
        Config config_;
    };
}
