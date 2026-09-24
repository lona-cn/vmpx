package("iguana")
    set_kind("library", {headeronly = true})
    set_homepage("https://github.com/qicosmos/iguana")
    set_description("Universal serialization engine")
    set_license("Apache-2.0")

    add_urls("https://github.com/qicosmos/iguana/archive/refs/tags/$(version).tar.gz")
    add_versions("1.0.9", "b6e3f11a0c37538e84e25397565f5f12b0e6810e582bce7f3ca046425b0b1edf")
    add_deps("frozen")

    on_install(function (package)
        -- MSVC cannot instantiate the C++20 lambda-based member-name reflection.
        -- Keep its existing tuple-based branch while preserving C++20 for other compilers.
        io.replace("iguana/ylt/reflection/member_names.hpp",
            "#if __cplusplus >= 202002L",
            "#if __cplusplus >= 202002L && !defined(_MSC_VER)", {plain = true})
        os.vcp("iguana", package:installdir("include"))
    end)
