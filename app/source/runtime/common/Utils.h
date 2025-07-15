#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <boost/locale/encoding_utf.hpp>
#include <boost/locale/util/locale_data.hpp>

namespace vmpx
{
    inline std::tm Cvt2StdTM(int year, int month, int day)
    {
        std::tm time_tm = {};
        time_tm.tm_year = year - 1900; // tm_year 是从 1900 年开始
        time_tm.tm_mon = month - 1; // tm_mon 是从 0 月份开始（0 为 1 月）
        time_tm.tm_mday = day;
        return time_tm;
    }

#if defined(_WIN32)
    using tstring = std::wstring;
#else
    using tstring = std::string;
#endif

    tstring operator""_ts(const char* s, std::size_t len);

    tstring ToTString(std::string_view sv);

    size_t b64declen(const unsigned char* __restrict in, size_t inlen);

    std::wstring U8ToWString(const std::string_view utf8_str);

    bool WriteFile(std::span<const uint8_t> data, const std::filesystem::path& path);

    std::optional<std::vector<uint8_t>> ReadFile(const std::filesystem::path& path);

    [[maybe_unused]] std::vector<wchar_t> U8ToWVec(const std::string& utf8_str);

    /**
     * 
     * @param root 
     * @param filter void function(const std::filesystem::path &filename)
     * @return 
     */
    std::vector<std::filesystem::path> FindFiles(const std::filesystem::path& root,
                                                 const std::function<bool(const std::filesystem::path& filename)>&
                                                 filter);

    class Charset
    {
    public:
        Charset() = delete;
        ~Charset() = delete;
        Charset(const Charset& other) = delete;
        Charset(Charset&& other) noexcept = delete;
        Charset& operator=(const Charset& other) = delete;
        Charset& operator=(Charset&& other) noexcept = delete;

        static std::optional<const char*> DetCharset(std::span<const char> data);

        template <typename T>
        static std::optional<const char*> DetCharset(std::span<const T> data)
        {
            auto sp = std::span(
                reinterpret_cast<const char*>(data.data()),
                data.size_bytes());
            return GetCharset(sp);
        }

        static std::optional<const char*> DetCharset(const std::string& str);
        
    };
}
