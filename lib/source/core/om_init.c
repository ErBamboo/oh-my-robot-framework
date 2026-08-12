#include "core/om_init.h"

/*
 * .om_init 段的边界符号由链接器定义：
 *
 *   GCC ld :  链接脚本中 PROVIDE(__om_init_start = .); KEEP(*(.om_init));
 *             PROVIDE(__om_init_end = .)
 *
 *   armlink : scatter 文件将 .om_init 放入独立执行域 ER_OM_INIT，
 *             armlink 自动生成 Image$$ER_OM_INIT$$Base / Limit。
 *             armclang 允许 $ 出现在标识符中，故直接 extern 引用这两个
 *             符号并宏重命名为统一名称（避免 .set 别名被最终符号表省略）。
 *
 * 框架代码统一使用 __om_init_start / __om_init_end，不区分链接器类型。
 */

#if defined(__ARMCC_VERSION)
#define __om_init_start Image$$ER_OM_INIT$$Base
#define __om_init_end   Image$$ER_OM_INIT$$Limit
#endif

extern const OmInitEntry __om_init_start[];
extern const OmInitEntry __om_init_end[];

/**
 * @brief 排序缓冲上限（栈上指针数组）。
 * @details 区间内表项数 <= 该上限时按 (level, prio) 排序后执行；
 *          超出时退化为按链接顺序执行（全部仍会执行，但不保证顺序）。
 *          级"只增不删"，正常工程下表项数远小于该值；如需更大请外部定义覆盖。
 */
#ifndef OM_INIT_MAX_ENTRIES
#define OM_INIT_MAX_ENTRIES 128
#endif

/* 首个失败记录（无日志子系统时供诊断查询；后续对接 log/诊断服务） */
static const char *s_first_fail_name;
static OmRet       s_first_fail_ret;

/** @brief 合成单一排序键：level 占高位、prio 占低位，升序即"先按层、同级按 prio" */
static inline int om_init_key(const OmInitEntry *e)
{
    return ((int)e->level << 8) | (int)e->prio;
}

/** @brief 调用单个回调并登记首个失败；返回是否失败 */
static OmRet om_init_call_one(const OmInitEntry *e)
{
    OmRet ret = e->fn();
    if (ret == OM_OK)
    {
        return OM_OK;
    }
    if (s_first_fail_name == NULL)
    {
        s_first_fail_name = e->name;
        s_first_fail_ret  = ret;
    }
    return ret;
}

OmRet om_do_initcalls(OmInitLevel level_lo, OmInitLevel level_hi)
{
    const uint8_t lo = (uint8_t)level_lo;
    const uint8_t hi = (uint8_t)level_hi;

    s_first_fail_name = NULL;
    s_first_fail_ret  = OM_OK;

    /* 统计区间内表项数，决定是否走排序路径 */
    size_t total = 0;
    for (const OmInitEntry *p = __om_init_start; p < __om_init_end; p++)
    {
        if (p->fn != NULL && p->level >= lo && p->level < hi)
        {
            total++;
        }
    }

    if (total <= OM_INIT_MAX_ENTRIES)
    {
        /* 收集到栈缓冲并按 (level, prio) 选择排序 */
        const OmInitEntry *order[OM_INIT_MAX_ENTRIES];
        size_t count = 0;
        for (const OmInitEntry *p = __om_init_start; p < __om_init_end; p++)
        {
            if (p->fn != NULL && p->level >= lo && p->level < hi)
            {
                order[count++] = p;
            }
        }

        for (size_t i = 0; i + 1 < count; i++)
        {
            size_t min = i;
            for (size_t j = i + 1; j < count; j++)
            {
                if (om_init_key(order[j]) < om_init_key(order[min]))
                {
                    min = j;
                }
            }
            if (min != i)
            {
                const OmInitEntry *tmp = order[i];
                order[i]   = order[min];
                order[min] = tmp;
            }
        }

        for (size_t i = 0; i < count; i++)
        {
#ifdef OM_INIT_ABORT_ON_FAIL
            if (om_init_call_one(order[i]) != OM_OK)
            {
                return s_first_fail_ret;
            }
#else
            (void)om_init_call_one(order[i]);
#endif
        }
    }
    else
    {
        /* 超出缓冲：退化为链接顺序全执行（应调大 OM_INIT_MAX_ENTRIES） */
        for (const OmInitEntry *p = __om_init_start; p < __om_init_end; p++)
        {
            if (p->fn != NULL && p->level >= lo && p->level < hi)
            {
#ifdef OM_INIT_ABORT_ON_FAIL
                if (om_init_call_one(p) != OM_OK)
                {
                    return s_first_fail_ret;
                }
#else
                (void)om_init_call_one(p);
#endif
            }
        }
    }

    return s_first_fail_ret;
}
