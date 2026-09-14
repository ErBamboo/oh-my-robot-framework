--[[
    @file boot_image_test/xmake.lua
    @brief 镜像格式契约 host 测试（头布局/摘要/字段自洽/与主机侧工具对拍）

    运行方式：
      xmake f -c -P oh-my-robot-framework/samples/host/boot_image_test -m debug --mingw="D:/Program Files/ProgramTools/WinGW/w64devkit"
      xmake build -P oh-my-robot-framework/samples/host/boot_image_test
      xmake run -P oh-my-robot-framework/samples/host/boot_image_test host_boot_image_test

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
