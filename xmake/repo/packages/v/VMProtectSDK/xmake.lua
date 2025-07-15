package("VMProtectSDK")
set_description("VMProtectSDK")

on_load("@windows|x64", "@windows|x86", function(package)
    package:set("installdir", path.join(os.scriptdir()))
end)

on_fetch("@windows|x64", function(package)
    local result = {}
    result.links = {"VMProtectSDK64", "VMProtectDDK64"}
    result.linkdirs = {package:installdir("lib"), package:installdir("bin")}
    result.includedirs = package:installdir("include")
    return result
end)

on_fetch("@windows|x86", function(package)
    local result = {}
    result.links = {"VMProtectSDK32", "VMProtectDDK32"}
    result.linkdirs = {package:installdir("lib"), package:installdir("bin")}
    result.includedirs = package:installdir("include")
    return result
end)

package_end()
