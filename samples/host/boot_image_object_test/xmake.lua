--[[
    @file boot_image_object_test/xmake.lua
    @brief 镜像对象与校验器 host 测试（详细状态、对象身份、算法注入）

    运行方式（在框架根执行）：
      xmake f -c -P samples/host/boot_image_object_test -p mingw -m debug
      xmake build -P samples/host/boot_image_object_test
      xmake run -P samples/host/boot_image_object_test host_boot_image_object_test

    退出码 0=全绿。器件仿真与 osal 桩复用同级 flash_dev_test 基础设施。
]]

set_project("om_host_boot_image_object_test")
set_xmakever("3.0.7")
add_rules("mode.debug", "mode.release")

local fw = path.join(os.scriptdir(), "..", "..", "..")
local fdt = path.join(os.scriptdir(), "..", "flash_dev_test")

target("host_boot_image_object_test")
    set_kind("binary")
    set_languages("gnu11")
    set_warnings("all")

    add_includedirs(os.scriptdir())
    add_includedirs(fdt)
    add_includedirs(path.join(fw, "lib/include"))
    add_includedirs(path.join(fw, "lib/boot/include"))
    add_includedirs(path.join(fw, "lib/algorithm/include"))
    add_includedirs(path.join(fw, "lib/data_struct/include"))
    add_includedirs(path.join(fw, "lib/drivers/include"))

    add_files("boot_image_object_test.c")
    add_files(path.join(fdt, "host_osal.c"))
    add_files(path.join(fdt, "flash_sim.c"))
    add_files(path.join(fw, "lib/boot/src/image.c"))
    add_files(path.join(fw, "lib/boot/src/image_verifier/image_verifier.c"))
    add_files(path.join(fw, "lib/boot/src/image_verifier/algorithm/crc32.c"))
    add_files(path.join(fw, "lib/algorithm/src/checksum/crc32.c"))
    add_files(path.join(fw, "lib/drivers/src/storage/partition.c"))
    add_files(path.join(fw, "lib/drivers/src/peripheral/flash/hal_flash.c"))
    add_files(path.join(fw, "lib/drivers/src/model/device.c"))

    if is_plat("linux") then
        add_syslinks("pthread")
    end
target_end()
