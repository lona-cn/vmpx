#if defined(_WIN32)
#include <windows.h>
#endif
#include <csignal>
#include <iostream>
#include <filesystem>
#include <log4cplus/log4cplus.h>
#include <argparse/argparse.hpp>
#include "config.h"
#include "Server.h"
#include "VMPX.h"

namespace
{
    std::atomic_bool exited{false};

    void OnExit()
    {
        if (exited.exchange(true))return;
        log4cplus::Logger logger = vmpx::GetLogger();
        LOG4CPLUS_INFO(logger, LOG4CPLUS_STRING_TO_TSTRING("stopping server..."));
        vmpx::StopServer();
    }

    void SignalHandler(int sig)
    {
        OnExit();
    }

    void RegisterExitHandler()
    {
        std::atexit(OnExit);
        std::at_quick_exit(OnExit);
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGILL, SignalHandler);
        std::signal(SIGFPE, SignalHandler);
        std::signal(SIGSEGV, SignalHandler);
        std::signal(SIGTERM, SignalHandler);
        std::signal(SIGBREAK, SignalHandler);
        std::signal(SIGABRT, SignalHandler);
        std::signal(SIGABRT_COMPAT, SignalHandler);
    }

    void InitLogger()
    {
        if (!std::filesystem::exists("logs"))
        {
            std::filesystem::create_directory("logs");
        }
        log4cplus::initialize();
        log4cplus::PropertyConfigurator::doConfigure(LOG4CPLUS_STRING_TO_TSTRING("config/log4cplus.properties"));
        log4cplus::Logger logger = vmpx::GetLogger();
        LOG4CPLUS_INFO(logger,
                       LOG4CPLUS_STRING_TO_TSTRING(std::format("VERSION:{} GIT_COMMIT:{}",VERSION,GIT_COMMIT)));
    }

    struct Arguments
    {
        std::string ip;
        uint16_t port;
        std::string vmp_console_app_path;
    };

    Arguments ParseArguments(int argc, char* argv[])
    {
        auto argument_parser =
            std::make_unique<argparse::ArgumentParser>(PROJECT_NAME);
        argument_parser->add_argument("ip").default_value("0.0.0.0")
                       .help("ip address,default: 0.0.0.0");
        argument_parser->add_argument("port").default_value(static_cast<uint16_t>(80))
                       .help("port number,default 80").scan<'i', uint16_t>();
        argument_parser->add_argument("vmp_console_app_path").default_value("")
                       .help("vmp_console_app_path: VMProtect_Con.exe");
        argument_parser->parse_args(argc, argv);
        Arguments arguments{
            .ip = argument_parser->get<std::string>("ip"),
            .port = argument_parser->get<uint16_t>("port"),
            .vmp_console_app_path = argument_parser->get<std::string>("vmp_console_app_path")
        };
        return arguments;
    }
}

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
#endif
    InitLogger();
    RegisterExitHandler();
    try
    {
        auto arguments = ParseArguments(argc, argv);
        vmpx::InitNetwork();
        vmpx::StartServer(arguments.ip, arguments.port,
                          arguments.vmp_console_app_path);
    }
    catch (std::runtime_error& e)
    {
        LOG4CPLUS_ERROR(vmpx::GetLogger(), LOG4CPLUS_STRING_TO_TSTRING(e.what()));
        return 1;
    }
    LOG4CPLUS_INFO(vmpx::GetLogger(), LOG4CPLUS_C_STR_TO_TSTRING("application exit"));
    return 0;
}
