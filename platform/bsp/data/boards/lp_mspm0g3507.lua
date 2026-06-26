--- @file oh_my_robot/platform/bsp/data/boards/lp_mspm0g3507.lua
--- @brief LP_MSPM0G3507 板级数据
--- @details TI LaunchPad LP_MSPM0G3507 — MSPM0G3507 + XDS110 板载调试器。

--- 板级数据结构
---@class BspBoard
---@field name string 板级名称
---@field chip string 芯片名称
---@field vendor string 供应商名称
---@field defines string[] 预处理宏
---@field includedirs string[] 头文件目录
---@field sources string[] 源文件路径
---@field override_sources string[] 强覆盖源文件（直接并入 binary）
---@field osal table<string, string> OS 配置路径映射
---@field startup table<string, string> 启动文件映射
---@field linkerscript table<string, string> 链接脚本映射
---@field components string[] 组件白名单
---@field component_overrides table<string, table> 组件覆盖配置
local board = {
    name = "lp-mspm0g3507",
    chip = "mspm0g3507",
    vendor = "ti",
    defines = {},
    includedirs = {
        "boards/lp-mspm0g3507/include",
    },
    sources = {
        "boards/lp-mspm0g3507/source/bsp_cpu.c",
        "boards/lp-mspm0g3507/source/port/om_port_hw.c",
    },
    override_sources = {},
    osal = {
        freertos = "boards/lp-mspm0g3507/osal/freertos",
    },
    startup = {},
    linkerscript = {},
    components = {
        "device",
    },
    component_overrides = {},
}

--- 获取板级数据
---@return BspBoard
function get()
    return board
end
