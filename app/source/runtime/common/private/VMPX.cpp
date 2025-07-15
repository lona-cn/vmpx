#include "../VMPX.h"

#include <magic_enum.hpp>
#include <ylt/struct_json/json_reader.h>
#include <ylt/struct_json/json_writer.h>
#include <tobiaslocker_base64/base64.hpp>
#include <cryptopp/rsa.h>
#include <cryptopp/osrng.h> // 操作系统随机数生成器
#include <cryptopp/sha.h>
#include <pugixml.hpp>
#include <boost/process.hpp>
#include <boost/asio.hpp>
#include <boost/locale.hpp>

#include "Utils.h"

namespace
{
    template <typename T>
    std::vector<T> B64DecToVec(const byte* str, size_t size)
    {
        auto vec = base64::decode_into<std::vector<
            uint8_t>>(std::string_view(reinterpret_cast<const char*>(str), size));
        return vec;
    }

    template <typename T>
    std::string BytesToB64(std::span<T> data)
    {
        auto bytes = std::span<uint8_t>(reinterpret_cast<uint8_t*>(data.data()), data.size_bytes());
        return BytesToB64(bytes);
    }

    std::string BytesToB64(std::span<uint8_t> bytes)
    {
        return base64::encode_into<std::string>(bytes.begin(), bytes.end());
    }

    // 将 CryptoPP Integer 编码为 std::vector<uint8_t>
    std::vector<uint8_t> Integer2Vec(const CryptoPP::Integer& n)
    {
        size_t encodedSize = n.MinEncodedSize();
        std::vector<uint8_t> out(encodedSize);
        n.Encode(out.data(), encodedSize);
        return out;
    }

    [[maybe_unused]] CryptoPP::Integer Vec2Integer(const std::vector<uint8_t>& vec)
    {
        return {vec.data(), vec.size()};
    }
}

static std::expected<std::string, std::string> GenerateSerialNumber(
    const vmpx::ProductInfo& pi,
    const vmpx::SerialInfo& si);

vmpx::HWID vmpx::HWID::FromData(std::span<uint8_t> bytes) noexcept
{
    auto begin = bytes.begin();
    HWID hwid;
    std::copy(begin, begin + STRIDE, hwid.cpu.begin());
    std::copy(begin + STRIDE, begin + STRIDE * 2, hwid.host.begin());
    std::copy(begin + STRIDE * 2, begin + STRIDE * 3, hwid.hdd.begin());
    const auto num_network_adapters = bytes.size() / STRIDE - 3;
    hwid.network_adapters.reserve(num_network_adapters);
    for (ItemType tmp_item; size_t i : std::views::iota(12llu, bytes.size()) | std::views::stride(STRIDE))
    {
        std::copy(begin + i, begin + i + STRIDE, tmp_item.begin());
        tmp_item[0] -= 2;
        hwid.network_adapters.push_back(tmp_item);
    }
    hwid.cpu[0] -= 0;
    hwid.host[0] -= 1;
    hwid.hdd[0] -= 3;
    return hwid;
}

std::expected<vmpx::HWID, std::string> vmpx::HWID::FromBase64(std::string_view str)
{
    try
    {
        auto vec = B64DecToVec<uint8_t>(reinterpret_cast<const byte*>(str.data()), str.size());
        return FromData<>(std::span{vec});
    }
    catch (std::runtime_error& e)
    {
        return std::unexpected(e.what());
    }
}

auto vmpx::HWID::ToString() const noexcept -> std::string
{
    std::string str = std::format("{:08X}\n{:08X}\n{:08X}",
                                  *reinterpret_cast<const uint32_t*>(cpu.data()),
                                  *reinterpret_cast<const uint32_t*>(host.data()),
                                  *reinterpret_cast<const uint32_t*>(hdd.data()));
    for (const auto& network_adapter : network_adapters)
    {
        str += std::format("\n{:08X}", *reinterpret_cast<const uint32_t*>(network_adapter.data()));
    }
    return str;
}

