#include "../ActivationCodeService.h"

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <cryptopp/osrng.h>
#include <cryptopp/sha.h>
#include <ylt/struct_yaml/yaml_reader.h>
#include <ylt/struct_yaml/yaml_writer.h>

#include "../Utils.h"

namespace
{
    std::string ToHex(std::span<const CryptoPP::byte> bytes)
    {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (auto byte : bytes)
        {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
    }

    std::string HashActivationCode(std::string_view code)
    {
        std::array<CryptoPP::byte, CryptoPP::SHA256::DIGESTSIZE> digest{};
        CryptoPP::SHA256 hash;
        hash.CalculateDigest(digest.data(),
                             reinterpret_cast<const CryptoPP::byte*>(code.data()), code.size());
        return ToHex(digest);
    }

    std::string GenerateActivationCode(CryptoPP::AutoSeededRandomPool& random)
    {
        std::array<CryptoPP::byte, 32> bytes{};
        random.GenerateBlock(bytes.data(), bytes.size());
        return "VMPX-" + ToHex(bytes);
    }

    std::chrono::year_month_day UtcToday()
    {
        return std::chrono::year_month_day{
            std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    }

    bool IsSupportedKeySize(uint32_t key_size)
    {
        return key_size >= 1024 && key_size <= 4096 && key_size % 1024 == 0;
    }

    vmpx::ActivationFailure InvalidRequest(std::string message)
    {
        return {vmpx::ActivationFailure::Code::invalid_request, std::move(message)};
    }
}

vmpx::ActivationCodeService::ActivationCodeService(const std::filesystem::path& data_dir):
    config_path_(data_dir / "activation_codes.yml")
{
    std::filesystem::create_directories(data_dir);
    if (std::filesystem::exists(config_path_))
    {
        LoadConfig();
    }
    else
    {
        SaveConfig();
    }
}

std::expected<std::string, vmpx::ActivationFailure> vmpx::ActivationCodeService::Create(
    const ProductInfoEntity& product_info, const SerialInfo& serial_info)
{
    const std::chrono::year_month_day expiration{
        std::chrono::year{serial_info.exp_year},
        std::chrono::month{static_cast<unsigned>(serial_info.exp_month)},
        std::chrono::day{static_cast<unsigned>(serial_info.exp_day)}};
    if (!expiration.ok() || std::chrono::sys_days{expiration} < std::chrono::sys_days{UtcToday()})
        return std::unexpected(InvalidRequest("a valid future or current expiration date is required"));

    auto decoded_product_info = product_info.ToProductInfo();
    if (!decoded_product_info)
        return std::unexpected(InvalidRequest(std::format("invalid product info:{}", decoded_product_info.error())));
    const auto& decoded = *decoded_product_info;
    if (!IsSupportedKeySize(decoded.key_size) || decoded.modulus.size() != decoded.key_size / 8 ||
        decoded.public_exponent.empty() || decoded.private_exponent.empty() || decoded.product_code.size() != 8)
        return std::unexpected(InvalidRequest("product info has invalid RSA key fields"));

    Record record{
        .product_info = product_info,
        .user_name = serial_info.user_name,
        .email = serial_info.email,
        .exp_year = serial_info.exp_year,
        .exp_month = serial_info.exp_month,
        .exp_day = serial_info.exp_day
    };
    CryptoPP::AutoSeededRandomPool random;
    std::lock_guard lock(mutex_);
    std::string activation_code;
    std::string digest;
    do
    {
        activation_code = GenerateActivationCode(random);
        digest = HashActivationCode(activation_code);
    }
    while (config_.records.contains(digest));

    config_.records.emplace(digest, std::move(record));
    try
    {
        SaveConfig();
    }
    catch (const std::exception& e)
    {
        config_.records.erase(digest);
        return std::unexpected(ActivationFailure{
            ActivationFailure::Code::internal,
            std::format("unable to persist activation code:{}", e.what())});
    }
    return activation_code;
}

std::expected<vmpx::SerialNumberInfo, vmpx::ActivationFailure> vmpx::ActivationCodeService::Activate(
    std::string_view activation_code, std::string_view hwid)
{
    if (activation_code.size() != 69 || !activation_code.starts_with("VMPX-"))
        return std::unexpected(ActivationFailure{ActivationFailure::Code::not_found, "activation code not found"});
    for (char ch : activation_code.substr(5))
    {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')))
            return std::unexpected(ActivationFailure{ActivationFailure::Code::not_found, "activation code not found"});
    }
    auto decoded_hwid = HWID::FromBase64(hwid);
    if (!decoded_hwid)
        return std::unexpected(InvalidRequest(std::format("invalid HWID:{}", decoded_hwid.error())));

    Record record;
    {
        const auto digest = HashActivationCode(activation_code);
        std::lock_guard lock(mutex_);
        auto it = config_.records.find(digest);
        if (it == config_.records.end())
            return std::unexpected(ActivationFailure{ActivationFailure::Code::not_found, "activation code not found"});
        record = it->second;
    }

    const std::chrono::year_month_day expiration{
        std::chrono::year{record.exp_year},
        std::chrono::month{static_cast<unsigned>(record.exp_month)},
        std::chrono::day{static_cast<unsigned>(record.exp_day)}};
    if (!expiration.ok())
        return std::unexpected(ActivationFailure{ActivationFailure::Code::internal, "activation record is invalid"});
    const auto today = UtcToday();
    if (std::chrono::sys_days{today} > std::chrono::sys_days{expiration})
        return std::unexpected(ActivationFailure{ActivationFailure::Code::expired, "activation code has expired"});

    auto product_info = record.product_info.ToProductInfo();
    if (!product_info)
        return std::unexpected(ActivationFailure{ActivationFailure::Code::internal,
                                                 std::format("stored product info is invalid:{}", product_info.error())});
    SerialInfo serial_info{};
    serial_info.user_name = std::move(record.user_name);
    serial_info.email = std::move(record.email);
    serial_info.hwid = std::string{hwid};
    serial_info.exp_year = static_cast<int>(today.year());
    serial_info.exp_month = static_cast<unsigned>(today.month());
    serial_info.exp_day = static_cast<unsigned>(today.day());
    auto serial_number = GenSerialNumber(*product_info, serial_info);
    if (!serial_number)
        return std::unexpected(ActivationFailure{ActivationFailure::Code::internal,
                                                 std::format("unable to generate daily serial:{}", serial_number.error())});
    return *serial_number;
}

void vmpx::ActivationCodeService::SaveConfig()
{
    auto temp_path = config_path_;
    temp_path += ".tmp";
    try
    {
        std::string yaml;
        struct_yaml::to_yaml(config_, yaml);
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error{"unable to open activation config temporary file"};
        out.write(yaml.data(), static_cast<std::streamsize>(yaml.size()));
        out.flush();
        if (!out) throw std::runtime_error{"unable to write activation config temporary file"};
        out.close();
        if (!out) throw std::runtime_error{"unable to close activation config temporary file"};
#if defined(_WIN32)
        if (!MoveFileExW(temp_path.c_str(), config_path_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "unable to replace activation config");
#else
        std::error_code ec;
        std::filesystem::permissions(temp_path,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) throw std::system_error(ec, "unable to restrict activation config permissions");
        std::filesystem::rename(temp_path, config_path_, ec);
        if (ec) throw std::system_error(ec, "unable to replace activation config");
#endif
    }
    catch (const std::exception& e)
    {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        throw std::runtime_error(std::format("could not save activation config:{}, error:{}",
                                             PathToUtf8(config_path_), e.what()));
    }
}

void vmpx::ActivationCodeService::LoadConfig()
{
    auto data = ReadFile(config_path_);
    if (!data) throw std::runtime_error("unable to read activation config");
    std::string yaml(data->begin(), data->end());
    std::error_code ec;
    struct_yaml::from_yaml(config_, yaml, ec);
    if (ec)
        throw std::runtime_error(std::format("could not parse activation config:{}, error:{}",
                                             PathToUtf8(config_path_), ec.message()));
}
