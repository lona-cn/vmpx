local target_name = "vmpx_server"
local kind = "binary"
local group_name = "program"
local pkgs = { "log4cplus", "libhv", "yalantinglibs", "argparse", "libzip", "utfcpp", "uchardet" }
local deps = { "runtime" }
local syslinks = {}
local function callback()
end
CreateTarget(target_name, kind, os.scriptdir(), group_name, pkgs, deps, syslinks, callback)
