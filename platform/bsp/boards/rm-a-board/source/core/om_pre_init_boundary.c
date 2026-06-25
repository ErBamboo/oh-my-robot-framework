/**
 * @file    om_pre_init_boundary.c
 * @brief   armlink 符号别名：Image$$ → __om_pre_init
 * @details armlink scatter 文件将 .om_pre_init 放入独立执行域 ER_OM_PRE_INIT，
 *          armlink 自动生成 Image$$ER_OM_PRE_INIT$$Base / Limit 边界符号。
 *          本文件通过内联汇编将这两个符号映射为框架约定的统一名称
 *          __om_pre_init_start / __om_pre_init_end。
 *
 *          GCC ld 的符号由链接脚本 PROVIDE 直接提供，不需要此文件。
 *          此处由 #ifdef __ARMCC_VERSION 保证仅 armclang 工具链生效。
 */

#ifdef __ARMCC_VERSION
__asm__(
    ".global __om_pre_init_start\n\t"
    ".global __om_pre_init_end\n\t"
    ".set    __om_pre_init_start, Image$$ER_OM_PRE_INIT$$Base\n\t"
    ".set    __om_pre_init_end,   Image$$ER_OM_PRE_INIT$$Limit\n\t"
);
#endif /* __ARMCC_VERSION */