std::vector<uint8_t> vmpx::HWID::ToBytes() const noexcept
{
    std::vector<uint8_t> bytes{};
    {
        bytes.reserve(STRIDE * 3 + STRIDE * network_adapters.size());
        auto back_it = std::back_inserter(bytes);
        std::copy_n(cpu.cbegin(), STRIDE, back_it);
        std::copy_n(host.cbegin(), STRIDE, back_it);
        std::copy_n(hdd.cbegin(), STRIDE, back_it);
        bytes[STRIDE] += 1;
        bytes[STRIDE * 2] += 3;
        for (const auto& network_adapter : network_adapters)
        {
            std::copy_n(network_adapter.cbegin(), STRIDE, back_it);
            bytes[bytes.size() - 4] += 2;
        }
    }
    return bytes;
}

auto vmpx::HWID::ToBase64() const noexcept -> std::string
{
    auto bytes = ToBytes();
    return BytesToB64(std::span(bytes));
}


std::string vmpx::ProductInfo::ToJson(const ProductInfo& pi) noexcept
{
    auto entity = ProductInfoEntity::FromProductInfo(pi);
    return entity.ToJson();
}

std::expected<vmpx::ProductInfo, std::string> vmpx::ProductInfo::FromJson(const std::string& json) noexcept
{
    auto entity = ProductInfoEntity::FromJson(json);
    if (!entity.has_value())return std::unexpected(entity.error());
    return entity.value().ToProductInfo();
}

std::unique_ptr<VMProtectProductInfo> vmpx::ProductInfo::ToVMP() const noexcept
{
    auto p_pi = std::make_unique<VMProtectProductInfo>();
    p_pi->algorithm = DEFAULT_ALGORITHM;
    p_pi->nBits = this->key_size;
    p_pi->nModulusSize = this->modulus.size();
    p_pi->pModulus = const_cast<byte*>(this->modulus.data());
    p_pi->nPrivateSize = this->private_exponent.size();
    p_pi->pPrivate = const_cast<byte*>(this->private_exponent.data());
    p_pi->nProductCodeSize = this->product_code.size();
    p_pi->pProductCode = const_cast<byte*>(this->product_code.data());
    return p_pi;
}

vmpx::ProductInfoEntity vmpx::ProductInfoEntity::FromProductInfo(const vmpx::ProductInfo& info)
{
    ProductInfoEntity entity;
    entity.key_size = info.key_size;
    entity.modulus = base64::encode_into<std::string>(info.modulus.begin(), info.modulus.end());
    entity.public_exponent = base64::encode_into<std::string>(info.public_exponent.begin(), info.public_exponent.end());
    entity.private_exponent = base64::encode_into<std::string>(info.private_exponent.begin(),
                                                               info.private_exponent.end());
    entity.product_code = base64::encode_into<std::string>(info.product_code.begin(), info.product_code.end());
    return entity;
}

std::expected<vmpx::ProductInfoEntity, std::string> vmpx::ProductInfoEntity::FromJson(const std::string& str) noexcept
{
    ProductInfoEntity entity;
    std::error_code ec;
    struct_json::from_json(entity, str, ec);
    if (ec)
    {
        return std::unexpected(ec.message());
    }
    return entity;
}

std::string vmpx::ProductInfoEntity::ToJson() noexcept
{
    std::string ss;
    struct_json::to_json(*this, ss);
    return ss;
}

vmpx::ProductInfo vmpx::ProductInfoEntity::ToProductInfo() const noexcept
{
    ProductInfo pi;
    pi.key_size = key_size;
    pi.modulus = base64::decode_into<std::vector<byte>>(modulus);
    pi.public_exponent = base64::decode_into<std::vector<byte>>(public_exponent);
    pi.private_exponent = base64::decode_into<std::vector<byte>>(private_exponent);
    pi.product_code = base64::decode_into<std::vector<byte>>(product_code);
    return pi;
}

std::unique_ptr<VMProtectSerialNumberInfo> vmpx::SerialInfo::ToVMP() const noexcept
{
    this->wide_fields->user_name.clear();
    this->wide_fields->email.clear();
    this->wide_fields->user_name = U8ToWVec(this->user_name);
    this->wide_fields->email = U8ToWVec(this->email);
    auto si = std::make_unique<VMProtectSerialNumberInfo>();
    si->flags = HAS_USER_NAME | HAS_EMAIL | HAS_HARDWARE_ID | HAS_EXP_DATE;
    si->pUserName = this->wide_fields->user_name.data();
    si->pEMail = this->wide_fields->email.data();
    si->pHardwareID = const_cast<char*>(this->hwid.data());
    si->dwExpDate = MAKEDATE(exp_year, exp_month, exp_day);
    return si;
}

