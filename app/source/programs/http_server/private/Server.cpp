#include "../Server.h"

#include <cstdint>

#include <expected>
#include <chrono>

#include <span>
#include <string>

#include <hv/HttpServer.h>
#include <hv/HttpService.h>
#include <hv/hlog.h>
#include <ylt/struct_json/json_reader.h>
#include <ylt/struct_json/json_writer.h>
#include <hv/hasync.h>

#include <log4cplus/log4cplus.h>
#include <magic_enum/magic_enum.hpp>
#include <boost/locale.hpp>
#include "config.h"
#include "AppPackService.h"
#include "ActivationCodeService.h"
#include "Utils.h"
#include "ZipUtils.h"


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
struct CreateActivationCodeRequest
{
    vmpx::ProductInfoEntity product_info;
    vmpx::SerialInfo serial_info;
};

struct ActivateRequest
{
    std::string activation_code;
    std::string hwid;
};

struct ActivationCodeResponse
{
    std::string activation_code;
    int exp_year;
    int exp_month;
    int exp_day;
};
struct RevokeActivationCodeRequest
{
    std::string activation_code;
};

struct ActivationCodeRevokedResponse
{
    bool revoked;
};



namespace
{
    std::atomic_size_t active_pack_requests{};
    std::unique_ptr<vmpx::app_pack::AppPackService> pack_service{nullptr};
    std::unique_ptr<vmpx::ActivationCodeService> activation_service{nullptr};
    std::unique_ptr<hv::HttpServer> server;
    std::unique_ptr<hv::HttpService> http_service;
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
        std::string log_str = std::format(R"("{} {}" {}({}) {}Bytes "{}")",
                                          http_method_str(request->method),
                                          request->url,
                                          static_cast<uint16_t>(response->status_code),
                                          http_status_str(response->status_code),
                                          request->content_length,
                                          request->headers["User-Agent"]);
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
        std::string log_str = std::format(R"("{} {}" {}({}) {}Bytes "{}")",
                                          http_method_str(request->method),
                                          request->url,
                                          static_cast<uint16_t>(response->status_code),
                                          http_status_str(response->status_code),
                                          request->content_length,
                                          request->headers["User-Agent"]);
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

    template <int (*Handler)(const HttpContextPtr&)>
    int HandleAppRequest(const HttpContextPtr& ctx) noexcept
    {
        try
        {
            return Handler(ctx);
        }
        catch (const std::exception&)
        {
            return CtxSendJson(ctx, ErrorEntity{"application request failed"},
                               HTTP_STATUS_INTERNAL_SERVER_ERROR);
        }
        catch (...)
        {
            return CtxSendJson(ctx, ErrorEntity{"application request failed"},
                               HTTP_STATUS_INTERNAL_SERVER_ERROR);
        }
    }

