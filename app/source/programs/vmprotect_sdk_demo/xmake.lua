if is_plat("windows") and is_arch("x64") then
    CreateTarget("vmprotect_sdk_demo", "binary", os.scriptdir(), "program", {}, {}, {}, function()
        add_packages("VMProtectSDK")
    end)
end
