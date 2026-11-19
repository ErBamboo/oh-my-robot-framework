/**
 * @file main.c
 * @brief Bootloader 测试样例（os=none）
 *
 * 职责：
 * - 执行调度器前的板级/驱动初始化；
 * - 校验生成的布局分区表；
 * - 构造 Meta 与两个 Image 对象，各执行一次读取/校验；
 * - 把结果放进固定符号 g_boot_sample_state，供 J-Link/Ozone 观察。
 *
 * 决策与跳转属于后续阶段，本样例在对象评估完成后保持可观察的空闲状态。
 */

#include <stdint.h>
#include <string.h>

#include "boot/image.h"
#include "boot/image_verifier.h"
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

/** 引导阶段；调试器按该字段判断样例执行到哪里。 */
typedef enum OmBootSampleStage
{
    OM_BOOT_SAMPLE_STAGE_ENTRY = 0,       /**< main 入口 */
    OM_BOOT_SAMPLE_STAGE_PRE_SCHED = 1,   /**< 调度器前初始化完成 */
    OM_BOOT_SAMPLE_STAGE_PARTITION = 2,   /**< 分区表校验完成 */
    OM_BOOT_SAMPLE_STAGE_OBJECT_INIT = 3, /**< Meta 与 Image 对象初始化完成 */
    OM_BOOT_SAMPLE_STAGE_OBJECT_RUN = 4,  /**< 读取与评估完成 */
    OM_BOOT_SAMPLE_STAGE_READY = 5,       /**< 样例就绪 */
    OM_BOOT_SAMPLE_STAGE_ERROR = 6,       /**< 初始化失败 */
} OmBootSampleStage;

/** 固定布局的状态结构；只读诊断。 */
typedef struct OmBootSampleState
{
    uint32_t magic;                 /**< BOOT_SAMPLE_MAGIC */
    uint32_t stage;                 /**< OmBootSampleStage */
    uint32_t preScheduler;          /**< om_startup_pre_scheduler() 返回值 */
    uint32_t vtor;                  /**< SCB->VTOR */
    uint32_t layout;                /**< OM_BOOT_LAYOUT_FINGERPRINT */
    uint32_t registryCount;         /**< 分区条目数 */
    uint32_t registryValid;         /**< om_partition_registry_validate() 返回值 */
    uint32_t verifierInit;          /**< om_boot_image_verifier_init() 返回值 */
    uint32_t metaReady;             /**< meta 分区是否可用 */
    uint32_t metaStatus;            /**< OmBootMetaReadStatus */
    uint32_t metaCopy;              /**< 最新记录所在副本 */
    uint32_t metaSeq;               /**< 最新记录 seq */
    uint32_t metaState;             /**< OmBootMetaState */
    uint32_t metaSlot;              /**< activeSlot */
    uint32_t metaBootCount;         /**< bootCount */
    uint32_t metaVersion;           /**< imageVersion */
    uint32_t metaConfirmedMask;     /**< 按槽确认位 */
    uint32_t metaTrialFromRollback; /**< 回退试用标记 */
    uint32_t metaReason;            /**< OmBootMetaReason */
    uint32_t imageAReady;           /**< 镜像 A 对象是否可用 */
    uint32_t imageAVerify;          /**< OmBootImageStatus */
    uint32_t imageAValid;           /**< 0/1 */
    uint32_t imageAVersion;         /**< 镜像版本 */
    uint32_t imageBReady;           /**< 镜像 B 对象是否可用 */
    uint32_t imageBVerify;          /**< OmBootImageStatus */
    uint32_t imageBValid;           /**< 0/1 */
    uint32_t imageBVersion;         /**< 镜像版本 */
} OmBootSampleState;

volatile OmBootSampleState g_boot_sample_state;