    int OnGenSerialNumber(const HttpContextPtr& ctx) noexcept
    {
        try
        {
            auto& body = ctx->body();
            GenSerialNumberRequest req{};
            {
                auto charset_opt = vmpx::Charset::DetCharset(body);
                if (!charset_opt)
                    return CtxSendJson(ctx, ErrorEntity{"body charset could not be detected"},
                                       HTTP_STATUS_BAD_REQUEST);
                auto utf_body = boost::locale::conv::to_utf<char>(body, *charset_opt);
                std::error_code ec;
                struct_json::from_json(req, utf_body, ec);
                if (ec)
                    return CtxSendJson(ctx, ErrorEntity{"unable to parse JSON request"},
                                       HTTP_STATUS_BAD_REQUEST);
            }
            std::chrono::year_month_day expiration{
                std::chrono::year{req.serial_info.exp_year},
                std::chrono::month{static_cast<unsigned>(req.serial_info.exp_month)},
                std::chrono::day{static_cast<unsigned>(req.serial_info.exp_day)}
            };
            if (req.product_info.key_size == 0 || !expiration.ok())
                return CtxSendJson(ctx, ErrorEntity{"product_info.key_size and a valid expiration date are required"},
                                   HTTP_STATUS_BAD_REQUEST);

            if (req.ignore_network_adapters)
            {
                auto hwid = vmpx::HWID::FromBase64(req.serial_info.hwid);
                if (!hwid)
                    return CtxSendJson(ctx, ErrorEntity{"invalid HWID"},
                                       HTTP_STATUS_BAD_REQUEST);
                hwid->network_adapters.clear();
                req.serial_info.hwid = hwid->ToBase64();
            }
            auto product_info = req.product_info.ToProductInfo();
            if (!product_info)
                return CtxSendJson(ctx, ErrorEntity{std::format("unable to parse product info:{}", product_info.error())},
                                   HTTP_STATUS_BAD_REQUEST);
            auto serial_number_info = vmpx::GenSerialNumber(*product_info, req.serial_info);
            if (!serial_number_info.has_value())
            {
                return CtxSendJson(ctx, ErrorEntity{
                                       .message = std::format("unable to generate serial number with error:{}",
                                                              serial_number_info.error())
                                   }, HTTP_STATUS_BAD_REQUEST);
            }
            return CtxSendJson(ctx, serial_number_info.value());
        }
        catch (const std::exception&)
        {
            return CtxSendJson(ctx, ErrorEntity{"request processing failed"},
                               HTTP_STATUS_INTERNAL_SERVER_ERROR);
        }
    }

    int OnGenRandomProductInfo(const HttpContextPtr& ctx) noexcept
    {
        uint64_t key_size = 0;
        try
        {
            auto& req_json = ctx->json();
            if (!req_json.is_object() || !req_json.contains("key_size") ||
                !req_json["key_size"].is_number_integer())
                return CtxSendJson(ctx, ErrorEntity{"key_size must be an integer"},
                                   HTTP_STATUS_BAD_REQUEST);
            const auto& key_size_json = req_json["key_size"];
            if (key_size_json.is_number_unsigned())
            {
                key_size = key_size_json.get<uint64_t>();
            }
            else
            {
                auto signed_key_size = key_size_json.get<int64_t>();
                if (signed_key_size < 0)
                    return CtxSendJson(ctx, ErrorEntity{"key_size must be positive"},
                                       HTTP_STATUS_BAD_REQUEST);
                key_size = static_cast<uint64_t>(signed_key_size);
            }
            if (key_size < 1024 || key_size > 4096 || key_size % 1024 != 0)
                return CtxSendJson(ctx, ErrorEntity{"key_size must be 1024, 2048, 3072, or 4096"},
                                   HTTP_STATUS_BAD_REQUEST);
        }
        catch (...)
        {
            return CtxSendJson(ctx, ErrorEntity{"unable to parse JSON request"},
                               HTTP_STATUS_BAD_REQUEST);
        }

        try
        {
            auto pi = vmpx::GenRandomProductInfo(static_cast<size_t>(key_size));
            auto pi_entity = vmpx::ProductInfoEntity::FromProductInfo(pi);
            return CtxSendJson(ctx, pi_entity);
        }
        catch (const std::exception&)
        {
            return CtxSendJson(ctx, ErrorEntity{"request processing failed"},
                               HTTP_STATUS_INTERNAL_SERVER_ERROR);
        }
    }
    http_status ActivationHttpStatus(vmpx::ActivationFailure::Code code)
    {
        switch (code)
        {
        case vmpx::ActivationFailure::Code::invalid_request: return HTTP_STATUS_BAD_REQUEST;
        case vmpx::ActivationFailure::Code::not_found: return HTTP_STATUS_NOT_FOUND;
        case vmpx::ActivationFailure::Code::expired: return static_cast<http_status>(410);
        case vmpx::ActivationFailure::Code::internal: return HTTP_STATUS_INTERNAL_SERVER_ERROR;
        }
        return HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }

