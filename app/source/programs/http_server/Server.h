#pragma once
#include <string_view>
#include <log4cplus/logger.h>

namespace vmpx
{
    constexpr std::string_view DEFAULT_BASE_URL = "/api/v1";

    log4cplus::Logger GetLogger() noexcept;

    void InitNetwork() noexcept;

    /**
     * 
     * @param ip 
     * @param port 
     * @param base_url 
     * @param vmp_console_app_path 如果为empty则不启用App打包服务 
     */
    void StartServer(std::string_view ip, uint16_t port,
                     std::string_view vmp_console_app_path = "",
                     std::string_view base_url = DEFAULT_BASE_URL) noexcept;

    void StopServer() noexcept;
}
