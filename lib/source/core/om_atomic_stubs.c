/**
 * @file      om_atomic_stubs.c
 * @brief     Cortex-M0+ __atomic_* 兼容桩（ARMv6-M 无 ldrex/strex）
 *
 * TI Clang (LLVM) 对 Cortex-M0+ 不提供 __atomic_* 库函数。
 * M0+ 为单核，原子性通过关全局中断保证，替代硬件独占访问指令。
 *
 * 使用 om_hw_disable_interrupt() / om_hw_restore_interrupt() 作为
 * 平台无关的临界区入口，不直接依赖 port_* 或 CMSIS。
 */

#include <stdint.h>
#include "core/om_interrupt.h"

#define CRITICAL_ENTER(key)   uint32_t key = om_hw_disable_interrupt()
#define CRITICAL_EXIT(key)    om_hw_restore_interrupt(key)

/* --- 1-byte atomics --- */

uint8_t __atomic_load_1(const volatile void *ptr, int memorder)
{
    (void)memorder;
    CRITICAL_ENTER(k);
    uint8_t val = *(const volatile uint8_t *)ptr;
    CRITICAL_EXIT(k);
    return val;
}

void __atomic_store_1(volatile void *ptr, uint8_t val, int memorder)
{
    (void)memorder;
    CRITICAL_ENTER(k);
    *(volatile uint8_t *)ptr = val;
    CRITICAL_EXIT(k);
}

uint8_t __atomic_compare_exchange_1(volatile void *ptr, void *expected,
                                     uint8_t desired, int weak,
                                     int success, int failure)
{
    (void)weak; (void)success; (void)failure;
    CRITICAL_ENTER(k);
    uint8_t *exp = (uint8_t *)expected;
    uint8_t  cur = *(volatile uint8_t *)ptr;
    if (cur == *exp)
        *(volatile uint8_t *)ptr = desired;
    else
        *exp = cur;
    CRITICAL_EXIT(k);
    return (cur == *exp);
}

/* --- 4-byte atomics --- */

uint32_t __atomic_load_4(const volatile void *ptr, int memorder)
{
    (void)memorder;
    CRITICAL_ENTER(k);
    uint32_t val = *(const volatile uint32_t *)ptr;
    CRITICAL_EXIT(k);
    return val;
}

void __atomic_store_4(volatile void *ptr, uint32_t val, int memorder)
{
    (void)memorder;
    CRITICAL_ENTER(k);
    *(volatile uint32_t *)ptr = val;
    CRITICAL_EXIT(k);
}

uint32_t __atomic_fetch_or_4(volatile void *ptr, uint32_t val, int memorder)
{
    (void)memorder;
    CRITICAL_ENTER(k);
    uint32_t old = *(volatile uint32_t *)ptr;
    *(volatile uint32_t *)ptr = old | val;
    CRITICAL_EXIT(k);
    return old;
}

uint32_t __atomic_fetch_and_4(volatile void *ptr, uint32_t val, int memorder)
{
    (void)memorder;
    CRITICAL_ENTER(k);
    uint32_t old = *(volatile uint32_t *)ptr;
    *(volatile uint32_t *)ptr = old & val;
    CRITICAL_EXIT(k);
    return old;
}
