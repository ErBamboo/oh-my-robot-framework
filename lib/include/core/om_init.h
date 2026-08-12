/**
 * @file  om_init.h
 * @brief 分散加载自动注册初始化系统
 * @details 参考 Linux initcall、Zephyr SYS_INIT、RT-Thread auto-init。模块在自己的
 *          .c 里写一行 OM_INIT_<LEVEL>(func) 即可把回调注册进 .om_init 段；
 *          om_do_initcalls(level_lo, level_hi) 在启动期遍历该段，按 (level, prio)
 *          排序后依次调用，main() 无需显式调用各模块的 init。
 *
 *          边界符号 __om_init_start / __om_init_end 由链接脚本定义：
 *            GCC ld  : PROVIDE(__om_init_start = .); KEEP(*(.om_init));
 *                     PROVIDE(__om_init_end = .)
 *            armlink : scatter 执行域 ER_OM_INIT + 板级 boundary 别名文件
 *                     .set __om_init_start, Image$$ER_OM_INIT$$Base / ..._end, ...$$Limit
 *
 *          ── 级别划分：依赖轴镜像分层（每级 = 架构的一层）────────────────────
 *
 *            EARLIEST  —— HAL/时钟就绪前，仅寄存器操作
 *            BOARD     —— 板级自举(om_board_init@prio0) + bsp 设备注册（设备提供者）
 *            DRIVER    —— PAL/适配器/电机驱动（设备消费者，device_find 绑定）
 *            SERVICE   —— comm/log/config/diagnostics
 *            SYSTEM    —— chassis/gimbal/supercap 等业务系统
 *            LATE      —— 全部就绪后的自检/诊断收尾
 *
 *            级序即架构层序（lower layer 先注册，上层可 device_find 到下层），
 *            对应 Zephyr PRE_KERNEL_1(提供者)/PRE_KERNEL_2(消费者) 与 Linux
 *            subsys/device 的分层思想，也对应 RT-Thread BOARD/DEVICE/COMPONENT/APP。
 *
 *          ── 调度器上下文（能力轴，属性而非一级）──────────────────────────
 *
 *            EARLIEST / BOARD / DRIVER —— 调度器未启，跑在 main，回调不得阻塞、
 *                                         不得使用需调度器的 OSAL 服务
 *            SERVICE / SYSTEM / LATE   —— 调度器已启，跑在 init 线程，可阻塞/IPC/建线程
 *
 * @note  1. 编译器差异由 om_def.h 的 OM_SECTION / OM_USED 吸收
 *        2. 链接器差异收敛于板级 linker 目录（.ld / .sct + boundary 别名）
 *        3. 回调返回 OmRet；失败默认记录并继续（启动期 abort=变砖），
 *           定义 OM_INIT_ABORT_ON_FAIL 可在首个失败时中止
 *        4. 同级同 prio 的相对顺序不保证；需要强序请用不同 prio
 *        5. 自注册模块所在静态库须在全量链接（--whole-archive）下链接，
 *           否则无外部引用的 .o 会被链接器按需抽取丢弃，回调静默丢失
 *        6. 级"只增不删"：新增级不影响老模块，删除级要改所有注册点
 */

#ifndef __OM_INIT_H__
#define __OM_INIT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "core/om_def.h"

/*===========================================================================
 * 类型
 *===========================================================================*/

/** @brief 初始化回调函数类型 —— 返回 OmRet，OM_OK 表示成功 */
typedef OmRet (*OmInitFn)(void);

/** @brief init 注册表项 —— 由 OM_INIT 宏放入 .om_init 段 */
typedef struct
{
    OmInitFn    fn;    /**< 回调函数（NULL 表示空槽，跳过） */
    const char *name;  /**< 诊断用名（编译期 #func） */
    uint8_t     level; /**< 级别 OmInitLevel */
    uint8_t     prio;  /**< 同级优先级 0-99，小者先执行 */
} OmInitEntry;

/*===========================================================================
 * 级别（依赖轴：每级 = 架构的一层）
 *===========================================================================*/

typedef enum
{
    OM_INIT_LEVEL_EARLIEST = 0, /**< HAL/时钟前，仅寄存器；调度器未启，不可阻塞 */
    OM_INIT_LEVEL_BOARD,        /**< 板级自举 + bsp 设备注册（设备提供者）；不可阻塞 */
    OM_INIT_LEVEL_DRIVER,       /**< PAL/适配器/电机驱动（设备消费者）；不可阻塞 */
    OM_INIT_LEVEL_SERVICE,      /**< comm/log/config/diagnostics；可阻塞/IPC */
    OM_INIT_LEVEL_SYSTEM,       /**< 业务系统 chassis/gimbal/supercap；可阻塞/IPC */
    OM_INIT_LEVEL_LATE,         /**< 全就绪后自检/诊断；可阻塞/IPC */
    OM_INIT_LEVEL_COUNT,        /**< 级别总数（哨兵，用作 om_do_initcalls 的上界） */
} OmInitLevel;

