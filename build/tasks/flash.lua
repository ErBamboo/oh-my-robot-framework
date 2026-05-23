--- @task flash
--- @brief 统一烧录任务
--- @details 通过策略模式支持 J-Link / DAPLink 等多种调试器。
task("flash")
    set_menu {
        usage = "xmake flash [options]",
        description = "Flash firmware via J-Link / DAPLink",
        options = {
            {nil, "flasher", "kv", nil, "Flasher type: jlink, daplink"},
            {"d", "device", "kv", nil, "J-Link device name / pyOCD target name"},
            {"i", "interface", "kv", nil, "J-Link interface (swd/jtag)"},
            {"s", "speed", "kv", nil, "J-Link speed (kHz) / DAPLink frequency (Hz)"},
            {"f", "firmware", "kv", nil, "Firmware file (.hex/.elf/.bin)"},
            {"H", "prefer_hex", "kv", nil, "Prefer HEX when auto resolving (true/false)"},
            {"p", "program", "kv", nil, "Flasher executable path"},
            {"t", "target", "kv", nil, "Target name (xmake target)"},
            {"r", "reset", "kv", "true", "Reset after flash"},
            {"g", "run", "kv", "true", "Run after flash"},
            {"n", "native_output", "kv", nil, "Show native flasher CLI output"},
            {"u", "uid", "kv", nil, "DAPLink probe serial number / UID"},
        }
    }
    on_run(function()
        -- ===================================================================
        -- 工具函数
        -- ===================================================================
        local build_root = path.join(os.scriptdir(), "..")
        local modules_root = path.join(build_root, "modules")

        local function resolve_bool(value, default_value)
            if value == nil then
                return default_value
            end
            if type(value) == "boolean" then
                return value
            end
            local normalized = tostring(value):lower()
            if normalized == "1" or normalized == "true" or normalized == "yes" then
                return true
            end
            if normalized == "0" or normalized == "false" or normalized == "no" then
                return false
            end
            return default_value
        end

        local function resolve_path(file_path)
            if path.is_absolute(file_path) then
                return file_path
            end
            return path.join(os.projectdir(), file_path)
        end

        local function normalize_extension(file_path)
            local ext = path.extension(file_path) or ""
            ext = ext:lower()
            if ext ~= "" and ext:sub(1, 1) ~= "." then
                ext = "." .. ext
            end
            return ext
        end

        local function ensure_firmware_extension(file_path)
            local ext = normalize_extension(file_path)
            if ext == ".elf" or ext == ".hex" or ext == ".bin" then
                return
            end
            raise("firmware must include explicit extension: .elf/.hex/.bin")
        end

        local function read_config_scalar(value)
            if type(value) == "table" then
                return value[1]
            end
            return value
        end

        local function normalize_toolchain_name(name)
            if not name or name == "" then
                return name
            end
            local base = name:match("^(.-)%[")
            return base or name
        end

        --- 从 flash preset 提取通用字段（兼容新旧格式）
        local function read_common_value(flash_preset, key)
            if not flash_preset then
                return nil
            end
            if flash_preset[key] ~= nil and flash_preset[key] ~= "" then
                return flash_preset[key]
            end
            return nil
        end

        --- 加载 flash 预设
        local function load_flash_preset()
            local oh_my_robot = import("oh-my-robot", {rootdir = modules_root})
            local preset = oh_my_robot.get_preset()
            if not preset or type(preset) ~= "table" then
                return nil
            end
            return preset.flash
        end

        --- 加载工具链预设名
        local function load_toolchain_preset()
            local oh_my_robot = import("oh-my-robot", {rootdir = modules_root})
            local preset = oh_my_robot.get_preset()
            local toolchain_default = preset and preset.toolchain_default or nil
            return toolchain_default and toolchain_default.name or nil
        end

        --- 解析目标输出文件
        local function resolve_target_output(target_name, prefer_hex)
            local project = import("core.project.project")
            project.load_targets()
            local target = project.target(target_name)
            if not target then
                raise("target not found: " .. target_name)
            end
            local target_dir = target:targetdir()
            local basename = target:basename()
            local elf_file = target:targetfile()
            local elf_ext = normalize_extension(elf_file)
            local hex_file = path.join(target_dir, basename .. ".hex")
            local bin_file = path.join(target_dir, basename .. ".bin")
            if prefer_hex and os.isfile(hex_file) then
                return hex_file
            end
            if not prefer_hex and os.isfile(bin_file) then
                return bin_file
            end
            if os.isfile(elf_file) then
                if elf_ext == "" then
                    raise("target output missing extension: please set filename to .elf")
                end
                return elf_file
            end
            if os.isfile(hex_file) then
                return hex_file
            end
            if os.isfile(bin_file) then
                return bin_file
            end
            raise("flash file not found: please build target and enable oh_my_robot.image_convert")
        end

        --- 通用程序查找
        local function find_program(name, paths)
            local find_tool = import("lib.detect.find_tool")
            local tool = find_tool(name, {paths = paths or {}})
            if tool and tool.program and os.isexec(tool.program) then
                return tool.program
            end
            return nil
        end

        -- ===================================================================
        -- Flasher 策略：J-Link
        -- ===================================================================
        local flasher_jlink = {
            name = "jlink",

            resolve_config = function(self, ctx)
                local option = ctx.option
                local preset = ctx.flash_preset

                local jlink_preset = nil
                if preset then
                    if type(preset.jlink) == "table" then
                        jlink_preset = preset.jlink
                    elseif not preset.flasher then
                        -- 旧格式：整个 flash 表就是 jlink 配置
                        jlink_preset = preset
                    end
                end

                local config = {
                    device = option.get("device")
                        or (jlink_preset and jlink_preset.device)
                        or "STM32F407IG",
                    interface = option.get("interface")
                        or (jlink_preset and jlink_preset.interface)
                        or "swd",
                    speed = option.get("speed")
                        or (jlink_preset and jlink_preset.speed)
                        or "4000",
                    program = option.get("program")
                        or (jlink_preset and jlink_preset.program),
                    reset_after = resolve_bool(
                        option.get("reset") or read_common_value(preset, "reset"),
                        true
                    ),
                    run_after = resolve_bool(
                        option.get("run") or read_common_value(preset, "run"),
                        true
                    ),
                    native_output = resolve_bool(
                        option.get("native_output") or read_common_value(preset, "native_output"),
                        false
                    ),
                }
                ctx.flasher_config = config
                return ctx
            end,

            get_program = function(config)
                if config.program and config.program ~= "" then
                    local abs = resolve_path(config.program)
                    if os.isexec(abs) then
                        return abs
                    end
                    raise("jlink program not executable: " .. abs)
                end
                local found = find_program("JLink")
                    or find_program("JLink.exe")
                if found then
                    return found
                end
                raise("J-Link program not found: please pass --program=<path>")
            end,

            build_command = function(config, firmware, build_dir)
                local firmware_size = nil
                if normalize_extension(firmware) == ".bin" then
                    firmware_size = os.filesize(firmware)
                    if not firmware_size or firmware_size == 0 then
                        raise("cannot determine firmware file size: " .. firmware)
                    end
                end

                local lines = {}
                lines[#lines + 1] = "device " .. config.device
                lines[#lines + 1] = "if " .. config.interface
                lines[#lines + 1] = "speed " .. tostring(config.speed)
                if config.reset_after then
                    lines[#lines + 1] = "r"
                end
                lines[#lines + 1] = "halt"
                local ext = normalize_extension(firmware)
                if ext == ".bin" then
                    local end_addr = 0x08000000 + firmware_size
                    lines[#lines + 1] = string.format("erase 0x08000000, 0x%08X", end_addr)
                    lines[#lines + 1] = "loadbin " .. firmware .. ", 0x08000000"
                    lines[#lines + 1] = "verifybin " .. firmware .. ", 0x08000000"
                else
                    lines[#lines + 1] = "loadfile " .. firmware
                end
                if config.reset_after then
                    lines[#lines + 1] = "r"
                end
                if config.run_after then
                    lines[#lines + 1] = "g"
                end
                lines[#lines + 1] = "q"

                local flash_dir = path.join(build_dir, "oh-my-robot", "flash")
                os.mkdir(flash_dir)
                local command_path = path.join(flash_dir, "jlink_flash.jlink")
                io.writefile(command_path, table.concat(lines, "\n") .. "\n")
                return {"-CommandFile", command_path}
            end,
        }

        -- ===================================================================
        -- Flasher 策略：DAPLink (OpenOCD)
        -- ===================================================================
        local flasher_daplink = {
            name = "daplink",

            resolve_config = function(self, ctx)
                local option = ctx.option
                local preset = ctx.flash_preset

                local daplink_preset = nil
                if preset and type(preset.daplink) == "table" then
                    daplink_preset = preset.daplink
                end

                local config = {
                    config_file = (daplink_preset and daplink_preset.config),
                    frequency = option.get("speed")
                        or (daplink_preset and daplink_preset.frequency)
                        or 4000,
                    program = option.get("program")
                        or (daplink_preset and daplink_preset.program),
                    reset_after = resolve_bool(
                        option.get("reset") or read_common_value(preset, "reset"),
                        true
                    ),
                    run_after = resolve_bool(
                        option.get("run") or read_common_value(preset, "run"),
                        true
                    ),
                    native_output = resolve_bool(
                        option.get("native_output") or read_common_value(preset, "native_output"),
                        false
                    ),
                }
                ctx.flasher_config = config
                return ctx
            end,

            get_program = function(config)
                if config.program and config.program ~= "" then
                    local abs = resolve_path(config.program)
                    if os.isexec(abs) then
                        return abs
                    end
                    local found = find_program(config.program)
                    if found then
                        return found
                    end
                    raise("openocd program not found: " .. abs)
                end
                local found = find_program("openocd")
                if found then
                    return found
                end
                raise("openocd not found: please install OpenOCD or pass --program=<path>")
            end,

            build_command = function(config, firmware, build_dir)
                local args = {}
                if config.config_file then
                    args[#args + 1] = "-f"
                    args[#args + 1] = resolve_path(config.config_file)
                end
                if config.frequency then
                    args[#args + 1] = "-c"
                    args[#args + 1] = "adapter speed " .. tostring(config.frequency)
                end
                -- 将反斜杠转为正斜杠，避免 OpenOCD TCL 将其当作转义符
                local normalized_fw = firmware:gsub("\\", "/")
                local flash_cmd = "program " .. normalized_fw .. " verify"
                if config.reset_after then
                    flash_cmd = flash_cmd .. " reset"
                end
                flash_cmd = flash_cmd .. " exit"
                args[#args + 1] = "-c"
                args[#args + 1] = flash_cmd
                return args
            end,
        }

        -- ===================================================================
        -- Flasher 注册表
        -- ===================================================================
        local flasher_registry = {}

        local function register_flasher(flasher)
            if not flasher or not flasher.name then
                raise("flasher register: name required")
            end
            if flasher_registry[flasher.name] then
                raise("flasher register: duplicate name " .. flasher.name)
            end
            flasher_registry[flasher.name] = flasher
        end

        local function get_flasher(name)
            return flasher_registry[name]
        end

        local function detect_flasher_from_preset(preset)
            if not preset then
                return "jlink"
            end
            if preset.flasher then
                return preset.flasher
            end
            -- 旧格式兼容：无 flasher 字段默认为 jlink
            return "jlink"
        end

        register_flasher(flasher_jlink)
        register_flasher(flasher_daplink)

        -- ===================================================================
        -- 流水线步骤
        -- ===================================================================

        --- Step 0: 构建上下文
        local function build_context()
            local config = import("core.project.config")
            local option = import("core.base.option")
            config.load()
            local preset_toolchain = load_toolchain_preset()
            local config_toolchain = read_config_scalar(config.get("toolchain"))
            if preset_toolchain and config_toolchain then
                local preset_base = normalize_toolchain_name(preset_toolchain)
                local config_base = normalize_toolchain_name(config_toolchain)
                if preset_base ~= config_base then
                    print("[oh-my-robot] warning: preset toolchain (" .. preset_toolchain
                        .. ") differs from config toolchain (" .. config_toolchain
                        .. "). Using config.")
                end
            end
            return {
                config = config,
                option = option,
                flash_preset = load_flash_preset(),
            }
        end

        --- Step 1: 加载 flasher 策略
        local function step_load_flasher(ctx)
            local flasher_name = ctx.option.get("flasher")
            if not flasher_name then
                flasher_name = detect_flasher_from_preset(ctx.flash_preset)
            end
            ctx.flasher = get_flasher(flasher_name)
            if not ctx.flasher then
                local names = {}
                for k, _ in pairs(flasher_registry) do
                    names[#names + 1] = k
                end
                raise("unknown flasher: " .. flasher_name
                    .. ". Available: " .. table.concat(names, ", "))
            end
            return ctx
        end

        --- Step 2: 解析固件路径
        local function step_resolve_firmware(ctx)
            local option = ctx.option
            local preset = ctx.flash_preset
            ctx.target_name = option.get("target")
                or read_common_value(preset, "target")
                or "robot_project"
            ctx.prefer_hex = resolve_bool(
                option.get("prefer_hex") or read_common_value(preset, "prefer_hex"),
                true
            )
            local file_override = option.get("firmware")
                or read_common_value(preset, "firmware")
                or read_common_value(preset, "file")
            if file_override then
                ensure_firmware_extension(file_override)
            end
            ctx.firmware = file_override and resolve_path(file_override)
                or resolve_target_output(ctx.target_name, ctx.prefer_hex)
            if not os.isfile(ctx.firmware) then
                raise("flash file not found: " .. ctx.firmware)
            end
            return ctx
        end

        --- Step 3: 解析 flasher 配置
        local function step_resolve_flasher_config(ctx)
            ctx.flasher:resolve_config(ctx)
            return ctx
        end

        --- Step 4: 准备命令
        local function step_prepare_command(ctx)
            ctx.program = ctx.flasher.get_program(ctx.flasher_config)
            ctx.args = ctx.flasher.build_command(
                ctx.flasher_config,
                ctx.firmware,
                ctx.config.builddir()
            )
            return ctx
        end

        --- Step 5: 执行烧录
        local function step_execute(ctx)
            if ctx.flasher_config.native_output then
                os.execv(ctx.program, ctx.args)
            else
                os.runv(ctx.program, ctx.args)
            end
            local toolchain_name = ctx.config and ctx.config.get("toolchain") or "unknown"
            print("[oh-my-robot] flash flasher: " .. ctx.flasher.name)
            print("[oh-my-robot] flash program: " .. tostring(ctx.program))
            print("[oh-my-robot] flash firmware: " .. tostring(ctx.firmware))
            print("[oh-my-robot] flash toolchain: " .. tostring(toolchain_name))
            print("[oh-my-robot] flash native output: " .. tostring(ctx.flasher_config.native_output))
            return ctx
        end

        local ctx = build_context()
        local steps = {
            step_load_flasher,
            step_resolve_firmware,
            step_resolve_flasher_config,
            step_prepare_command,
            step_execute,
        }
        for _, step in ipairs(steps) do
            ctx = step(ctx)
        end
    end)
