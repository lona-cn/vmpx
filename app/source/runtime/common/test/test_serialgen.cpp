#include <array>
#include <cstdint>
#include <format>
#include <iostream>
#include <print>
#include <span>
#include <stdexcept>

#include "VMPX.h"
int main(int argc, char* argv[])
{
    std::array<uint8_t, 11> short_hwid{};
    if (vmpx::HWID::FromData(std::span<const uint8_t>{short_hwid}))
        return 1;
    if (vmpx::HWID::FromBase64("AQIDBA=="))
        return 2;
    vmpx::ProductInfoEntity invalid_product_info;
    invalid_product_info.modulus = "!";
    if (invalid_product_info.ToProductInfo())
        return 3;
    try
    {
        (void)vmpx::GenRandomProductInfo(1000);
        return 4;
    }
    catch (const std::invalid_argument&)
    {
    }

    auto hwid = vmpx::HWID::FromBase64("eENCrFnwMIMzwzPH3pgmMMInHQUy5rsv7qM52r5jO30=");
    if (!hwid)
        return 5;
    std::cout << std::format("hwid:{}\n", hwid->ToBase64()) << "\n";

    vmpx::SerialInfo serial_info{
        .user_name = "John Doe",
        .email = "john@doe.com",
        .hwid = hwid->ToBase64(),
        .exp_year = 2025,
        .exp_month = 7,
        .exp_day = 11,
    };

    auto rnd_pi = vmpx::GenRandomProductInfo(2048);

    if (auto result = vmpx::GenSerialNumber(rnd_pi, serial_info); result.has_value())
    {
        std::println(std::cout, "serial number:{}", result->serial_number);
    }
    else
    {
        std::cerr << "unable to generate serial number\n";
        return 6;
    }
    return 0;
}

