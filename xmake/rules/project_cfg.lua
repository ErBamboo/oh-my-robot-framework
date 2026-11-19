--- @file oh_my_robot/xmake/rules/project_cfg.lua
--- @brief 工程配置片段自动发现规则 + 配置变更感知
--- @details ①检测工程 cfg/ 目录中的配置片段并注入编译上下文：
---   cfg/om_appcfg.h                        → 框架层覆写（注入 -DOM_USE_APPCFG）
---   cfg/boards/<board>/om_boardcfg.h       → 板层覆写（注入 -DOM_USE_BOARDCFG）
---   cfg/boards/<board>/om_bootcfg.json     → boot 布局源；生成头/表/链接脚本到 build/gen
---           片段不存在 = 零成本（不注入任何内容）；<board> 取当前构建板
---           （context.board_name——与 preset board= 一致）。
--- ②boot 布局：JSON 源是唯一事实源；生成物只写 build/<...>/gen/om_bootcfg，
---   不写回 cfg。包含 C 头、分区表、生成清单，以及由板级模板填入 ROM 值的
---   完整链接脚本。旧的 om_bootcfg.h 仅保留过渡兼容，和 JSON 同时存在时构建失败。
--- ③配置变更感知：每个配置再生（xmake f）向 <project>/.xmake/om_cfg_state
---   写入当前配置状态摘要，并作为所有目标的 depfile——配置/旗标变更后
---   构建期自动重编（根治"旗标变更不重编/陈旧归档成员"——实测备忘）。
---   命名/职责/优先级契约见 ADR-0017 (project_config_layering)。
--- 用法：各目标在规则组中显式添加 "oh_my_robot.project_cfg"（binary 与框架库目标
---       lib/xmake.lua tar_* 均挂接——注入全编译单元一致，见 2026-09-05 审计：
---       binary-only 注入致驱动库源（如 log_serial_backend.c 锋利边守卫）未达
---       appcfg 宏——配置分层一致性缺口）；库目标另挂同款 depfile（同文件状态机）。
local om_root = path.join(os.scriptdir(), "..", "..")
local modules_root = path.join(om_root, "xmake", "modules")

--- 配置状态摘要文件（所有目标共用的配置标记 depfile；lib/xmake.lua 同名路径）
local function cfg_state_file()
    return path.join(os.projectdir(), ".xmake", "om_cfg_state")
end

--- 解析构建根目录（支持 xmake -o）
---@param project_dir string 工程根
---@param config table xmake 配置模块
---@return string build_dir 构建根目录绝对路径
local function resolve_build_dir(project_dir, config)
    return path.absolute(config.get("builddir") or "build", project_dir)
end

--- 查找 Python 解释器
---@param find_tool function xmake find_tool 模块
---@return string program Python 可执行文件
local function resolve_python(find_tool)
    local python = find_tool("python3") or find_tool("python")
    if not python then
        raise("python3/python not found (required by tools/omlayout)")
    end
    return python.program or python
end

--- 链接脚本变体名（与构建选项 om_link_variant 对应）
local BOOT_LINKER_VARIANTS = {"bootloader", "app_a", "app_b"}

--- 解析 boot 布局源与生成路径
---@param project_dir string 工程根
---@param board_name string 板名
---@param build_dir string 构建根目录
---@return table paths 路径集合
local function resolve_boot_layout(project_dir, board_name, build_dir)
    local board_dir = path.join(project_dir, "cfg", "boards", board_name)
    local out_dir = path.join(build_dir, "gen", board_name, "om_bootcfg")
    return {
        json = path.join(board_dir, "om_bootcfg.json"),
        legacy = path.join(board_dir, "om_bootcfg.h"),
        out = out_dir,
        header = path.join(out_dir, "om_bootcfg.h"),
        table = path.join(out_dir, "boot_partition_data.c"),
        table_header = path.join(out_dir, "boot_partition_data.h"),
        manifest = path.join(out_dir, "om_bootcfg.generated.json"),
        stamp = path.join(out_dir, ".omlayout.stamp"),
        template_root = path.join(om_root, "platform", "bsp", "boards", board_name, "linker", "templates"),
        linker_gcc = path.join(out_dir, "linker", "gcc"),
        linker_arm = path.join(out_dir, "linker", "arm"),
    }
end

--- 生成器脚本路径
local function layout_generator_script()
    return path.join(om_root, "tools", "omlayout", "omlayout.py")
end

--- 模板文件列表
---@param paths table 路径集合
---@return string[] files 模板文件
local function template_files(paths)
    return {
        path.join(paths.template_root, "gcc", "layout.ld.in"),
        path.join(paths.template_root, "arm", "layout.sct.in"),
    }
end

