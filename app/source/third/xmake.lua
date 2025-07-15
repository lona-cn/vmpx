IncludeSubDirs(os.scriptdir())
add_requires("log4cplus", "libhv", "VMProtect", "VMProtectSDK", "yalantinglibs", "tobiaslocker_base64", "cryptopp",
    "magic_enum", "utfcpp", "argparse", "pugixml", "boost", "libzip", "uchardet")

add_requireconfs("boost",
    {
        configs = {
            process = true,
            filesystem = true,
            asio = true,
            exception = true,
            regex = true,
            context = true,
            locale = true
        }
    })

add_requireconfs("libzip",
    {
        configs = {
            bzip2 = true,
            zstd = true,
            lzma = true
        }
    }
)
