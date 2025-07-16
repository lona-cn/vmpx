#include "../Server.h"

#include <expected>
#include <span>
#include <string>

#include <hv/HttpServer.h>
#include <hv/HttpService.h>
#include <hv/hlog.h>
#include <ylt/struct_json/json_reader.h>
#include <ylt/struct_json/json_writer.h>
#include <log4cplus/log4cplus.h>
#include <magic_enum/magic_enum.hpp>
#include <boost/locale.hpp>
#include "config.h"
#include "AppPackService.h"
#include "Utils.h"
#include "VMPX.h"


struct ErrorEntity
{
    std::string message;
};

struct GenSerialNumberRequest
{
    vmpx::ProductInfoEntity product_info;
    vmpx::SerialInfo serial_info;
    bool ignore_network_adapters = false;
};

namespace
{
    std::unique_ptr<vmpx::app_pack::AppPackService> pack_service{nullptr};
    std::unique_ptr<hv::HttpServer> server = nullptr;
    thread_local std::string log_buf_string;

    std::string URLEncode(const std::string& value)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex << std::uppercase;

        for (unsigned char c : value)
        {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                escaped << c;
            }
            else
            {
                escaped << '%' << std::setw(2) << int(c);
            }
        }

        return escaped.str();
    }

    template <typename T>
    auto CtxSendJson(const HttpContextPtr& ctx, T&& value, http_status status = HTTP_STATUS_OK) noexcept
    {
        std::string str;
        struct_json::to_json(std::forward<T>(value), str);
        log4cplus::Logger logger = vmpx::GetLogger();
        ctx->setStatus(status);
        auto request = ctx->request;
        auto response = ctx->response;
        std::string log_str = std::format(R"("{} {}" {}({}) {}Bytes "{}" "{}")",
                                          http_method_str(request->method),
                                          request->url,
                                          static_cast<uint16_t>(response->status_code),
                                          http_status_str(response->status_code),
                                          request->content_length,
                                          request->headers["User-Agent"],
                                          str);
        LOG4CPLUS_INFO(logger, LOG4CPLUS_STRING_TO_TSTRING(log_str));
        return ctx->send(str, http_content_type::APPLICATION_JSON);
    }

    auto CtxSendJsonString(const HttpContextPtr& ctx, std::string_view json_string,
                           http_status status = HTTP_STATUS_OK) noexcept
    {
        log4cplus::Logger logger = vmpx::GetLogger();
        ctx->setStatus(status);
        auto request = ctx->request;
        auto response = ctx->response;
        std::string log_str = std::format(R"("{} {}" {}({}) {}Bytes "{}" "{}")",
                                          http_method_str(request->method),
                                          request->url,
                                          static_cast<uint16_t>(response->status_code),
                                          http_status_str(response->status_code),
                                          request->content_length,
                                          request->headers["User-Agent"],
                                          json_string);
        LOG4CPLUS_INFO(logger, LOG4CPLUS_STRING_TO_TSTRING(log_str));
        return ctx->send(std::string(json_string), http_content_type::APPLICATION_JSON);
    }

    void OnHVLog(int loglevel, const char* buf, int len) noexcept
    {
        log4cplus::Logger logger = vmpx::GetLogger();
        log_buf_string.clear();
        // log_buf_string.assign(buf, len);
        log_buf_string.append_range(std::span(buf, len));
        LOG4CPLUS_DEBUG(logger, LOG4CPLUS_STRING_TO_TSTRING(log_buf_string));
    }

    int OnGenSerialNumber(const HttpContextPtr& ctx) noexcept
    {
        try
        {
            auto& body = ctx->body();
            GenSerialNumberRequest req;
            {
                auto charset_opt = vmpx::Charset::DetCharset(body);
                if (!charset_opt)
                    return CtxSendJson(ctx, ErrorEntity{"can not detect body charset"});
                auto utf_body = boost::locale::conv::to_utf<char>(body, *charset_opt);
                std::error_code ec;
                struct_json::from_json(req, utf_body, ec);
                if (ec)
                {
                    return CtxSendJson(ctx, ErrorEntity{
                                           .message = std::format(
                                               "unable to parse json with error: {}\njson string:\n{}",
                                               (ec).message(), utf_body)
                                       }, HTTP_STATUS_BAD_REQUEST);
                }
            }
            if (req.ignore_network_adapters)
            {
                auto hwid = vmpx::HWID::FromBase64(req.serial_info.hwid);
                if (!hwid)
                    return CtxSendJson(ctx, std::format("unable to parse HWID:{}", hwid.error()),
                                       HTTP_STATUS_BAD_REQUEST);
                hwid->network_adapters.clear();
                req.serial_info.hwid = hwid->ToBase64();
            }
            auto serial_number_info = vmpx::GenSerialNumber(req.product_info.ToProductInfo(), req.serial_info);
            if (!serial_number_info.has_value())
            {
                return CtxSendJson(ctx, ErrorEntity{
                                       .message = std::format("unable to generate serial number with error:{}",
                                                              serial_number_info.error())
                                   }, HTTP_STATUS_BAD_REQUEST);
            }
            return CtxSendJson(ctx, serial_number_info.value());
        }
        catch (std::exception& e)
        {
            return CtxSendJson(ctx, ErrorEntity{.message = std::format("unknown error:{}", e.what())},
                               HTTP_STATUS_INTERNAL_SERVER_ERROR);
        }
    }

    int OnGenRandomProductInfo(const HttpContextPtr& ctx) noexcept
    {
        try
        {
            auto& req_json = ctx->json();
            size_t key_size = req_json["key_size"].get<size_t>();
            auto pi = vmpx::GenRandomProductInfo(key_size);
            auto pi_entity = vmpx::ProductInfoEntity::FromProductInfo(pi);
            return CtxSendJson(ctx, pi_entity);
        }
        catch (std::exception& e)
        {
            return CtxSendJson(ctx, ErrorEntity{.message = std::format("unknown error:{}", e.what())},
                               HTTP_STATUS_INTERNAL_SERVER_ERROR);
        }
    }

    int OnAppAdd(const HttpContextPtr& ctx)
    {
        auto& request = ctx->request;
        auto& queries = request->query_params;
        auto name_it = queries.find("name");
        auto vmp_file_path_it = queries.find("vmp_file_path");
        if (name_it == queries.end())
            return CtxSendJson(ctx, ErrorEntity{
                                   "param [name] is required"
                               }, HTTP_STATUS_BAD_REQUEST);
        const auto& name = name_it->second;
        std::filesystem::path vmp_file_path = vmp_file_path_it != queries.end() ? vmp_file_path_it->second : "";

        auto add_result = pack_service->Add(
            name, std::span(static_cast<uint8_t*>(request->Content()),
                            request->content_length), vmp_file_path);
        if (!add_result)
            return CtxSendJson(ctx, ErrorEntity{add_result.error()}, HTTP_STATUS_BAD_REQUEST);
        add_result->vmp_file_path = boost::locale::conv::to_utf<char>(add_result->vmp_file_path, "GBK");
        add_result->packed_app_path = boost::locale::conv::to_utf<char>(add_result->packed_app_path, "GBK");
        return CtxSendJson(ctx, add_result.value());
    }

    int OnAppRemove(const HttpContextPtr& ctx)
    {
        auto& request = ctx->request;
        auto& queries = request->query_params;
        auto name_it = queries.find("name");
        if (name_it == queries.end())
            return CtxSendJson(ctx, ErrorEntity{
                                   "param [name] is required"
                               }, HTTP_STATUS_BAD_REQUEST);
        const auto& name = name_it->second;
        if (pack_service->Remove(name)) return CtxSendJson(ctx, ErrorEntity{"ok"});
        return CtxSendJson(ctx, ErrorEntity{"app not found"}, HTTP_STATUS_NOT_FOUND);
    }

    int OnAppList(const HttpContextPtr& ctx)
    {
        auto app_names = pack_service->List();
        return CtxSendJson(ctx, app_names);
    }

    int OnAppPack(const HttpContextPtr& ctx)
    {
        auto& request = ctx->request;
        auto& queries = request->query_params;
        auto name_it = queries.find("name");
        if (name_it == queries.end())
            return CtxSendJson(ctx, ErrorEntity{
                                   "param [name] is required"
                               }, HTTP_STATUS_BAD_REQUEST);
        const auto& name = name_it->second;
        // 找不到application
        if (!pack_service->Has(name))
            return CtxSendJson(ctx,
                               ErrorEntity{"unable to find app"}, HTTP_STATUS_BAD_REQUEST);
        auto packed_app_path = pack_service->GetPacked(name);
        // 未打包，执行打包程序
        if (packed_app_path.empty())
        {
            auto pack_result = pack_service->Pack(name);
            if (!pack_result)
                return CtxSendJson(ctx, ErrorEntity{std::format("unable to pack application:{}", pack_result.error())},
                                   HTTP_STATUS_INTERNAL_SERVER_ERROR);
            packed_app_path = pack_result.value();
        }
        if (!packed_app_path.empty())
        {
            auto packed_app_path_str = packed_app_path.string();
            auto packed_app_filename_str = packed_app_path.filename().string();
            //TODO 字符集这块
            // auto utf_packed_app_filename_str = boost::locale::conv::to_utf<char>(
            // packed_app_filename_str, vmpx::Charset::DetCharset(packed_app_filename_str).value_or("ASCII"));
            auto utf_packed_app_filename_str = boost::locale::conv::to_utf<char>(
                packed_app_filename_str, "GBK");
            std::string encoded_utf_packed_app_filename_str = URLEncode(utf_packed_app_filename_str);
            std::string content_disposition = std::format("attachment; filename=\"{}\"; filename*=UTF-8''{}",
                                                          packed_app_filename_str, encoded_utf_packed_app_filename_str);
            ctx->setHeader("Content-Disposition", content_disposition);
            ctx->setHeader("Content-Type", "application/zip");
            return ctx->sendFile(packed_app_path_str.c_str());
        }
        return CtxSendJson(ctx, ErrorEntity{"unknown error"},
                           HTTP_STATUS_INTERNAL_SERVER_ERROR);
    }

    int OnGetProductInfo(const HttpContextPtr& ctx)
    {
        auto params = ctx->params();
        auto name_it = params.find("name");
        if (name_it == params.end())
            return CtxSendJson(ctx, ErrorEntity{"param [name] is required"}, HTTP_STATUS_BAD_REQUEST);
        const auto& name = name_it->second;
        auto pi_result = pack_service->GetProductInfo(name);
        if (!pi_result)
            return CtxSendJson(ctx, ErrorEntity{std::format("unable to get product:{}", pi_result.error())},
                               HTTP_STATUS_BAD_REQUEST);
        auto json_str = pi_result->ToJson();
        return CtxSendJsonString(ctx, json_str);
    }
}