std::expected<vmpx::SerialNumberInfo, std::string> vmpx::GenSerialNumber(
    const ProductInfo& pi, const SerialInfo& si) noexcept
{
    // auto sn = GenerateSerialNumber(pi, si);
    // if (!sn)
    //     return std::unexpected(sn.error());
    // std::erase(sn.value(), '\n');
    // return SerialNumberInfo{
    //     .serial_number = sn.value(),
    //     .expired_year = si.exp_year, .expired_month = si.exp_month, .expired_day = si.exp_day
    // };
    char* pBuf = nullptr;
    auto vmp_pi = pi.ToVMP();
    auto vmp_si = si.ToVMP();
    auto res = VMProtectGenerateSerialNumber(vmp_pi.get(), vmp_si.get(), &pBuf);
    if (res == ALL_RIGHT)
    {
        auto sni = SerialNumberInfo{
            .serial_number = std::string(pBuf),
            .expired_year = si.exp_year, .expired_month = si.exp_month, .expired_day = si.exp_day
        };
        VMProtectFreeSerialNumberMemory(pBuf);
        std::erase(sni.serial_number, '\n');
        return sni;
    }
    return std::unexpected{std::string(magic_enum::enum_name(res))};
}

vmpx::ProductInfo vmpx::GenRandomProductInfo(size_t key_size, bool random_public_exponent) noexcept
{
    using namespace CryptoPP;
    CryptoPP::AutoSeededRandomPool rng{};
    InvertibleRSAFunction privKeyParams;
    Integer public_exponent = 0x10001; //65537
    //TODO: 支持随机public exponent
    // if (random_public_exponent)
    // {
    //     do
    //     {
    //         public_exponent = Integer(rng, 3, INT32_MAX); // 生成一个小范围的随机整数
    //         if (public_exponent.IsEven()) ++public_exponent; // 确保是奇数
    //     }
    //     while (public_exponent < 3); // 确保在合理范围
    // }
    privKeyParams.Initialize(rng, static_cast<unsigned int>(key_size));
    RSA::PrivateKey privateKey(privKeyParams);
    auto& private_key_exponent = privateKey.GetPrivateExponent();
    auto& public_key_exponent = privateKey.GetPublicExponent();
    auto private_key_exponent_bytes = Integer2Vec(private_key_exponent);
    auto public_key_exponent_bytes = Integer2Vec(public_key_exponent);
    auto& modulus = privateKey.GetModulus();
    std::vector<uint8_t> modulusBytes = Integer2Vec(modulus);
    ProductInfo pi;
    pi.key_size = static_cast<uint32_t>(key_size);
    pi.modulus = modulusBytes;
    pi.public_exponent = public_key_exponent_bytes;
    pi.private_exponent = private_key_exponent_bytes;
    pi.product_code.resize(8);
    rng.GenerateBlock(pi.product_code.data(), pi.product_code.size());
    return pi;
}

std::expected<std::filesystem::path, std::string> vmpx::PackApp(const std::filesystem::path& vmp_console_app_path,
                                                                const std::filesystem::path& vmp_file_path,
                                                                const std::filesystem::path& output_dir)
{
    pugi::xml_document doc;
    if (auto xml_parse_result = doc.load_file(vmp_file_path.c_str()); !xml_parse_result)
    {
        auto error_msg = xml_parse_result.description();
        return std::unexpected(error_msg);
    }
    auto protection_node = doc.child("Document").child("Protection");
    if (protection_node.empty())
    {
        static constexpr std::string_view error_msg = R"(unable to find Node: "Document.Protection")";
        return std::unexpected(std::string(error_msg));
    }
    auto input_file_name_attr = protection_node.attribute("InputFileName");
    if (input_file_name_attr.empty())
    {
        static constexpr std::string_view error_msg = R"(unable to find attribute: "InputFileName")";
        return std::unexpected(std::string(error_msg));
    }
    auto input_file_name = input_file_name_attr.as_string();
    auto app_file_path = std::filesystem::path{vmp_file_path.parent_path().string() + "/" + input_file_name};
    auto app_file_path_name = app_file_path.string();
    auto output_file_name_attr = protection_node.attribute("OutputFileName");
    auto output_dir_tmp = output_dir.empty() ? app_file_path.parent_path() : output_dir;
    std::string output_file_name = output_file_name_attr.empty()
                                       ? output_dir_tmp.string() + "/" + app_file_path.stem().string() + "-vmp" +
                                       app_file_path.extension().string()
                                       : output_dir_tmp.string() + "/" + output_file_name_attr.as_string();
    boost::asio::io_context ctx;
    boost::asio::readable_pipe rp{ctx};
    output_file_name = boost::locale::conv::from_utf(output_file_name, "GBK");
    auto proc = boost::process::process{
        ctx, vmp_console_app_path.string(),
        {boost::locale::conv::to_utf<char>(vmp_file_path.string(), "GBK"), output_file_name},
        boost::process::process_stdio{{}, rp, {}}
    };
    std::string sub_proc_stdout_buf;
    sub_proc_stdout_buf.reserve(4096);
    boost::system::error_code ec;
    boost::asio::read(rp, boost::asio::dynamic_buffer(sub_proc_stdout_buf), ec);
    if (ec && ec.value() != 109)
    {
        auto error_msg = std::format("subprocess io exception occurred:{}", ec.message());
        return std::unexpected(error_msg);
    }
    proc.wait();
    if (sub_proc_stdout_buf.rfind("Compilation completed") != std::string::npos)
    {
        return std::filesystem::path(output_file_name);
    }
    return std::unexpected(std::format("pack failed,stdout log:\n", sub_proc_stdout_buf));
}


