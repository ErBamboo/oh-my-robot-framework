--- @file oh_my_robot/platform/bsp/inputs.lua
--- @brief BSP 构建输入组装
--- @details 负责整合 board/chip/vendor 输入并规范化路径。
local bsp_root = os.scriptdir()
local utils = import("utils", {rootdir = bsp_root})
local data_loader = import("data_loader", {rootdir = bsp_root})
local components = import("components", {rootdir = bsp_root})

--- 解析当前构建的链接变体与自注册形态
---@return string variant_name 链接变体名称
---@return string selfreg_profile 自注册形态（full/boot）
local function resolve_build_options()
    local config = import("core.project.config")
    config.load()
    local variant_name = config.get("om_link_variant")
    if not variant_name or variant_name == "" then
        variant_name = "default"
    end
    local selfreg_profile = config.get("om_selfreg_profile")
    if not selfreg_profile or selfreg_profile == "" then
        selfreg_profile = "full"
    end
    return variant_name, selfreg_profile
end

--- 解析板级链接变体表项
---@param profile table Profile 数据
---@param variant_name string 链接变体名称
---@return table variant 变体表项（缺失时返回空表）
local function resolve_link_variant(profile, variant_name)
    local variants = profile.board.link_variants or {}
    return variants[variant_name] or variants.default or {}
end

--- 收集 profile 中声明的强覆盖源文件
---@param profile table Profile 数据
---@return string[] sources 覆盖源文件列表
local function collect_override_sources(profile)
    local sources = {}
    utils.append_list(sources, profile.vendor.override_sources)
    utils.append_list(sources, profile.chip.override_sources)
    utils.append_list(sources, profile.board.override_sources)
    return sources
end

--- 收集引导器形态的强覆盖源文件
---@param profile table Profile 数据
---@return string[] sources 覆盖源文件列表
local function collect_boot_override_sources(profile)
    local boot_sources = profile.board.boot_override_sources
    if not boot_sources then
        -- 未声明引导器子集时退回全量，避免裁剪配置在未适配的板上静默丢源
        return collect_override_sources(profile)
    end
    local sources = {}
    utils.append_list(sources, boot_sources)
    return sources
end