/*===========================================================================
 * 注册宏
 *===========================================================================*/

#define OM_INIT_PASTE2(a, b) a##b
#define OM_INIT_PASTE(a, b)  OM_INIT_PASTE2(a, b)

/** @brief 分级别名的默认优先级（0-99，小者先） */
#ifndef OM_INIT_PRIO_DEFAULT
#define OM_INIT_PRIO_DEFAULT 50
#endif

/**
 * @brief 注册初始化回调到 .om_init 段（泛型，自选级别与优先级）
 *
 * @param func  回调函数名（OmInitFn：OmRet func(void)）
 * @param level 级别（OM_INIT_LEVEL_*）
 * @param prio  同级优先级 0-99，小者先执行
 *
 * 用 __COUNTER__ 生成唯一符号名，允许同一 func 在多个级别或多个 prio 注册
 * （对应 Linux __define_initcall 的 id 参数解决的"重复符号"问题）。
 */
#define OM_INIT(func, level, prio)                                          \
    OM_USED static const OmInitEntry OM_INIT_PASTE(om_init_entry_, __COUNTER__) \
        OM_SECTION(".om_init") = { (func), #func, (uint8_t)(level), (uint8_t)(prio) }

/**
 * @brief 分级别名（语法糖，默认优先级 OM_INIT_PRIO_DEFAULT）
 *
 * 用法示例：
 *   OM_INIT_BOARD(bsp_can_register);                       // 设备提供者
 *   OM_INIT(om_board_init, OM_INIT_LEVEL_BOARD, 0);        // 需 prio 0 时用泛型
 *   OM_INIT_DRIVER(can_adapter_init);                      // 设备消费者
 *   OM_INIT_SYSTEM(supercap_self_init);
 */
#define OM_INIT_EARLIEST(fn) OM_INIT(fn, OM_INIT_LEVEL_EARLIEST, OM_INIT_PRIO_DEFAULT)
#define OM_INIT_BOARD(fn)    OM_INIT(fn, OM_INIT_LEVEL_BOARD,    OM_INIT_PRIO_DEFAULT)
#define OM_INIT_DRIVER(fn)   OM_INIT(fn, OM_INIT_LEVEL_DRIVER,   OM_INIT_PRIO_DEFAULT)
#define OM_INIT_SERVICE(fn)  OM_INIT(fn, OM_INIT_LEVEL_SERVICE,  OM_INIT_PRIO_DEFAULT)
#define OM_INIT_SYSTEM(fn)   OM_INIT(fn, OM_INIT_LEVEL_SYSTEM,   OM_INIT_PRIO_DEFAULT)
#define OM_INIT_LATE(fn)     OM_INIT(fn, OM_INIT_LEVEL_LATE,     OM_INIT_PRIO_DEFAULT)

/*===========================================================================
 * 编排
 *===========================================================================*/

/**
 * @brief 执行 [level_lo, level_hi) 区间内所有已注册回调
 *
 * 遍历 .om_init 段，筛出级别落在 [level_lo, level_hi) 的表项，按 level 升序、
 * 同级 prio 升序排序后依次调用。
 *
 * 失败策略：默认记录首个失败的 {name, ret} 并继续执行其余回调，函数末尾返回该
 * 失败码（无失败返回 OM_OK）。定义 OM_INIT_ABORT_ON_FAIL 时，首个失败即立即返回。
 *
 * 典型用法（调度器上下文分裂）：
 *   - 调度器启动前（main）：
 *       om_do_initcalls(OM_INIT_LEVEL_EARLIEST, OM_INIT_LEVEL_SERVICE)
 *     跑 EARLIEST + BOARD + DRIVER（不可阻塞）；
 *   - 调度器启动后（init 线程）：
 *       om_do_initcalls(OM_INIT_LEVEL_SERVICE, OM_INIT_LEVEL_COUNT)
 *     跑 SERVICE + SYSTEM + LATE（可阻塞/IPC）。
 *
 * @param level_lo 起始级别（含）
 * @param level_hi 结束级别（不含），用 OM_INIT_LEVEL_COUNT 表示到末尾
 * @return OmRet  OM_OK 或首个失败回调的错误码
 */
OmRet om_do_initcalls(OmInitLevel level_lo, OmInitLevel level_hi);

#ifdef __cplusplus
}
#endif

#endif /* __OM_INIT_H__ */
