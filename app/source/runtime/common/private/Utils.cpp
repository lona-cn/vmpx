#include "Utils.h"

#include <fstream>
#include <iostream>
#include <utf8cpp/utf8.h>
#include <boost/locale.hpp>
#include <uchardet.h>

namespace
{
    thread_local uchardet_t ud = uchardet_new();
}

vmpx::tstring vmpx::operator ""_ts(const char* s, std::size_t len)
{
#if defined(_WIN32)
    return std::wstring(s, s + len);
#else
        return std::string(s, len);
#endif
}

[[maybe_unused]] std::wstring vmpx::U8ToWString(const std::string_view utf8_str)
{
    std::wstring result;

#if defined(_WIN32)
    [[likely]]
#endif
    if (sizeof(wchar_t) == 2)
    {
        std::u16string utf16;
        utf8::utf8to16(utf8_str.begin(), utf8_str.end(), std::back_inserter(utf16));
        result.assign(utf16.begin(), utf16.end());
    }
#if !defined(_WIN32)
    [[likely]]
#endif
    else if (sizeof(wchar_t) == 4)
    {
        std::u32string utf32;
        utf8::utf8to32(utf8_str.begin(), utf8_str.end(), std::back_inserter(utf32));
        result.assign(utf32.begin(), utf32.end());
    }

    return result;
}

vmpx::tstring vmpx::ToTString(std::string_view sv)
{
#if defined(_WIN32)
    return U8ToWString(sv);
#else
        // 其他平台直接用 std::string 构造
        return tstring(sv);
#endif
}

size_t vmpx::b64declen(const unsigned char* in, size_t inlen)
{
    if (!inlen || (inlen & 3)) return 0;

    size_t outlen = (inlen / 4) * 3;
    const unsigned char* ip = in + inlen;
    if (ip[-1] != '=');
    else if (ip[-2] != '=') outlen -= 1;
    else if (ip[-3] != '=') outlen -= 2;
    else outlen -= 3;
    return outlen;
}

bool vmpx::WriteFile(std::span<const uint8_t> data, const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec); // 忽略失败也行

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open())
        return false;

    try
    {
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    catch (std::exception& e)
    {
        std::cout << e.what() << std::endl;
    }

    return ofs.good();
}

std::optional<std::vector<uint8_t>> vmpx::ReadFile(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path) || std::filesystem::is_directory(path))
        return std::nullopt;
    auto size = std::filesystem::file_size(path);
    std::vector<uint8_t> buffer(size);
    std::ifstream ifs(path, std::ios::binary);
    ifs.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

std::vector<wchar_t> vmpx::U8ToWVec(const std::string& utf8_str)
{
    std::vector<wchar_t> output;

    if (sizeof(wchar_t) == 2)
    {
        std::u16string utf16;
        utf8::utf8to16(utf8_str.begin(), utf8_str.end(), std::back_inserter(utf16));
        output.assign(utf16.begin(), utf16.end());
    }
    else if (sizeof(wchar_t) == 4)
    {
        std::u32string utf32;
        utf8::utf8to32(utf8_str.begin(), utf8_str.end(), std::back_inserter(utf32));
        output.assign(utf32.begin(), utf32.end());
    }
    else
    {
        throw std::runtime_error("Unsupported wchar_t size");
    }

    output.push_back(L'\0'); // 保留结尾的空字符
    return output;
}

std::vector<std::filesystem::path> vmpx::FindFiles(const std::filesystem::path& root,
                                                   const std::function<bool(const std::filesystem::path& filename)>&
                                                   filter)
{
    std::vector<std::filesystem::path> files;

    if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root))
    {
        std::cerr << "Invalid root path: " << root << '\n';
        return files;
    }

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec)
        {
            std::cerr << "Warning: " << ec.message() << " in " << it->path() << '\n';
            continue;
        }

        if (it->is_regular_file(ec) && filter(it->path().filename()))
        {
            files.push_back(it->path());
        }
    }

    return files;
}

std::optional<const char*> vmpx::Charset::DetCharset(std::span<const char> data)
{
    uchardet_reset(ud);
    if (uchardet_handle_data(ud, data.data(), data.size_bytes()) != 0)
        return std::nullopt;
    uchardet_data_end(ud);
    return uchardet_get_charset(ud);
}

std::optional<const char*> vmpx::Charset::DetCharset(const std::string& str)
{
    return DetCharset(std::span(str.data(), str.size()));
}
