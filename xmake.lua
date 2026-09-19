add_rules("mode.debug", "mode.release")
set_languages("cxx17")

local plugin_version = os.getenv("PLUGIN_VERSION") or "dev"

-- The CS:GO dedicated server is 32-bit on both platforms, so the plugin must be
-- too: xmake f -p windows -a x86   /   xmake f -p linux -a i386
target("csgo-multi-appid")
    set_kind("shared")
    set_symbols("debug", "embed")

    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    add_includedirs("src")
    add_defines("PLUGIN_VERSION=\"" .. plugin_version .. "\"")

    -- No SDK: the one engine interface this plugin implements is mirrored in
    -- plugin.cpp, and tier0's Msg/Warning are resolved at runtime.
    if is_plat("windows") then
        set_runtimes("MT")
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS")
        add_syslinks("ws2_32")
    else
        -- Valve loads plugins as <name>.so, without the lib prefix.
        set_prefixname("")
        -- _GNU_SOURCE for RTLD_NOLOAD; g++ defines it already, but be explicit.
        add_defines("_GNU_SOURCE")
        add_cxflags("-m32", "-fPIC", {force = true})
        add_ldflags("-m32", {force = true})
        add_syslinks("dl")
    end

-- Self-check for the pattern matcher, the one piece of logic here that can be
-- wrong in a way the compiler will not catch. Not built by default:
--   xmake build sigtest && xmake run sigtest
target("sigtest")
    set_kind("binary")
    set_default(false)
    add_files("test/sig_test.cpp", "src/platform.cpp")
    add_includedirs("src")
    if is_plat("windows") then
        set_runtimes("MT")
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS")
        add_syslinks("ws2_32")
    else
        add_defines("_GNU_SOURCE")
        add_cxflags("-m32", {force = true})
        add_ldflags("-m32", {force = true})
        add_syslinks("dl")
    end
