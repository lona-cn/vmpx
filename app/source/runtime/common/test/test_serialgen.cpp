#include <assert.h>
#include <iostream>

#include "VMPX.h"
#define assertm(exp, msg) assert((void(msg), exp))

int main(int argc, char* argv[])
{
    auto hwid = vmpx::HWID::FromBase64("eENCrFnwMIMzwzPH3pgmMMInHQUy5rsv7qM52r5jO30=");
    if (hwid.has_value())
    {
        std::cout << std::format("hwid:{}\n", hwid->ToBase64()) << "\n";
    }
    else
    {
        std::cerr << "unable to convert hwid from base64" << "\n";
        return -1;
    }

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
        std::println(std::cout, "serial number:{}", *result);
    }
    else
    {
        assertm(false, "unable to generate serial number");
    }
    return 0;
}