--- 收集 profile 中声明的自注册源文件（OM_INIT 自注册的板级模块）
--- 含显式声明（如 bsp_cpu.c）+ 自动发现的板级外设源（boards/<board>/source/peripherals/**.c，
--- 排除已在 override_sources 中的 ISR 文件，避免重复直连）。加外设只需在 peripherals/ 下
--- 新建 .c 并写 OM_INIT，无需改板数据。
---@param profile table Profile 数据
---@return string[] sources 自注册源文件列表（板级相对路径，正斜杠）
local function collect_selfreg_sources(profile)
    local sources = {}
    utils.append_list(sources, profile.vendor.selfreg_sources)
    utils.append_list(sources, profile.chip.selfreg_sources)
    utils.append_list(sources, profile.board.selfreg_sources)

    local board_name = profile.board and profile.board.name
    if board_name and board_name ~= "" then
        local periph_dir = path.join(bsp_root, "boards", board_name, "source", "peripherals")
        if os.isdir(periph_dir) then
            local override_set = {}
            for _, s in ipairs(collect_override_sources(profile)) do
                override_set[s] = true
            end
            local files = os.files(path.join(periph_dir, "**.c"))
            for _, f in ipairs(files or {}) do
                local rel = path.relative(f, bsp_root):gsub("\\", "/")
                if not override_set[rel] then
                    sources[#sources + 1] = rel
                end
            end
        end
    end
    return sources
end

--- 收集引导器形态的自注册源文件（只含启动必需的显式子集）
---@param profile table Profile 数据
---@return string[] sources 自注册源文件列表
local function collect_boot_selfreg_sources(profile)
    local boot_sources = profile.board.boot_selfreg_sources
    if not boot_sources then
        -- 未声明引导器子集时退回全量，避免裁剪配置在未适配的板上静默丢源
        return collect_selfreg_sources(profile)
    end
    local sources = {}
    utils.append_list(sources, boot_sources)
    return sources
end

--- 按自注册形态选择强覆盖源
---@param profile table Profile 数据
---@param selfreg_profile string 自注册形态
---@return string[] sources 覆盖源文件列表
local function select_override_sources(profile, selfreg_profile)
    if selfreg_profile == "boot" then
        return collect_boot_override_sources(profile)
    end
    return collect_override_sources(profile)
end

--- 按自注册形态选择自注册源
---@param profile table Profile 数据
---@param selfreg_profile string 自注册形态
---@return string[] sources 自注册源文件列表
local function select_selfreg_sources(profile, selfreg_profile)
    if selfreg_profile == "boot" then
        return collect_boot_selfreg_sources(profile)
    end
    return collect_selfreg_sources(profile)
end

--- 过滤需要从静态库中排除的源文件
---@param all_sources string[] 全量源文件
---@param excluded_sources string[] 需排除源文件
---@return string[] sources 过滤后源文件
local function filter_excluded_sources(all_sources, excluded_sources)
    if not excluded_sources or #excluded_sources == 0 then
        return all_sources
    end
    local excluded_map = {}
    for _, source in ipairs(excluded_sources) do
        excluded_map[source] = true
    end
    local sources = {}
    for _, source in ipairs(all_sources or {}) do
        if not excluded_map[source] then
            sources[#sources + 1] = source
        end
    end
    return sources
end

--- 获取板级强覆盖源文件（用于直接并入 binary，保证 weak 可被 strong 覆盖）
---@param board_name string 板级名称
---@param selfreg_profile string|nil 自注册形态（缺省读构建配置）
---@return string[] sources 覆盖源文件
function get_board_override_sources(board_name, selfreg_profile)
    local profile = data_loader.get_profile(board_name)
    local selected_profile = selfreg_profile
    if not selected_profile or selected_profile == "" then
        local _, config_profile = resolve_build_options()
        selected_profile = config_profile
    end
    local override_sources = select_override_sources(profile, selected_profile)
    return utils.normalize_paths(bsp_root, override_sources)
end

--- 获取板级自注册源文件（直接并入 binary，保证 OM_INIT 回调不被静态库抽取丢弃）
---@param board_name string 板级名称
---@param selfreg_profile string|nil 自注册形态（缺省读构建配置）
---@return string[] sources 自注册源文件
function get_board_selfreg_sources(board_name, selfreg_profile)
    local profile = data_loader.get_profile(board_name)
    local selected_profile = selfreg_profile
    if not selected_profile or selected_profile == "" then
        local _, config_profile = resolve_build_options()
        selected_profile = config_profile
    end
    local selfreg_sources = select_selfreg_sources(profile, selected_profile)
    return utils.normalize_paths(bsp_root, selfreg_sources)
end

--- 获取板级构建输入（头文件、源文件、宏与额外资源）
---@param board_name string 板级名称
---@return table inputs 构建输入
function get_board_build_inputs(board_name)
    local profile = data_loader.get_profile(board_name)
    local variant_name, selfreg_profile = resolve_build_options()
    local variant = resolve_link_variant(profile, variant_name)

    local inputs = {
        defines = {},
        includedirs = {},
        sources = {},
        headerfiles = {},
        extrafiles = {},
    }

    utils.append_list(inputs.defines, profile.vendor.defines)
    utils.append_list(inputs.defines, profile.chip.defines)
    utils.append_list(inputs.defines, profile.board.defines)

    -- 槽感知的向量表偏移：链接变体给出，编译期注入板级启动代码
    local vect_tab_offset = variant.vect_tab_offset or 0
    inputs.defines[#inputs.defines + 1] = string.format("OM_BOARD_VECT_TAB_OFFSET=0x%X", vect_tab_offset)

    utils.append_list(inputs.includedirs, profile.vendor.includedirs)
    utils.append_list(inputs.includedirs, profile.chip.includedirs)
    utils.append_list(inputs.includedirs, profile.board.includedirs)

    utils.append_list(inputs.sources, profile.vendor.sources)
    utils.append_list(inputs.sources, profile.chip.sources)
    utils.append_list(inputs.sources, profile.board.sources)

    local component_data = components.collect_component_data(profile)
    utils.append_list(inputs.defines, component_data.defines)
    utils.append_list(inputs.includedirs, component_data.includedirs)
    utils.append_list(inputs.sources, component_data.sources)
    utils.append_list(inputs.headerfiles, component_data.headerfiles)
    utils.append_list(inputs.extrafiles, component_data.extrafiles)

    inputs.includedirs = utils.normalize_paths(bsp_root, inputs.includedirs)
    inputs.sources = utils.normalize_paths(bsp_root, inputs.sources)
    inputs.override_sources = get_board_override_sources(board_name, selfreg_profile)
    inputs.selfreg_sources = get_board_selfreg_sources(board_name, selfreg_profile)
    -- override_sources 与 selfreg_sources 都直接并入 binary，须从 tar_board 源集中剔除
    inputs.sources = filter_excluded_sources(inputs.sources, inputs.override_sources)
    inputs.sources = filter_excluded_sources(inputs.sources, inputs.selfreg_sources)
    inputs.headerfiles = utils.normalize_paths(bsp_root, inputs.headerfiles)
    inputs.extrafiles = utils.normalize_paths(bsp_root, inputs.extrafiles)

    return inputs
end
