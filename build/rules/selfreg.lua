--- @file oh_my_robot/build/rules/selfreg.lua
--- @brief OM 自注册模块直接注入规则
--- @details 把框架自注册模块源（lib/systems/src、lib/services/src 下的全部 .c）直接编译进
---          binary，保证 OM_INIT 自注册 entry 不被静态库按需抽取丢弃。
---
---          原理：直接编译产生的 .o 先于静态归档被链接，已定义模块符号；链接器随后处理
---          归档时，该成员因符号已解析而不再被抽取，故无需从静态库剔除源文件，也不会产生
---          重复符号。这是 Linux/Zephyr "全对象链接（obj-y）" 目标在 XMake 静态归档模型下的
---          等价实现（XMake 无干净的 per-dep --whole-archive 入口，见 ADR-0010）。
---
---          用法：binary 目标除 context/board_assets/image_convert 外，再加本规则：
---            add_rules("oh_my_robot.context", "oh_my_robot.board_assets",
---                      "oh_my_robot.image_convert", "oh_my_robot.selfreg")
---
---          约定：lib/systems/src 与 lib/services/src 视为"自注册模块目录"，其下所有 .c
---          自动注入。需要后台任务/资源的模块在自己的 SYSTEM 级回调里建线程即可。

local om_root = path.join(os.scriptdir(), "..", "..")

--- 自注册模块源目录（相对框架根），其下所有 .c 自动注入 binary。
local SELFREG_DIRS = {
    "lib/systems/src",
    "lib/services/src",
}

rule("oh_my_robot.selfreg")
    --- 配置阶段把自注册模块源直接挂到 binary
    ---@param target target 二进制目标
    on_config(function(target)
        if target:kind() ~= "binary" then
            return
        end
        local includedirs = {
            path.join(om_root, "lib/include"),
            path.join(om_root, "lib/systems/include"),
            path.join(om_root, "lib/services/include"),
        }
        for _, dir in ipairs(SELFREG_DIRS) do
            local files = os.files(path.join(om_root, dir, "**.c"))
            if files then
                for _, f in ipairs(files) do
                    target:add("files", f, { includedirs = includedirs })
                end
            end
        end
    end)
rule_end()