log4cplus::Logger vmpx::GetLogger() noexcept
{
    return log4cplus::Logger::getInstance(LOG4CPLUS_TEXT(PROJECT_NAME));
}

void vmpx::InitNetwork() noexcept
{
    log4cplus::Logger logger = vmpx::GetLogger();
    LOG4CPLUS_INFO(logger, LOG4CPLUS_C_STR_TO_TSTRING("init network"));
    hlog_set_handler(OnHVLog);
}

void vmpx::StartServer(std::string_view ip, uint16_t port,
                       std::string_view vmp_console_app_path, std::string_view base_url) noexcept
{
    using namespace hv;
    auto cwd = std::filesystem::current_path();
    auto data_dir = cwd / "data";
    auto http_service = std::make_unique<HttpService>();
    http_service->AllowCORS();
    http_service->base_url = base_url;
    http_service->Static("/", "./assets/static");
    http_service->POST("/gen_serial_number", OnGenSerialNumber);
    http_service->POST("/gen_random_product_info", OnGenRandomProductInfo);
    // AppPack Service
    if (!vmp_console_app_path.empty())
    {
        pack_service = std::make_unique<app_pack::AppPackService>(
            vmp_console_app_path, data_dir);
        http_service->POST("/app/add", OnAppAdd);
        http_service->GET("/app/remove", OnAppRemove);
        http_service->GET("/app/list", OnAppList);
        http_service->POST("/app/pack", OnAppPack);
        http_service->GET("/app/product_info", OnGetProductInfo);
    }
    server = std::make_unique<hv::HttpServer>();
    server->registerHttpService(http_service.get());
    server->run(std::format("{}:{}", ip, port).c_str(), true);
}

void vmpx::StopServer() noexcept
{
    if (server)
    {
        server->stop();
    }
}
