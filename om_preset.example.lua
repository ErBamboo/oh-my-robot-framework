-- Example preset for project-local configuration.
-- Copy this file to <project-root>/om_preset.lua and update paths for the current project.

local preset = {
  board = {name = "rm-c-board"},
  os = {name = "freertos"},
  toolchain_default = {
    name = "gnu-rm",
  },
  toolchain_presets = {
    ["gnu-rm"] = {
      sdk = "D:/Toolchains/gcc-arm-none-eabi-10.3-2021.10",
      bin = "D:/Toolchains/gcc-arm-none-eabi-10.3-2021.10/bin",
    },
    ["armclang"] = {
      sdk = "D:/Program Files/ProgramTools/Keil_v5/ARM/ARMCLANG",
      bin = "D:/Program Files/ProgramTools/Keil_v5/ARM/ARMCLANG/bin",
    },
  },
  flash = {
    flasher = "jlink",                -- 默认烧录器：jlink / daplink
    target = "robot_project",
    firmware = nil,
    prefer_hex = true,
    reset = true,
    run = true,
    native_output = false,
    jlink = {
      device = "STM32F407IG",
      interface = "swd",
      speed = 4000,
      program = "D:/Program Files/ProgramTools/SEGGER/Jlink/JLink.exe",
    },
    daplink = {
      program = "D:/openocd-win/bin/openocd.exe",
      config = ".vscode/daplink.cfg",
      frequency = 4000,
    },
  },
}

function get_preset()
  return preset
end