enum class SerialNumberChunks:uint8_t
{
    SERIAL_CHUNK_VERSION = 0x01, //	1 byte of data - version
    SERIAL_CHUNK_USER_NAME = 0x02, //	1 + N bytes - length + N bytes of customer's name (without enging \0).
    SERIAL_CHUNK_EMAIL = 0x03, //	1 + N bytes - length + N bytes of customer's email (without ending \0).
    SERIAL_CHUNK_HWID = 0x04, //	1 + N bytes - length + N bytes of hardware id (N % 4 == 0)
    SERIAL_CHUNK_EXP_DATE = 0x05, //	4 bytes - (year << 16) + (month << 8) + (day)
    SERIAL_CHUNK_RUNNING_TIME_LIMIT = 0x06, //	1 byte - number of minutes
    SERIAL_CHUNK_PRODUCT_CODE = 0x07, //	8 bytes - used for decrypting some parts of exe-file
    SERIAL_CHUNK_USER_DATA = 0x08, //	1 + N bytes - length + N bytes of user data
    SERIAL_CHUNK_MAX_BUILD = 0x09, //	4 bytes - (year << 16) + (month << 8) + (day)

    SERIAL_CHUNK_END = 0xFF //	4 bytes - checksum: the first four bytes of sha-1 hash from the data before that chunk
};

static std::vector<byte> SHA1Hash(const std::vector<byte>& data)
{
    using namespace CryptoPP;
    SHA1 sha;
    std::vector<byte> digest(SHA1::DIGESTSIZE);
    sha.CalculateDigest(digest.data(), data.data(), data.size());
    return digest;
}

/**
 * TODO：需要修，目前还不能用
 * @param pi 
 * @param si 
 * @return 
 */