--- 生成物列表（C 产物 + 存在模板时的链接脚本）
---@param paths table 路径集合
---@return string[] files 生成物
local function output_files(paths)
    local files = {paths.header, paths.table, paths.table_header, paths.manifest}
    if os.isdir(paths.template_root) then
        for _, variant in ipairs(BOOT_LINKER_VARIANTS) do
            files[#files + 1] = path.join(paths.linker_gcc, variant .. ".ld")
            files[#files + 1] = path.join(paths.linker_arm, variant .. ".sct")
        end
    end
    return files
end

--- 生成标记内容（源 + 生成器 + 模板的版本指纹）
---@param paths table 路径集合
---@param io table xmake io 模块
---@param osapi table xmake os 模块
---@return string stamp 标记内容
local function layout_stamp(paths, io, osapi)
    local parts = {
        io.readfile(paths.json) or "",
        "\n-- generator mtime: ",
        tostring(osapi.mtime(layout_generator_script())),
        "\n",
    }
    if os.isdir(paths.template_root) then
        for _, file in ipairs(template_files(paths)) do
            parts[#parts + 1] = "-- template " .. file .. ":\n"
            parts[#parts + 1] = io.readfile(file) or ""
            parts[#parts + 1] = "\n"
        end
    end
    return table.concat(parts)
end

--- 生成物是否与源、生成器和模板一致（生成标记落盘，跨目标事件去重）
---@param paths table 路径集合
---@param io table xmake io 模块
---@param osapi table xmake os 模块
---@return boolean current
local function layout_is_current(paths, io, osapi)
    local script = layout_generator_script()
    if not (os.isfile(paths.json) and os.isfile(script) and os.isfile(paths.stamp)) then
        return false
    end
    if os.isdir(paths.template_root) then
        for _, file in ipairs(template_files(paths)) do
            if not os.isfile(file) then
                return false
            end
        end
    end
    for _, file in ipairs(output_files(paths)) do
        if not os.isfile(file) then
            return false
        end
    end
    return io.readfile(paths.stamp) == layout_stamp(paths, io, osapi)
end

--- 生成物快照（用于检测任何输出变化）
---@param paths table 路径集合
---@param io table xmake io 模块
---@return table snapshot 文件内容表
local function snapshot_outputs(paths, io)
    local snapshot = {}
    for _, file in ipairs(output_files(paths)) do
        snapshot[file] = os.isfile(file) and io.readfile(file) or nil
    end
    return snapshot
end

--- 运行布局生成器并写生成标记
---@param paths table 路径集合
---@param io table xmake io 模块
---@param find_tool function xmake find_tool 模块
---@param execv function xmake os.execv
---@param osapi table xmake os 模块
---@return boolean changed 生成物是否发生变化
local function run_layout_generator(paths, io, find_tool, execv, osapi)
    local script = layout_generator_script()
    if not os.isfile(script) then
        raise("boot layout generator not found: " .. script)
    end
    local before = snapshot_outputs(paths, io)
    local python = resolve_python(find_tool)
    local argv = {script, "generate", "--spec", paths.json, "--out", paths.out}
    if os.isdir(paths.template_root) then
        argv[#argv + 1] = "--template-root"
        argv[#argv + 1] = paths.template_root
    end
    local code = execv(python, argv, {stdout = io.stdout, stderr = io.stderr})
    if code ~= 0 then
        raise("boot layout generation failed: " .. paths.json)
    end
    local changed = false
    for _, file in ipairs(output_files(paths)) do
        local after = os.isfile(file) and io.readfile(file) or nil
        if before[file] ~= after then
            changed = true
        end
    end
    io.writefile(paths.stamp, layout_stamp(paths, io, osapi))
    return changed
end

--- 确保本板布局与源、生成器一致
---@param paths table 路径集合
---@param io table xmake io 模块
---@param find_tool function xmake find_tool 模块
---@param execv function xmake os.execv
---@param osapi table xmake os 模块
---@return boolean changed 生成物是否发生变化
local function ensure_layout_generated(paths, io, find_tool, execv, osapi)
    if layout_is_current(paths, io, osapi) then
        return false
    end
    return run_layout_generator(paths, io, find_tool, execv, osapi)
end

--- 清理 xmake 内容缓存（生成物变化时确保重编/重链）
---@param project_dir string 工程根
---@param build_dir string 构建根目录
---@param osapi table xmake os 模块
local function invalidate_build_cache(project_dir, build_dir, osapi)
    local cache_dir = path.join(project_dir, ".cache")
    if os.isdir(cache_dir) then
        osapi.rmdir(cache_dir)
    end
    local build_cache_dir = path.join(build_dir, ".build_cache")
    if os.isdir(build_cache_dir) then
        osapi.rmdir(build_cache_dir)
    end
end

rule("oh_my_robot.project_cfg")
    --- 配置阶段检测并注入工程配置片段 + 写入配置状态标记
    ---@param target target 目标对象
    on_config(function(target)
        local config = import("core.project.config")
        config.load()
        local find_tool = import("lib.detect.find_tool")
        local oh_my_robot = import("oh-my-robot", {rootdir = modules_root})
        local context = oh_my_robot.get_context()
        local project_dir = os.projectdir()
        local build_dir = resolve_build_dir(project_dir, config)
        local appcfg = path.join(project_dir, "cfg", "om_appcfg.h")
        local has_appcfg = os.isfile(appcfg)
        local board_name = context.board_name
        local boardcfg = board_name and path.join(project_dir, "cfg", "boards", board_name, "om_boardcfg.h") or nil
        local has_boardcfg = boardcfg and os.isfile(boardcfg) or false
        local link_variant = config.get("om_link_variant") or "default"
        local selfreg_profile = config.get("om_selfreg_profile") or "full"

        local boot_paths = board_name and resolve_boot_layout(project_dir, board_name, build_dir) or nil
        local has_bootcfg_json = boot_paths and os.isfile(boot_paths.json) or false
        local has_bootcfg_legacy = boot_paths and os.isfile(boot_paths.legacy) or false
        if has_bootcfg_json and has_bootcfg_legacy then
            raise("both om_bootcfg.json and om_bootcfg.h exist; remove the legacy header")
        end
        if has_bootcfg_json then
            if ensure_layout_generated(boot_paths, io, find_tool, os.execv, os) then
                invalidate_build_cache(project_dir, build_dir, os)
            end
        end

        -- 配置状态摘要：内容 = 注入决定性状态（含片段文件原文——值变更即感知）；
        -- 仅当内容变化时重写（mtime 变化→depfile 触发重编）
        local function cfg_content(p)
            return os.isfile(p) and io.readfile(p) or ""
        end
        local boot_state = has_bootcfg_json and cfg_content(boot_paths.json)
            or (has_bootcfg_legacy and cfg_content(boot_paths.legacy) or "")
        local state = string.format("board=%s\nvariant=%s\nselfreg=%s\n== appcfg ==\n%s\n== boardcfg ==\n%s\n== bootcfg ==\n%s",
                                    tostring(board_name), link_variant, selfreg_profile,
                                    cfg_content(appcfg), cfg_content(boardcfg), boot_state)
        local marker = cfg_state_file()
        local cur = os.isfile(marker) and io.readfile(marker) or nil
        if cur ~= state then
            io.writefile(marker, state)
            invalidate_build_cache(project_dir, build_dir, os)
        end

        -- 注入面向全部目标（含框架库 tar_*）：配置分层（ADR-0017）= 所有编译单元
        -- 一致可见工程配置——binary-only 会让库源（drivers 等）配置不一致
        if has_appcfg then
            target:add("defines", "OM_USE_APPCFG")
            target:add("includedirs", path.join(project_dir, "cfg"))
        end
        if has_boardcfg then
            target:add("defines", "OM_USE_BOARDCFG")
            target:add("includedirs", path.join(project_dir, "cfg", "boards", board_name))
        end
        if has_bootcfg_json then
            target:add("defines", "OM_USE_BOOTCFG")
            target:add("includedirs", boot_paths.out)
            -- 分区表实例进 binary；库目标只消费生成的 om_bootcfg.h
            if target:kind() == "binary" then
                target:add("files", boot_paths.table)
            end
        elseif has_bootcfg_legacy then
            target:add("defines", "OM_USE_BOOTCFG")
            target:add("includedirs", path.join(project_dir, "cfg", "boards", board_name))
        end
    end)

    --- 构建前再生成一次，吸收配置后对 JSON 的修改
    ---@param target target 目标对象
    before_build(function(target)
        local find_tool = import("lib.detect.find_tool")
        local oh_my_robot = import("oh-my-robot", {rootdir = modules_root})
        local context = oh_my_robot.get_context()
        local board_name = context.board_name
        if not board_name then
            return
        end
        local config = import("core.project.config")
        config.load()
        local project_dir = os.projectdir()
        local build_dir = resolve_build_dir(project_dir, config)
        local paths = resolve_boot_layout(project_dir, board_name, build_dir)
        if not os.isfile(paths.json) then
            return
        end
        if os.isfile(paths.legacy) then
            raise("both om_bootcfg.json and om_bootcfg.h exist; remove the legacy header")
        end
        if layout_is_current(paths, io, os) then
            return
        end
        local changed = run_layout_generator(paths, io, find_tool, os.execv, os)
        if changed then
            invalidate_build_cache(project_dir, build_dir, os)
        end
    end)
