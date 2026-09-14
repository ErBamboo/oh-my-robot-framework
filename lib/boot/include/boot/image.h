/**
 * @file   image.h
 * @brief  镜像格式契约——bootloader 与 app 两个独立工程的共享 ABI
 *
 * 设计思想：
 * - 镜像 = 自描述二进制：定长头 + 对齐填充 + 负载 + 固定尺寸摘要区。引导方
 *   只凭镜像本身即可判定"是什么、多大、从哪开始、是否完整"，不依赖外部元数据。
 * - 头是**被摘要覆盖的只读元数据**：运行期可变状态（是否已确认、当前活动槽）
 *   一律放决策数据区，不回头里写——改写头即破坏摘要。
 * - 摘要覆盖 [0, digestOffset)，**不含摘要区自身**（否则递归）。
 * - 头在槽基址、负载在 payloadOffset：负载偏移必须等于 app 的链接偏移，
 *   且满足向量表对齐；两者不一致的镜像不可启动。
 * - 兼容性由版本字段表达（hdrVersion）：旧侧遇新结构必须**明确拒绝**，
 *   绝不按旧布局解读。字段布局或判定顺序的任何不兼容变更都要升版本。
 * - **地址不进本文件**：槽基址与容量来自工程侧分区配置，本文件只定义相对槽
 *   基址的偏移与尺寸；引导方按分区名取槽，代码中不出现地址字面量。
 *
 * 裁剪姿态：可裁剪组件（boot/OTA 消费才引入）。**纯定义头**——零依赖、不引
 *           osal/drivers、不产生代码，与任意 OS 形态（含无 OS）及裁剪组合兼容；
 *           消费方按需包含。
 *
 * 设计档案：docs/boot_ota/image_header_contract.md
 */

#ifndef __BOOT_IMAGE_H__
#define __BOOT_IMAGE_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * 契约常量（不变量——变更即升 OM_IMAGE_HDR_VERSION）
 *===========================================================================*/

/** 镜像魔数："OMRI"。取非 0x00000000 / 非 0xFFFFFFFF，使空白或擦除的存储
 *  天然不匹配（无需额外的"空槽"判据）。 */
#define OM_IMAGE_MAGIC 0x4F4D5249u

/** 头部布局版本。头结构发生**不兼容**变更时递增；引导方读到不认识的版本
 *  必须拒绝该槽，而不是按已知布局解读。 */
#define OM_IMAGE_HDR_VERSION 1u

/** 头部总长（= sizeof(OmImageHeader)）。同时是"头本体"与"负载偏移"两个
 *  概念的分界——二者刻意不共用一个字段。 */
#define OM_IMAGE_HDR_SIZE 64u

/** 负载偏移约定值：负载相对槽基址的偏移，也是 app 的链接偏移基准。
 *  取 0x200 是为满足向量表对齐（异常向量表基址须对齐到 条目数×4 向上取
 *  2 的幂）。镜像头里的 payloadOffset 字段是每镜像自描述值，正常应等于本值。 */
#define OM_IMAGE_PAYLOAD_OFFSET 0x200u

/** 摘要区预留容量（槽内负载之后）。预留恒定，使"摘要算法演进"不改变布局。 */
#define OM_IMAGE_DIGEST_REGION_SIZE 256u

/*===========================================================================
 * 头标志位（位值刻意不连续，保留空洞供演进）
 *===========================================================================*/

/** 不可直接启动（为资源镜像/从镜像预留）。 */
#define OM_IMAGE_F_NON_BOOTABLE 0x00000001u

/** 镜像已按某个槽地址构建（配合 slotId 校验）。 */
#define OM_IMAGE_F_SLOT_BOUND 0x00000100u

/** 摘要存于决策数据区而非槽内尾部（为"摘要搬家"预留）。 */
#define OM_IMAGE_F_DIGEST_IN_META 0x00000200u

/** 厂商/项目私有位起始。 */
#define OM_IMAGE_F_VENDOR_BASE 0x00010000u

/*===========================================================================
 * 枚举
 *===========================================================================*/

/** 摘要算法。**命名的是标准变体，不是任何芯片后端的实现口径**——同一 ID 在
 *  所有平台上必须产出逐字节相同的结果。 */
typedef enum OmImageDigestAlgo {
    OM_IMAGE_DIGEST_NONE           = 0, /**< 无摘要（不校验） */
    OM_IMAGE_DIGEST_CRC32_ISO_HDLC = 1, /**< CRC-32/ISO-HDLC（反射，初值与末尾异或全 1） */
    OM_IMAGE_DIGEST_SHA256         = 2, /**< SHA-256（需签名链配合才有防篡改能力） */
} OmImageDigestAlgo;

/** 槽标识（写入头内，供引导方比对）。 */
typedef enum OmImageSlot {
    OM_IMAGE_SLOT_A = 0,
    OM_IMAGE_SLOT_B = 1,
} OmImageSlot;

