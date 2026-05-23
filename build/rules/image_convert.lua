--- @file oh_my_robot/build/rules/image_convert.lua
--- @brief OM 镜像转换规则定义
--- @details 构建后生成 hex/bin 镜像并在清理时移除。
local om_root = path.join(os.scriptdir(), "..", "..")
local modules_root = path.join(om_root, "build", "modules")

--- @rule oh_my_robot.image_convert
--- @brief 生成 hex/bin 镜像
--- @details 在构建完成后执行镜像转换。
rule("oh_my_robot.image_convert")
    --- 构建后生成 hex/bin 镜像
    ---@param target target 目标对象
    after_build(function(target)
        -- 解析工具链镜像转换器并执行转换
        local oh_my_robot = import("oh-my-robot", {rootdir = modules_root})
        local context = oh_my_robot.get_context()
        local toolchain_lib = import("toolchain_lib", {rootdir = modules_root})
        local toolchain_config = toolchain_lib.get_toolchain_config()
        local converters = toolchain_lib.resolve_image_converters(toolchain_config, context.toolchain_name)
        if converters then
            local elf_file = target:targetfile()
            local output_dir = target:targetdir()
            local basename = target:basename()
            local hex_file = path.join(output_dir, basename .. ".hex")
            local bin_file = path.join(output_dir, basename .. ".bin")
            toolchain_lib.run_image_convert(converters.hex, elf_file, hex_file)
            toolchain_lib.run_image_convert(converters.bin, elf_file, bin_file)
        end
        -- armlink produces ELF with single LOAD segment that does not encode
        -- VMA/LMA separation for RW data. Add a second LOAD segment so GDB
        -- loads .data init values to flash LMA rather than RAM VMA.
        if context.toolchain_name == "armclang" then
            local elf_file = target:targetfile()
            if os.isfile(elf_file) then
                local f = io.open(elf_file, "rb")
                if f then
                    local data = f:read("a")
                    f:close()
                    if data:sub(1, 4) == "\127ELF" and data:byte(5) == 1 and data:byte(6) == 1 then
                        -- ELF32 header (1-based positions): e_phoff@0x1D  e_shoff@0x21
                        local e_phoff, e_shoff = string.unpack("<I4I4", data, 0x1D)
                        -- e_phnum@0x2D  e_shentsize@0x2F  e_shnum@0x31  e_shstrndx@0x33
                        local e_phnum, e_shentsize = string.unpack("<HH", data, 0x2D)
                        local e_shnum, e_shstrndx = string.unpack("<HH", data, 0x31)
                        if e_shstrndx ~= 0 and e_shstrndx < e_shnum then
                            -- String table section header: sh_offset@0x10  sh_size@0x14
                            local strtab_hdr = 1 + e_shoff + e_shstrndx * e_shentsize
                            local strtab_off = string.unpack("<I4", data, strtab_hdr + 0x10)
                            -- Find RW_IRAM1 PROGBITS section
                            local rw_off, rw_sz
                            for i = 0, e_shnum - 1 do
                                local sh = 1 + e_shoff + i * e_shentsize
                                local sh_name, sh_type = string.unpack("<I4I4", data, sh)
                                local sh_addr, sh_offset, sh_size = string.unpack("<I4I4I4", data, sh + 0xC)
                                if sh_type == 1 and sh_addr == 0x20000000 then  -- SHT_PROGBITS
                                    local ns = 1 + strtab_off + sh_name
                                    local ne = data:find("\0", ns)
                                    local name = data:sub(ns, (ne or (ns + 255)) - 1)
                                    if name:find("RW") then
                                        rw_off, rw_sz = sh_offset, sh_size
                                        break
                                    end
                                end
                            end
                            if rw_off and rw_sz > 0 then
                                local flash_lma = 0x08000000 + (rw_off - 0x40)
                                -- ELF32_Phdr: p_type p_offset p_vaddr p_paddr p_filesz p_memsz p_flags p_align
                                local new_phdr = string.pack("<I4I4I4I4I4I4I4I4",
                                    1, rw_off, 0x20000000, flash_lma,
                                    rw_sz, rw_sz, 2 | 4, 8)
                                local ins_pos = 1 + e_phoff + e_phnum * 0x20
                                data = data:sub(1, ins_pos - 1) .. new_phdr .. data:sub(ins_pos)
                                -- Update e_phnum (+1)
                                data = data:sub(1, 0x2C) .. string.pack("<H", e_phnum + 1) .. data:sub(0x2F)
                                -- Update e_shoff (+0x20)
                                data = data:sub(1, 0x20) .. string.pack("<I4", e_shoff + 0x20) .. data:sub(0x25)
                                f = io.open(elf_file, "wb")
                                if f then
                                    f:write(data)
                                    f:close()
                                end
                            end
                        end
                    end
                end
            end
        end
    end)
    --- 清理时删除生成的 hex/bin 镜像
    ---@param target target 目标对象
    after_clean(function(target)
        local output_dir = target:targetdir()
        local basename = target:basename()
        local hex_file = path.join(output_dir, basename .. ".hex")
        local bin_file = path.join(output_dir, basename .. ".bin")
        if os.isfile(hex_file) then
            os.tryrm(hex_file)
        end
        if os.isfile(bin_file) then
            os.tryrm(bin_file)
        end
    end)
rule_end()
