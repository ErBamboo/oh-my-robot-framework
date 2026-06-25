#include "core/om_pre_init.h"

/*
 * .om_pre_init 段的边界符号由链接器定义：
 *
 *   GCC ld :  链接脚本中 PROVIDE(__om_pre_init_start = .)
 *             PROVIDE(__om_pre_init_end   = .)
 *
 *   armlink : scatter 文件将 .om_pre_init 放入独立执行域 ER_OM_PRE_INIT，
 *             再由板级 linker 目录中的符号别名文件将
 *             Image$$ER_OM_PRE_INIT$$Base / Limit 映射为
 *             __om_pre_init_start / __om_pre_init_end
 *
 * 框架只认这两个符号名，不区分链接器类型。
 */

extern const OmPreInitFunc __om_pre_init_start[];
extern const OmPreInitFunc __om_pre_init_end[];

/**
 * @brief 执行所有已注册的极早期预初始化回调
 */
void om_pre_init_run(void)
{
    const OmPreInitFunc *p;
    for (p = __om_pre_init_start; p < __om_pre_init_end; p++)
    {
        if (*p)
            (*p)();
    }
}
