--- @file oh_my_robot/build/rules/project_cfg.lua
--- @brief 工程配置片段自动发现规则
--- @details 检测工程 cfg/ 目录中的配置片段并注入编译上下文：
---   cfg/om_appcfg.h                        → 框架层覆写（注入 -DOM_USE_APPCFG）
---   cfg/boards/<board>/om_boardcfg.h          → 板层覆写（注入 -DOM_USE_BOARDCFG）
---           片段不存在 = 零成本（不注入任何内容）；<board> 取当前构建板
---           （context.board_name——与 preset board= 一致）。
---           命名/职责/优先级契约见 ADR-0017 (project_config_layering)。
--- 用法：binary 目标在规则组中显式添加 "oh_my_robot.project_cfg"
local om_root = path.join(os.scriptdir(), "..", "..")
local modules_root = path.join(om_root, "build", "modules")

rule("oh_my_robot.project_cfg")
    --- 配置阶段检测并注入工程配置片段
    ---@param target target 二进制目标
    on_config(function(target)
        if target:kind() ~= "binary" then
            return
        end
        local oh_my_robot = import("oh-my-robot", {rootdir = modules_root})
        local context = oh_my_robot.get_context()
        local project_dir = os.projectdir()
        -- 框架层片段：cfg/om_appcfg.h
        local appcfg = path.join(project_dir, "cfg", "om_appcfg.h")
        if os.isfile(appcfg) then
            target:add("defines", "OM_USE_APPCFG")
            target:add("includedirs", path.join(project_dir, "cfg"))
        end
        -- 板层片段：cfg/boards/<board>/om_boardcfg.h
        local board_name = context.board_name
        if board_name then
            local board_cfg_dir = path.join(project_dir, "cfg", "boards", board_name)
            local boardcfg = path.join(board_cfg_dir, "om_boardcfg.h")
            if os.isfile(boardcfg) then
                target:add("defines", "OM_USE_BOARDCFG")
                target:add("includedirs", board_cfg_dir)
            end
        end
    end)