int main(void)
{
    g_boot_sample_state.magic = BOOT_SAMPLE_MAGIC;
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_ENTRY;
    g_boot_sample_state.vtor = SCB->VTOR;
    g_boot_sample_state.layout = OM_BOOT_LAYOUT_FINGERPRINT;

    OmRet ret = om_startup_pre_scheduler(); /* 分阶段调用的返回码 */
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

    const OmPartitionRegistry *registry = om_boot_partition_registry(); /* 生成的分区表 */
    g_boot_sample_state.registryCount = om_partition_registry_count(registry);
    g_boot_sample_state.registryValid = (uint32_t)om_partition_registry_validate(registry);
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_PARTITION;

    OmPartitionHandle meta_part;   /* meta 分区句柄 */
    OmPartitionHandle slot_a_part; /* 槽 A 分区句柄 */
    OmPartitionHandle slot_b_part; /* 槽 B 分区句柄 */

    OmBool meta_open = (om_partition_open(registry, "meta", &meta_part) == OM_OK) ? OM_TRUE : OM_FALSE;
    OmBool slot_a_open = (om_partition_open(registry, "app_a", &slot_a_part) == OM_OK) ? OM_TRUE : OM_FALSE;
    OmBool slot_b_open = (om_partition_open(registry, "app_b", &slot_b_part) == OM_OK) ? OM_TRUE : OM_FALSE;

    OmBootImageVerifier verifier;                 /* 镜像校验器 */
    const OmBootImageVerifyAlgorithm *algorithms; /* 内置算法表 */
    uint32_t algorithm_count = 0u;                /* 算法表项数 */

    algorithms = om_boot_image_verify_algorithms(&algorithm_count);
    ret = om_boot_image_verifier_init(&verifier, algorithms, algorithm_count);
    g_boot_sample_state.verifierInit = (uint32_t)ret;
    if (ret != OM_OK)
    {
        g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_ERROR;
        for (;;)
        {
            __WFI();
        }
    }

    OmBootMeta meta; /* meta 对象 */
    if (meta_open == OM_TRUE && om_boot_meta_init(&meta, &meta_part) == OM_OK)
    {
        g_boot_sample_state.metaReady = 1u;
    }
    else
    {
        g_boot_sample_state.metaReady = 0u;
    }

    OmBootImage images[OM_BOOT_SLOT_COUNT];                        /* 两个镜像对象 */
    OmBool image_ready[OM_BOOT_SLOT_COUNT] = {OM_FALSE, OM_FALSE}; /* 镜像对象是否可用 */
    if (slot_a_open == OM_TRUE &&
        om_boot_image_init(&images[0], &slot_a_part, 0u, &verifier) == OM_OK)
    {
        image_ready[0] = OM_TRUE;
    }
    if (slot_b_open == OM_TRUE &&
        om_boot_image_init(&images[1], &slot_b_part, 1u, &verifier) == OM_OK)
    {
        image_ready[1] = OM_TRUE;
    }
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_OBJECT_INIT;

    OmBootMetaReadResult meta_result; /* meta 读取结果 */
    if (g_boot_sample_state.metaReady != 0u)
    {
        (void)om_boot_meta_read(&meta, &meta_result);
    }
    else
    {
        memset(&meta_result, 0, sizeof(meta_result));
        meta_result.status = OM_BOOT_META_READ_IO_ERROR;
    }

    OmBootImageResult image_a; /* 镜像 A 校验结果 */
    OmBootImageResult image_b; /* 镜像 B 校验结果 */
    memset(&image_a, 0, sizeof(image_a));
    memset(&image_b, 0, sizeof(image_b));
    if (image_ready[0] == OM_TRUE)
    {
        (void)om_boot_image_verify(&images[0], &image_a);
    }
    else
    {
        image_a.status = OM_BOOT_IMAGE_READ_ERROR;
    }
    if (image_ready[1] == OM_TRUE)
    {
        (void)om_boot_image_verify(&images[1], &image_b);
    }
    else
    {
        image_b.status = OM_BOOT_IMAGE_READ_ERROR;
    }

    g_boot_sample_state.imageAReady = (image_ready[0] == OM_TRUE) ? 1u : 0u;
    g_boot_sample_state.imageAVerify = (uint32_t)image_a.status;
    g_boot_sample_state.imageAValid =
        (image_a.status == OM_BOOT_IMAGE_VALID) ? 1u : 0u;
    g_boot_sample_state.imageAVersion =
        (image_a.status == OM_BOOT_IMAGE_VALID) ? image_a.info.imageVersion : 0u;
    g_boot_sample_state.imageBReady = (image_ready[1] == OM_TRUE) ? 1u : 0u;
    g_boot_sample_state.imageBVerify = (uint32_t)image_b.status;
    g_boot_sample_state.imageBValid =
        (image_b.status == OM_BOOT_IMAGE_VALID) ? 1u : 0u;
    g_boot_sample_state.imageBVersion =
        (image_b.status == OM_BOOT_IMAGE_VALID) ? image_b.info.imageVersion : 0u;

    g_boot_sample_state.metaStatus = (uint32_t)meta_result.status;
    if (meta_result.status == OM_BOOT_META_READ_OK)
    {
        g_boot_sample_state.metaCopy = meta_result.record.copyIndex;
        g_boot_sample_state.metaSeq = meta_result.record.seq;
        g_boot_sample_state.metaState = (uint32_t)meta_result.record.data.state;
        g_boot_sample_state.metaSlot = meta_result.record.data.activeSlot;
        g_boot_sample_state.metaBootCount = meta_result.record.data.bootCount;
        g_boot_sample_state.metaVersion = meta_result.record.data.imageVersion;
        g_boot_sample_state.metaConfirmedMask =
            (meta_result.record.data.confirmed[0] == OM_TRUE ? OM_BOOT_META_FLAG_SLOT_A_CONFIRMED : 0u) |
            (meta_result.record.data.confirmed[1] == OM_TRUE ? OM_BOOT_META_FLAG_SLOT_B_CONFIRMED : 0u);
        g_boot_sample_state.metaTrialFromRollback =
            (uint32_t)meta_result.record.data.trialFromRollback;
        g_boot_sample_state.metaReason = (uint32_t)meta_result.record.data.reason;
    }

    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_OBJECT_RUN;
    g_boot_sample_state.stage = OM_BOOT_SAMPLE_STAGE_READY;

    /* 决策与跳转在此接入；当前保持可观察的空闲状态。 */
    for (;;)
    {
        __WFI();
    }
}
