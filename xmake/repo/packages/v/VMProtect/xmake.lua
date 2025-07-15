package("VMProtect")
set_description("VMProtect")

on_load("@windows|x64", "@windows|x86", function(package)
    package:set("installdir", path.join(os.scriptdir()))
end)

on_fetch("@windows|x64", function(package)
    local result = {}
    result.links = "KeyGen64"
    result.linkdirs = {package:installdir("lib"), package:installdir("bin")}
    result.includedirs = package:installdir("include")
    return result
end)

on_fetch("@windows|x86", function(package)
    local result = {}
    result.links = "KeyGen32"
    result.linkdirs = {package:installdir("lib"), package:installdir("bin")}
    result.includedirs = package:installdir("include")
    return result
end)

package_end()
