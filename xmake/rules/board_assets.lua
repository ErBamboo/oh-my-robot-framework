--- @file oh_my_robot/xmake/rules/board_assets.lua
--- @brief OM 板级资源规则定义
--- @details 注入板级启动文件与链接脚本。
local om_root = path.join(os.scriptdir(), "..", "..")
local modules_root = path.join(om_root, "xmake", "modules")

--- @rule oh_my_robot.board_assets
--- @brief 注入板级启动文件与链接脚本
--- @details 配置阶段根据 board/toolchain 解析并注入资源。
rule("oh_my_robot.board_assets")
    --- 配置阶段注入板级构建资源
    ---@param target target 目标对象
    on_config(function(target)
        if target:kind() ~= "binary" then
            return
        end
        local config = import("core.project.config")
        config.load()
        local variant_name = config.get("om_link_variant") or "default"
        local selfreg_profile = config.get("om_selfreg_profile") or "full"
        -- 读取板级构建资源
        local oh_my_robot = import("oh-my-robot", {rootdir = modules_root})
        local context = oh_my_robot.get_context()
        local bsp_root = path.join(om_root, "platform", "bsp")
        local bsp = import("bsp", {rootdir = bsp_root})
        local assets = bsp.get_board_build_assets(context.board_name, context.toolchain_name, variant_name)
        if assets.startup then
            target:add("files", assets.startup)
        end
        local override_sources = bsp.get_board_override_sources(context.board_name, selfreg_profile)
        if override_sources and #override_sources > 0 then
            local inputs = bsp.get_board_build_inputs(context.board_name)
            target:add("files", override_sources, {
                includedirs = inputs.includedirs,
                defines = inputs.defines,
            })
        end
        local selfreg_sources = bsp.get_board_selfreg_sources(context.board_name, selfreg_profile)
        if selfreg_sources and #selfreg_sources > 0 then
            local inputs = bsp.get_board_build_inputs(context.board_name)
            target:add("files", selfreg_sources, {
                includedirs = inputs.includedirs,
                defines = inputs.defines,
            })
        end
        if assets.linkerscript then
            local linker_flag = config.get("toolchain_linker_flag")
            if not linker_flag or linker_flag == "" then
                raise("toolchain linker flag not set: " .. context.toolchain_name)
            end
            target:add("ldflags", linker_flag .. assets.linkerscript, {force = true})
        end
    end)
rule_end()
