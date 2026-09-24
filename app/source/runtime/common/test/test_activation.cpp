#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <format>
#include <string>

#include "ActivationCodeService.h"
#include "Utils.h"

int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto test_root = std::filesystem::temp_directory_path() /
        ("vmpx-activation-test-" + std::to_string(unique));
    const auto data_dir = test_root / "active";
    const auto expired_dir = test_root / "expired";
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{test_root};

    const auto today = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const auto expiration = std::chrono::year_month_day{
        std::chrono::sys_days{today} + std::chrono::days{1}};
    const auto product = vmpx::GenRandomProductInfo(1024);
    const auto product_entity = vmpx::ProductInfoEntity::FromProductInfo(product);
    vmpx::SerialInfo license{};
    license.user_name = "Activation test";
    license.email = "test@example.com";
    license.exp_year = static_cast<int>(expiration.year());
    license.exp_month = static_cast<unsigned>(expiration.month());
    license.exp_day = static_cast<unsigned>(expiration.day());

    std::string activation_code;
    {
        vmpx::ActivationCodeService service{data_dir};
        auto created = service.Create(product_entity, license);
        if (!created) return 1;
        activation_code = *created;
    }

    std::ifstream config(data_dir / "activation_codes.yml", std::ios::binary);
    if (!config) return 2;
    const std::string persisted{std::istreambuf_iterator<char>{config}, {}};
    config.close();
    if (persisted.empty() || persisted.find(activation_code) != std::string::npos)
        return 2;

    vmpx::ActivationCodeService restarted{data_dir};
    constexpr std::string_view hwid = "eENCrFnwMIMzwzPH3pgmMMInHQUy5rsv7qM52r5jO30=";
    auto activated = restarted.Activate(activation_code, hwid);
    if (!activated || activated->serial_number.empty() ||
        activated->expired_year != static_cast<int>(today.year()) ||
        activated->expired_month != static_cast<unsigned>(today.month()) ||
        activated->expired_day != static_cast<unsigned>(today.day()))
        return 3;
    auto revoked = restarted.Revoke(activation_code);
    if (!revoked) return 6;
    auto after_revoke = restarted.Activate(activation_code, hwid);
    if (after_revoke || after_revoke.error().code != vmpx::ActivationFailure::Code::not_found)
        return 7;
    vmpx::ActivationCodeService after_revoke_restart{data_dir};
    auto still_revoked = after_revoke_restart.Activate(activation_code, hwid);
    if (still_revoked || still_revoked.error().code != vmpx::ActivationFailure::Code::not_found)
        return 8;

    std::filesystem::create_directories(expired_dir);
    auto expired_persisted = persisted;
    const auto expiration_field = std::format("exp_year: {}", license.exp_year);
    const auto expiration_pos = expired_persisted.find(expiration_field);
    if (expiration_pos == std::string::npos) return 9;
    expired_persisted.replace(expiration_pos, expiration_field.size(), "exp_year: 2000");
    const auto expired_config_path = expired_dir / "activation_codes.yml";
    {
        std::ofstream expired_output(expired_config_path, std::ios::binary | std::ios::trunc);
        expired_output.write(expired_persisted.data(),
                             static_cast<std::streamsize>(expired_persisted.size()));
        if (!expired_output) return 10;
    }
    vmpx::ActivationCodeService pruned{expired_dir};
    auto expired_activation = pruned.Activate(activation_code, hwid);
    if (expired_activation ||
        expired_activation.error().code != vmpx::ActivationFailure::Code::not_found)
        return 11;
    vmpx::ActivationCodeService pruned_restart{expired_dir};
    auto still_pruned = pruned_restart.Activate(activation_code, hwid);
    if (still_pruned ||
        still_pruned.error().code != vmpx::ActivationFailure::Code::not_found)
        return 12;

    auto invalid_code = activation_code;
    invalid_code.back() = invalid_code.back() == 'A' ? 'B' : 'A';
    auto unknown = restarted.Activate(invalid_code, hwid);
    if (unknown || unknown.error().code != vmpx::ActivationFailure::Code::not_found)
        return 4;
    auto invalid_hwid = restarted.Activate(activation_code, "invalid");
    if (invalid_hwid || invalid_hwid.error().code != vmpx::ActivationFailure::Code::invalid_request)
        return 5;
    return 0;
}