    int OnCreateActivationCode(const HttpContextPtr& ctx)
    {
        CreateActivationCodeRequest request{};
        std::error_code ec;
        struct_json::from_json(request, ctx->body(), ec);
        if (ec)
            return CtxSendJson(ctx, ErrorEntity{"unable to parse JSON request"},
                               HTTP_STATUS_BAD_REQUEST);
        auto result = activation_service->Create(request.product_info, request.serial_info);
        if (!result)
            return CtxSendJson(ctx, ErrorEntity{result.error().message}, ActivationHttpStatus(result.error().code));
        return CtxSendJson(ctx,
                           ActivationCodeResponse{*result, request.serial_info.exp_year,
                                                  request.serial_info.exp_month, request.serial_info.exp_day},
                           static_cast<http_status>(201));
    }

    int OnActivate(const HttpContextPtr& ctx)
    {
        ActivateRequest request{};
        std::error_code ec;
        struct_json::from_json(request, ctx->body(), ec);
        if (ec)
            return CtxSendJson(ctx, ErrorEntity{"unable to parse JSON request"},
                               HTTP_STATUS_BAD_REQUEST);
        auto result = activation_service->Activate(request.activation_code, request.hwid);
        if (!result)
            return CtxSendJson(ctx, ErrorEntity{result.error().message}, ActivationHttpStatus(result.error().code));
        return CtxSendJson(ctx, *result);
    }

    int OnRevokeActivationCode(const HttpContextPtr& ctx)
    {
        RevokeActivationCodeRequest request{};
        std::error_code ec;
        struct_json::from_json(request, ctx->body(), ec);
        if (ec)
            return CtxSendJson(ctx, ErrorEntity{"unable to parse JSON request"},
                               HTTP_STATUS_BAD_REQUEST);
        auto result = activation_service->Revoke(request.activation_code);
        if (!result)
            return CtxSendJson(ctx, ErrorEntity{result.error().message},
                               ActivationHttpStatus(result.error().code));
        return CtxSendJson(ctx, ActivationCodeRevokedResponse{true});
    }


