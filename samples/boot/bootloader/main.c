/**
 * @file main.c
 * @brief Bootloader 测试样例（os=none）
 *
 * 职责：
 * - 执行调度器前的板级/驱动初始化；
 * - 校验生成的布局分区表；
 * - 读取 meta 的最新有效 entry；
 * - 读取 A/B 槽头部 magic，供调试器确认槽状态；
 * - 把结果放进固定符号 g_boot_sample_state，供 J-Link/Ozone 直接观察。
 *
 * 本样例只读，不写 meta、不擦除、不跳转。启动决策与跳转属于 M2/M3，
 * 后续在 state 结构预留的阶段上接入，不在样例里提前实现。
 */

#include <stdint.h>

#include "boot/meta.h"
#include "core/om_def.h"
#include "core/om_init.h"
#include "drivers/storage/partition.h"
#include "stm32f4xx.h"

#include "boot_partition_data.h"
#include "om_bootcfg.h"

#if !defined(OM_BOOT_VARIANT_BOOTLOADER)
#error "bootloader sample requires OM_BOOT_VARIANT_BOOTLOADER"
#endif

/** 状态结构魔数：小端内存字节为 "OMBS"。 */
#define BOOT_SAMPLE_MAGIC 0x534D424Fu

/** 读槽头的错误哨兵，与真实 magic 区分。 */
#define BOOT_SAMPLE_OPEN_FAILED 0xFFFFFFFDu
#define BOOT_SAMPLE_READ_FAILED 0xFFFFFFFEu
#define BOOT_SAMPLE_BLANK 0xFFFFFFFFu

/** 引导阶段；调试器按该字段判断样例执行到哪里。 */
typedef enum OmBootSampleStage
{
    OM_BOOT_SAMPLE_STAGE_ENTRY = 0,     /**< main 入口 */
    OM_BOOT_SAMPLE_STAGE_PRE_SCHED = 1, /**< 调度器前初始化完成 */
    OM_BOOT_SAMPLE_STAGE_PARTITION = 2, /**< 分区表校验完成 */
    OM_BOOT_SAMPLE_STAGE_META = 3,      /**< meta 读取完成 */
    OM_BOOT_SAMPLE_STAGE_SLOTS = 4,     /**< 槽头读取完成 */
    OM_BOOT_SAMPLE_STAGE_READY = 5,     /**< 样例就绪，等待 M2/M3 接入 */
    OM_BOOT_SAMPLE_STAGE_ERROR = 6,     /**< 初始化失败 */
} OmBootSampleStage;

/** 固定布局的状态结构；只读诊断，不参与启动决策。 */
typedef struct OmBootSampleState
{
    uint32_t magic;         /**< BOOT_SAMPLE_MAGIC */
    uint32_t stage;         /**< OmBootSampleStage */
    uint32_t preScheduler;  /**< om_startup_pre_scheduler() 返回值 */
    uint32_t vtor;          /**< SCB->VTOR */
    uint32_t layout;        /**< OM_BOOT_LAYOUT_FINGERPRINT */
    uint32_t registryCount; /**< 分区条目数 */
    uint32_t registryValid; /**< om_partition_registry_validate() 返回值 */
    uint32_t metaOpen;      /**< om_partition_open("meta") 返回值 */
    uint32_t metaInit;      /**< om_boot_meta_init() 返回值 */
    uint32_t metaRead;      /**< om_boot_meta_read() 返回值 */
    uint32_t metaCopy;      /**< 最新有效 entry 所在副本 */
    uint32_t metaSeq;       /**< entry.seq */
    uint32_t metaSlot;      /**< entry.activeSlot */
    uint32_t metaState;     /**< entry.state */
    uint32_t metaBootCount; /**< entry.bootCount */
    uint32_t metaVersion;   /**< entry.imageVersion */
    uint32_t metaFlags;     /**< entry.flags（高 16 位为布局指纹） */
    uint32_t metaReason;    /**< entry.reason */
    uint32_t slotAMagic;    /**< app_a 槽头 magic；见 BOOT_SAMPLE_* 哨兵 */
    uint32_t slotBMagic;    /**< app_b 槽头 magic；见 BOOT_SAMPLE_* 哨兵 */
} OmBootSampleState;

volatile OmBootSampleState g_boot_sample_state;

/** @brief 读槽头 magic；只读，不解释镜像。 */
static uint32_t slot_header_magic(const OmPartitionRegistry *registry, const char *name)
{
    OmPartitionHandle handle;
    uint32_t magic = BOOT_SAMPLE_BLANK;
    OmRet ret = om_partition_open(registry, name, &handle);
    if (ret != OM_OK)
    {
        return BOOT_SAMPLE_OPEN_FAILED;
    }
    ret = om_partition_read(&handle, 0u, &magic, sizeof(magic));
    if (ret != OM_OK)
    {
        return BOOT_SAMPLE_READ_FAILED;
    }
    return magic;
}

int main(void)
{
    g_boot_sample_state.magic = BOOT_SAMPLE_MAGIC;
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_ENTRY;
    g_boot_sample_state.vtor = SCB->VTOR;
    g_boot_sample_state.layout = OM_BOOT_LAYOUT_FINGERPRINT;

    OmRet ret = om_startup_pre_scheduler();
    g_boot_sample_state.preScheduler = (uint32_t)ret;
    if (ret != OM_OK)
    {
        g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_ERROR;
        for (;;)
        {
            __WFI();
        }
    }
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_PRE_SCHED;

    const OmPartitionRegistry *registry = om_boot_partition_registry();
    g_boot_sample_state.registryCount = om_partition_registry_count(registry);
    g_boot_sample_state.registryValid =
        (uint32_t)om_partition_registry_validate(registry);
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_PARTITION;

    OmPartitionHandle meta_handle;
    ret = om_partition_open(registry, "meta", &meta_handle);
    g_boot_sample_state.metaOpen = (uint32_t)ret;
    if (ret == OM_OK)
    {
        OmBootMeta meta;
        ret = om_boot_meta_init(&meta, &meta_handle);
        g_boot_sample_state.metaInit = (uint32_t)ret;
        if (ret == OM_OK)
        {
            OmBootMetaEntry entry;
            uint32_t copy_index = 0u;
            OmRet read_ret = om_boot_meta_read(&meta, &entry, &copy_index);
            g_boot_sample_state.metaRead = (uint32_t)read_ret;
            if (read_ret == OM_OK)
            {
                g_boot_sample_state.metaCopy = copy_index;
                g_boot_sample_state.metaSeq = entry.seq;
                g_boot_sample_state.metaSlot = entry.activeSlot;
                g_boot_sample_state.metaState = entry.state;
                g_boot_sample_state.metaBootCount = entry.bootCount;
                g_boot_sample_state.metaVersion = entry.imageVersion;
                g_boot_sample_state.metaFlags = entry.flags;
                g_boot_sample_state.metaReason = entry.reason;
            }
        }
    }
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_META;

    g_boot_sample_state.slotAMagic = slot_header_magic(registry, "app_a");
    g_boot_sample_state.slotBMagic = slot_header_magic(registry, "app_b");
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_READY;

    /* 启动决策/跳转/下载在此接入；当前保持可观察的空闲状态。 */
    for (;;)
    {
        __WFI();
    }
}
