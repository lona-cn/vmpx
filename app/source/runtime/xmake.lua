IncludeSubDirs(os.scriptdir())

-- runtime target
target("runtime")
    set_kind("object")
    set_group("runtime")
    add_extrafiles(path.join(os.scriptdir(),"xmake.lua"))
    add_files("private/dummy.cpp")
    local deps = {"common"}
    for _,dep in ipairs(deps) do 
        add_deps(dep,{inherit = true})
    end
    set_policy("build.merge_archive", true)
target_end()