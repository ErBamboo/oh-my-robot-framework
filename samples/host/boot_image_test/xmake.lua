--[[
    @file boot_image_test/xmake.lua
    @brief 镜像格式契约 host 测试（头布局/摘要/字段自洽/与主机侧工具对拍）

    运行方式（在框架根执行）：
      xmake g --mingw="<本机 mingw64 sdk 路径>"   -- 每台机器一次；不配则 xmake 会挑中 Git 自带的
                                                 -- mingw64（其中没有 gcc，链接期报 cannot execute 'cc1'）
      xmake f -c -P samples/host/boot_image_test -p mingw -m debug
      xmake build -P samples/host/boot_image_test
      xmake run -P samples/host/boot_image_test host_boot_image_test

    退出码 0=全绿。契约常量与工具产出的期望值见 boot_image_test.c 顶部。
]]

set_project("om_host_boot_image_test")
set_xmakever("3.0.7")
add_rules("mode.debug", "mode.release")

local fw = path.join(os.scriptdir(), "..", "..", "..")

target("host_boot_image_test")
    set_kind("binary")
    set_languages("gnu11")
    set_warnings("all")

    add_includedirs(path.join(fw, "lib/boot/include"))
    add_includedirs(path.join(fw, "lib/algorithm/include"))

    add_files("boot_image_test.c")
    add_files(path.join(fw, "lib/algorithm/src/checksum/crc32.c"))
target_end()
