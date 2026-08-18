--[[
    @file om_log_test/xmake.lua
    @brief log 服务主机侧测试（无宿主端单元测试框架的最小脚手架，om_core_test 同款手法）

    运行方式（在仓库根）：
      xmake f -c -P samples/host/om_log_test -m debug
      xmake build -P samples/host/om_log_test
      xmake run -P samples/host/om_log_test om_log_formatter_test
      xmake run -P samples/host/om_log_test om_log_filter_test

    退出码 0=通过；非 0=失败（EXPECT 断言不通过）。
    formatter 目标：纯 C 流式格式化器（无 OS 依赖）；
    filter 目标：om_log_log 全链（过滤/临界区/扇出，port 桩 no-op 化临界区）。
]]

set_project("om_log_test")
set_xmakever("3.0.7")
add_rules("mode.debug", "mode.release")

local fw = path.join(os.scriptdir(), "..", "..", "..")
local log_src = path.join(fw, "lib/services/src/log")

target("om_log_formatter_test")
    set_kind("binary")
    set_languages("c11")
    add_includedirs(path.join(fw, "lib/include"))
    add_includedirs(path.join(fw, "lib/services/include"))
    add_includedirs(log_src)
    add_files("om_log_test_common.c", "om_log_formatter_test.c", path.join(log_src, "formatter.c"))
target_end()

target("om_log_filter_test")
    set_kind("binary")
    set_languages("c11")
    add_includedirs(path.join(fw, "lib/include"))
    add_includedirs(path.join(fw, "lib/services/include"))
    add_includedirs(log_src)
    add_files("om_log_test_common.c", "om_log_filter_test.c", "om_log_port_stub.c",
              path.join(log_src, "formatter.c"), path.join(log_src, "core.c"),
              path.join(log_src, "backend.c"))
target_end()
