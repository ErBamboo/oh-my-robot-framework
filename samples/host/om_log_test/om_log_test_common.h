#ifndef __OM_LOG_TEST_COMMON_H__
#define __OM_LOG_TEST_COMMON_H__

#include <stdio.h>

/* 断言：失败打印并置全局失败标志（main 返回非 0） */
#define EXPECT(cond)                                               \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_log_test_failed = 1;                                 \
        }                                                          \
    } while (0)

extern int g_log_test_failed;

#endif /* __OM_LOG_TEST_COMMON_H__ */
