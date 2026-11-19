--[[
    @file boot_meta_store_test/xmake.lua
    @brief 启动状态记录（meta）存储 host 测试（双份轮转、擦除与掉电破坏）

    运行方式（在框架根执行）：
      xmake g --mingw="<本机 mingw64 sdk 路径>"
      xmake f -c -P samples/host/boot_meta_store_test -p mingw -m debug
      xmake build -P samples/host/boot_meta_store_test
      xmake run -P samples/host/boot_meta_store_test host_boot_meta_store_test

    退出码 0=全绿。器件仿真与 osal 桩复用同级 flash_dev_test 基础设施。
]]

set_project("om_host_boot_meta_store_test")
set_xmakever("3.0.7")
add_rules("mode.debug", "mode.release")

local fw = path.join(os.scriptdir(), "..", "..", "..")
local fdt = path.join(os.scriptdir(), "..", "flash_dev_test")

target("host_boot_meta_store_test")
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

    add_files("boot_meta_store_test.c")
    add_files(path.join(fdt, "host_osal.c"))
    add_files(path.join(fdt, "flash_sim.c"))
    add_files(path.join(fw, "lib/boot/src/meta_contract.c"))
    add_files(path.join(fw, "lib/boot/src/meta.c"))
    add_files(path.join(fw, "lib/algorithm/src/checksum/crc32.c"))
    add_files(path.join(fw, "lib/drivers/src/storage/partition.c"))
    add_files(path.join(fw, "lib/drivers/src/peripheral/flash/hal_flash.c"))
    add_files(path.join(fw, "lib/drivers/src/model/device.c"))

    if is_plat("linux") then
        add_syslinks("pthread")
    end
target_end()
