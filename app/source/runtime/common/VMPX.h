#pragma once

#include <vector>
#include <windows.h>
#include <KeyGenAPI.h>
#include <array>
#include <span>
#include <ranges>
#include <expected>
#include <filesystem>
#include <memory>

namespace vmpx
{
    inline constexpr VMProtectAlgorithms DEFAULT_ALGORITHM = ALGORITHM_RSA;

    struct HWID
    {
        static constexpr size_t STRIDE = 4;
        using ItemType = std::array<uint8_t, STRIDE>;

        ItemType cpu, host, hdd;
        std::vector<ItemType> network_adapters;

        template <typename T>
        static HWID FromData(std::span<T> data) noexcept
        {
            auto bytes = std::span<uint8_t>(reinterpret_cast<uint8_t*>(data.data()), data.size_bytes());
            return FromData(bytes);
        }

        static HWID FromData(std::span<uint8_t> bytes) noexcept;

        static std::expected<HWID, std::string> FromBase64(std::string_view str);

        auto ToString() const noexcept -> std::string;

        std::vector<uint8_t> ToBytes() const noexcept;

        auto ToBase64() const noexcept -> std::string;
    };

    struct ProductInfo
    {
        uint32_t key_size;
        std::vector<byte> modulus;
        std::vector<byte> public_exponent;
        std::vector<byte> private_exponent;
        std::vector<byte> product_code;

        static std::string ToJson(const ProductInfo& pi) noexcept;
        static std::expected<ProductInfo, std::string> FromJson(const std::string& json) noexcept;

        std::unique_ptr<VMProtectProductInfo> ToVMP() const noexcept;

        std::string ToJson() const
        {
            return ToJson(*this);
        }
    };

    struct ProductInfoEntity
    {
        uint32_t key_size;
        std::string modulus;
        std::string public_exponent;
        std::string private_exponent;
        std::string product_code;

        static ProductInfoEntity FromProductInfo(const ProductInfo& info);

        static std::expected<ProductInfoEntity, std::string> FromJson(const std::string& str) noexcept;

        std::string ToJson() noexcept;

        ProductInfo ToProductInfo() const noexcept;
    };

    struct WideFields
    {
        mutable std::vector<wchar_t> user_name;
        mutable std::vector<wchar_t> email;
    };

    struct SerialInfo
    {
        std::string user_name;
        std::string email;
        std::string hwid;
        int exp_year;
        int exp_month;
        int exp_day;
        std::shared_ptr<WideFields> wide_fields = std::make_shared<WideFields>();

        std::unique_ptr<VMProtectSerialNumberInfo> ToVMP() const noexcept;
    };

    struct SerialNumberInfo
    {
        std::string serial_number;
        int expired_year;
        int expired_month;
        int expired_day;
    };

    std::expected<SerialNumberInfo, std::string> GenSerialNumber(
        const ProductInfo& pi,
        const SerialInfo& si) noexcept;

    ProductInfo GenRandomProductInfo(size_t key_size, bool random_public_exponent = false) noexcept;

    std::expected<std::filesystem::path, std::string> PackApp(const std::filesystem::path& vmp_console_app_path,
                                                              const std::filesystem::path& vmp_file_path,
                                                              const std::filesystem::path& output_dir =
                                                                  std::filesystem::path{});
}