/*===========================================================================
 * 头部结构
 *===========================================================================*/

/**
 * @brief 镜像头（槽基址起 64 字节）
 *
 * 全字段小端、自然对齐（无跨边界访问）。头被摘要覆盖，摘要不覆盖自身。
 */
typedef struct OmImageHeader {
    uint32_t magic;            /**< 0  OM_IMAGE_MAGIC */
    uint16_t hdrVersion;       /**< 4  头布局版本 */
    uint16_t hdrSize;          /**< 6  头本体内字节数（= 结构体长度） */
    uint32_t payloadOffset;    /**< 8  负载相对槽基址的偏移（= app 链接偏移） */
    uint32_t imageSize;        /**< 12 负载长度：不含头、不含摘要区 */
    uint32_t imageTotalSize;   /**< 16 payloadOffset + imageSize + 摘要区预留 */
    uint32_t imageVersion;     /**< 20 见 OM_IMAGE_VERSION 编码 */
    uint32_t flags;            /**< 24 OM_IMAGE_F_* */
    uint16_t digestAlgo;       /**< 28 OmImageDigestAlgo */
    uint16_t digestLen;        /**< 30 摘要长度（显式给出，不靠算法推导） */
    uint32_t digestOffset;     /**< 32 摘要区起点（= payloadOffset + imageSize） */
    uint16_t digestRegionSize; /**< 36 摘要区**预留容量**（非已用长度） */
    uint16_t slotId;           /**< 38 OmImageSlot */
    uint32_t headerCrc32;      /**< 40 头自身完整性（当前约定：恒 0 = 未启用） */
    uint8_t reserved[20];      /**< 44 恒 0xFF，供兼容性加字段 */
} OmImageHeader;

/** 版本号编码：major<<24 | minor<<16 | patch<<8（低位字节留给构建方自定义）。 */
#define OM_IMAGE_VERSION(major, minor, patch)                                    \
    ((((uint32_t)(major) & 0xFFu) << 24) | (((uint32_t)(minor) & 0xFFu) << 16) | \
     (((uint32_t)(patch) & 0xFFu) << 8))

#define OM_IMAGE_VERSION_MAJOR(v) ((uint8_t)(((v) >> 24) & 0xFFu))
#define OM_IMAGE_VERSION_MINOR(v) ((uint8_t)(((v) >> 16) & 0xFFu))
#define OM_IMAGE_VERSION_PATCH(v) ((uint8_t)(((v) >> 8) & 0xFFu))

_Static_assert(sizeof(OmImageHeader) == OM_IMAGE_HDR_SIZE, "OmImageHeader 必须为 64 字节");
_Static_assert(offsetof(OmImageHeader, magic) == 0, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, hdrVersion) == 4, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, hdrSize) == 6, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, payloadOffset) == 8, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, imageSize) == 12, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, imageTotalSize) == 16, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, imageVersion) == 20, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, flags) == 24, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, digestAlgo) == 28, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, digestLen) == 30, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, digestOffset) == 32, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, digestRegionSize) == 36, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, slotId) == 38, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, headerCrc32) == 40, "字段偏移与契约不符");
_Static_assert(offsetof(OmImageHeader, reserved) == 44, "字段偏移与契约不符");

/*===========================================================================
 * 槽内布局
 *
 *   槽基址 + 0x000   头（64B）                  ┐
 *          + 填充（0xFF）至 payloadOffset        ├─ 摘要覆盖 [0, digestOffset)
 *          + payloadOffset   负载               ┘
 *          + digestOffset    摘要区（预留 256B，不参与自身摘要）
 *===========================================================================*/

/*===========================================================================
 * 判定顺序（契约的一部分）
 *
 * 引导方读头后必须按此顺序判定，**尺寸类校验先于任何基于头内尺寸的访问**：
 *   1. 读满头（读失败 → 该槽不可用）
 *   2. magic 不符                        → 拒绝该槽（含"空白槽"语义）
 *   3. hdrVersion 不符                   → 拒绝该槽
 *   4. hdrSize != sizeof(头)             → 拒绝
 *   5. flags 含 NON_BOOTABLE             → 拒绝
 *   6. 四个尺寸字段范围自洽（加法不得溢出）
 *      且 imageTotalSize <= 槽容量       → 否则拒绝
 *   7. payloadOffset 对齐且 >= hdrSize   → 否则拒绝
 *   8. digestAlgo 未知                   → 拒绝（不得用未知算法尝试校验）
 *   9. digestLen 与算法不符              → 拒绝
 *  10. 按算法计算 [0, digestOffset) 的摘要并与摘要区比对 → 不符则拒绝
 *
 * 拒绝后的动作：标记该槽不可用 → 试另一槽 → 全不可用则进交互（维护）状态。
 * 不搬移、不擦除。
 *===========================================================================*/

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_IMAGE_H__ */
