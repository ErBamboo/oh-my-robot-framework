/**
 * @file  om_pre_init.h
 * @brief 极早期预初始化机制
 * @details 提供 OM_PRE_INIT 宏和 om_pre_init_run()，允许模块注册在
 *          om_board_init() 之前执行的极早期回调。

 *          回调函数指针存放在 .om_pre_init 段，链接脚本为该段定义边界符号
 *          __om_pre_init_start / __om_pre_init_end，om_pre_init_run() 遍历
 *          该区间依次调用非空指针。

 * 使用示例：
 *   static void my_early_init(void) { configure_pin(); }
 *   OM_PRE_INIT(my_early_init);
 *   // main() 开头调用 om_pre_init_run() 即可执行所有注册的回调

 * @note  1. 编译器差异由 om_compiler.h → om_port_compiler.h 吸收
 *        2. 链接器差异收敛于板级 linker 目录（.ld / .sct + 可选符号别名 .s）
 *        3. om_pre_init_run() 必须在 main() 中显式调用，不依赖 constructor
 *        4. 回调执行时仅有 .data/.bss 就绪，HAL 未初始化，只能操作寄存器
 *        5. 若链接脚本未提供边界符号，链接阶段报 undefined symbol 错误
 */

#ifndef __OM_PRE_INIT_H__
#define __OM_PRE_INIT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "core/om_compiler.h"
#include <stddef.h>

/** @brief 预初始化回调函数类型 */
typedef void (*OmPreInitFunc)(void);

/**
 * @brief 注册极早期预初始化回调
 *
 * 将 func 的函数指针放入 .om_pre_init 段，om_pre_init_run() 会在
 * om_board_init() 之前依次执行所有注册的回调。
 *
 * @param func  回调函数名（不带引号或括号）
 */
#define OM_PRE_INIT(func) \
    OM_USED static const OmPreInitFunc om_pre_init_##func \
        OM_SECTION(".om_pre_init") = (func)

/**
 * @brief 执行所有已注册的极早期预初始化回调
 *
 * 遍历 .om_pre_init 段（由链接器定义符号 __om_pre_init_start / end 界定），
 * 依次调用每个非空函数指针。必须在 main() 开头、om_board_init() 之前调用。
 */
void om_pre_init_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __OM_PRE_INIT_H__ */