static std::expected<std::string, std::string> GenerateSerialNumber(
    const vmpx::ProductInfo& pi,
    const vmpx::SerialInfo& si)
{
    // if (algorithm_ == alNone)
    // throw std::runtime_error(language[lsLicensingParametersNotInitialized]);

    std::vector<uint8_t> data;

    data.push_back(static_cast<uint8_t>(SerialNumberChunks::SERIAL_CHUNK_VERSION));
    data.push_back(0x01);

    if (!si.user_name.empty())
    {
        data.push_back(static_cast<uint8_t>(SerialNumberChunks::SERIAL_CHUNK_USER_NAME));
        if (si.user_name.size() > 255)
            return std::unexpected("user name too long");
        data.push_back(static_cast<uint8_t>(si.user_name.size()));
        data.insert(data.end(), si.user_name.begin(), si.user_name.end());
    }

    if (!si.email.empty())
    {
        data.push_back(static_cast<uint8_t>(SerialNumberChunks::SERIAL_CHUNK_EMAIL));
        if (si.email.size() > 255)
            return std::unexpected("email too long");
        data.push_back(static_cast<uint8_t>(si.email.size()));
        data.insert(data.end(), si.email.begin(), si.email.end());
    }

    if (!si.hwid.empty())
    {
        data.push_back(static_cast<uint8_t>(SerialNumberChunks::SERIAL_CHUNK_HWID));
        auto hwid_bytes_vec = B64DecToVec<
            uint8_t>(reinterpret_cast<const byte*>(si.hwid.c_str()), si.hwid.size());
        if (!hwid_bytes_vec.size() || hwid_bytes_vec.size() > 255 || hwid_bytes_vec.size() % 4 != 0)
            return std::unexpected("invalid hwid");
        data.push_back(static_cast<uint8_t>(hwid_bytes_vec.size()));
        data.insert(data.end(), hwid_bytes_vec.begin(), hwid_bytes_vec.end());
    }

    if (si.exp_year != 0)
    {
        data.push_back(static_cast<uint8_t>(SerialNumberChunks::SERIAL_CHUNK_EXP_DATE));
        uint32_t expire_date = (si.exp_year << 16) | (si.exp_month << 8) | si.exp_month;
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&expire_date),
                    reinterpret_cast<const uint8_t*>(&expire_date) + sizeof(expire_date));
    }

    // TODO: 支持时间限制
    // if (info.Flags & HAS_TIME_LIMIT)
    // {
    //     data.PushByte(SERIAL_CHUNK_RUNNING_TIME_LIMIT);
    //     data.PushByte(info.RunningTimeLimit);
    // }

    if (pi.product_code.size() != 8)
        return std::unexpected("invalid product code");

    data.push_back(static_cast<uint8_t>(SerialNumberChunks::SERIAL_CHUNK_PRODUCT_CODE));
    data.insert(data.end(), pi.product_code.begin(), pi.product_code.end());

    //TODO :支持UserData
    // if (info.Flags & HAS_USER_DATA)
    // {
    //     data.PushByte(SERIAL_CHUNK_USER_DATA);
    //     if (info.UserData.size() > 255)
    //         throw std::runtime_error(language[lsUserDataTooLong]);
    //     data.PushByte((uint8_t)info.UserData.size());
    //     data.PushBuff(info.UserData.c_str(), info.UserData.size());
    // }

    // TODO: 支持 MAX_BUILD_DATE
    // if (info.Flags & HAS_MAX_BUILD_DATE)
    // {
    //     data.PushByte(SERIAL_CHUNK_MAX_BUILD);
    //     data.PushDWord(info.MaxBuildDate.value());
    // }

    using namespace CryptoPP;
    // compute hash
    {
        auto hash = SHA1Hash(data);
        data.push_back(static_cast<uint8_t>(SerialNumberChunks::SERIAL_CHUNK_END)); // End chunk
        for (auto& item : hash | std::views::reverse)
        {
            data.push_back(item);
        }
    }

    // add padding
    {
        size_t min_padding = 8 + 3;
        size_t max_padding = min_padding + 16;
        size_t max_bytes = pi.key_size / 8;
        if (data.size() + min_padding > max_bytes)
            return std::unexpected("serial number too long");
        AutoSeededRandomPool rng;
        // srand(os::GetTickCount());
        size_t padding_bytes = min_padding + static_cast<int>(rng.GenerateWord32()) % (max_padding - min_padding);
        data.insert_range(data.begin(), std::span(data.data(), padding_bytes));
        data[0] = 0;
        data[1] = 2;
        data[padding_bytes - 1] = 0;
        for (size_t i = 2; i < padding_bytes - 1; i++)
        {
            uint8_t b = 0;
            while (!b)
            {
                b = rng.GenerateByte();
            }
            data[i] = b;
        }
        while (data.size() < max_bytes)
        {
            data.push_back(rng.GenerateByte());
        }
    }

    std::vector<byte> encrypted;
    // 加载 RSA key
    {
        AutoSeededRandomPool rng;
        Integer private_exp = Vec2Integer(pi.private_exponent);
        Integer public_exp = Vec2Integer(pi.public_exponent);
        Integer modulus = Vec2Integer(pi.modulus);
        RSA::PrivateKey private_key;
        private_key.Initialize(modulus, public_exp, private_exp); // PublicExp is unused here
        Integer m(data.data(), data.size());
        Integer c = a_exp_b_mod_c(m, private_exp, modulus);
        encrypted.resize(c.MinEncodedSize());
        c.Encode(encrypted.data(), encrypted.size());
        if (encrypted.empty())
            return std::unexpected("invalid encrypted data or serial number too long");
    }
    auto encoded = BytesToB64(encrypted);
    return encoded;
}
