#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "ActivationCodeService.h"
#include "Utils.h"

int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto data_dir = std::filesystem::temp_directory_path() /
        ("vmpx-activation-test-" + std::to_string(unique));
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{data_dir};

    const auto today = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const auto product = vmpx::GenRandomProductInfo(1024);
    const auto product_entity = vmpx::ProductInfoEntity::FromProductInfo(product);
    vmpx::SerialInfo license{};
    license.user_name = "Activation test";
    license.email = "test@example.com";
    license.exp_year = static_cast<int>(today.year());
    license.exp_month = static_cast<unsigned>(today.month());
    license.exp_day = static_cast<unsigned>(today.day());

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
    if (persisted.empty() || persisted.find(activation_code) != std::string::npos)
        return 2;

    vmpx::ActivationCodeService restarted{data_dir};
    constexpr std::string_view hwid = "eENCrFnwMIMzwzPH3pgmMMInHQUy5rsv7qM52r5jO30=";
    auto activated = restarted.Activate(activation_code, hwid);
    if (!activated || activated->serial_number.empty() ||
        activated->expired_year != license.exp_year ||
        activated->expired_month != license.exp_month ||
        activated->expired_day != license.exp_day)
        return 3;

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