    int OnAppAdd(const HttpContextPtr& ctx)
    {
        auto& request = ctx->request;
        if (request->content_length > vmpx::ZipExtractionLimits{}.max_archive_bytes)
            return CtxSendJson(ctx, ErrorEntity{"uploaded archive exceeds the maximum compressed size"},
                               HTTP_STATUS_PAYLOAD_TOO_LARGE);
        auto& queries = request->query_params;
        auto name_it = queries.find("name");
        auto vmp_file_path_it = queries.find("vmp_file_path");
        if (name_it == queries.end())
            return CtxSendJson(ctx, ErrorEntity{
                                   "param [name] is required"
                               }, HTTP_STATUS_BAD_REQUEST);
        const auto& name = name_it->second;
        std::filesystem::path vmp_file_path;
        if (vmp_file_path_it != queries.end())
        {
            try
            {
                vmp_file_path = vmpx::PathFromUtf8(vmp_file_path_it->second);
            }
            catch (const std::exception&)
            {
                return CtxSendJson(ctx, ErrorEntity{"vmp_file_path must be valid UTF-8"},
                                   HTTP_STATUS_BAD_REQUEST);
            }
        }

        auto add_result = pack_service->Add(
            name, std::span(static_cast<uint8_t*>(request->Content()),
                            request->content_length), vmp_file_path);
        if (!add_result)
            return CtxSendJson(ctx, ErrorEntity{add_result.error()}, HTTP_STATUS_BAD_REQUEST);
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

    void SendPackedApp(const HttpContextPtr& ctx, const std::string& name)
    {
        if (!pack_service->Has(name))
        {
            CtxSendJson(ctx, ErrorEntity{"unable to find app"}, HTTP_STATUS_BAD_REQUEST);
            return;
        }
        auto packed_app_path = pack_service->GetPacked(name);
        if (packed_app_path.empty())
        {
            auto pack_result = pack_service->Pack(name);
            if (!pack_result)
            {
                CtxSendJson(ctx, ErrorEntity{std::format("unable to pack application:{}", pack_result.error())},
                            HTTP_STATUS_INTERNAL_SERVER_ERROR);
                return;
            }
            packed_app_path = pack_result.value();
        }
        auto utf8_path = vmpx::PathToUtf8(packed_app_path);
        auto utf8_filename = vmpx::PathToUtf8(packed_app_path.filename());
        std::string ascii_filename;
        ascii_filename.reserve(utf8_filename.size());
        for (unsigned char ch : utf8_filename)
            ascii_filename.push_back(ch >= 0x20 && ch <= 0x7e && ch != '"' && ch != '\\' ? ch : '_');
        auto encoded_filename = URLEncode(utf8_filename);
        auto content_disposition = std::format("attachment; filename=\"{}\"; filename*=UTF-8''{}",
                                               ascii_filename, encoded_filename);
        ctx->setHeader("Content-Disposition", content_disposition);
        ctx->setHeader("Content-Type", "application/zip");
#if defined(_WIN32)
        auto send_path = boost::locale::conv::from_utf(utf8_path, "GBK");
#else
        auto send_path = std::move(utf8_path);
#endif
        ctx->sendFile(send_path.c_str());
    }

    int OnAppPack(const HttpContextPtr& ctx)
    {
        const auto& queries = ctx->request->query_params;
        auto name_it = queries.find("name");
        if (name_it == queries.end())
            return CtxSendJson(ctx, ErrorEntity{"param [name] is required"}, HTTP_STATUS_BAD_REQUEST);
        auto name = name_it->second;
        auto active = active_pack_requests.load(std::memory_order_relaxed);
        do
        {
            if (active >= 2)
            {
                ctx->setHeader("Retry-After", "1");
                return CtxSendJson(ctx, ErrorEntity{"pack service is busy; retry later"},
                                   HTTP_STATUS_SERVICE_UNAVAILABLE);
            }
        }
        while (!active_pack_requests.compare_exchange_weak(active, active + 1,
                                                           std::memory_order_acq_rel));
        try
        {
            hv::async([ctx, name = std::move(name)]
            {
                struct ActivePackGuard
                {
                    ~ActivePackGuard()
                    {
                        active_pack_requests.fetch_sub(1, std::memory_order_release);
                    }
                } guard;
                try
                {
                    SendPackedApp(ctx, name);
                }
                catch (const std::exception& e)
                {
                    CtxSendJson(ctx, ErrorEntity{std::format("application pack failed:{}", e.what())},
                                HTTP_STATUS_INTERNAL_SERVER_ERROR);
                }
                catch (...)
                {
                    CtxSendJson(ctx, ErrorEntity{"application pack failed"},
                                HTTP_STATUS_INTERNAL_SERVER_ERROR);
                }
            });
        }
        catch (...)
        {
            active_pack_requests.fetch_sub(1, std::memory_order_release);
            throw;
        }
        return HTTP_STATUS_UNFINISHED;
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
    activation_service = std::make_unique<ActivationCodeService>(data_dir);
    http_service = std::make_unique<HttpService>();
    http_service->AllowCORS();
    http_service->base_url = base_url;
    http_service->Static("/", "./assets/static");
    http_service->POST("/gen_serial_number", OnGenSerialNumber);
    http_service->POST("/gen_random_product_info", OnGenRandomProductInfo);
    http_service->POST("/app/activation_codes/revoke", HandleAppRequest<OnRevokeActivationCode>);
    http_service->POST("/app/activation_codes", HandleAppRequest<OnCreateActivationCode>);
    http_service->POST("/app/activate", HandleAppRequest<OnActivate>);
    // AppPack Service
    if (!vmp_console_app_path.empty())
    {
        pack_service = std::make_unique<app_pack::AppPackService>(
            vmp_console_app_path, data_dir);
        http_service->POST("/app/add", HandleAppRequest<OnAppAdd>);
        http_service->GET("/app/remove", HandleAppRequest<OnAppRemove>);
        http_service->GET("/app/list", HandleAppRequest<OnAppList>);
        http_service->POST("/app/pack", HandleAppRequest<OnAppPack>);
        http_service->GET("/app/product_info", HandleAppRequest<OnGetProductInfo>);
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
