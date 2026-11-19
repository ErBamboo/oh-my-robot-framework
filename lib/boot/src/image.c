/**
 * @file   image.c
 * @brief  镜像对象实现
 */

#include "boot/image.h"

#include <string.h>

#include "boot/contract/image.h"
#include "boot/image_verifier.h"

OmRet om_boot_image_init(OmBootImage *image, const OmPartitionHandle *partition,
                         uint32_t slot_id, const OmBootImageVerifier *verifier)
{
    if (image == NULL || partition == NULL || verifier == NULL ||
        slot_id > (uint32_t)OM_IMAGE_SLOT_B)
    {
        return OM_ERR_INVALID_ARG;
    }
    if (om_partition_capacity(partition) == 0u)
    {
        return OM_ERR_INVALID_ARG;
    }

    memset(image, 0, sizeof(*image));
    image->partition = *partition;
    image->slotId = slot_id;
    image->verifier = verifier;
    return OM_OK;
}

OmRet om_boot_image_verify(const OmBootImage *image, OmBootImageResult *result)
{
    if (image == NULL || result == NULL || image->verifier == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }
    return om_boot_image_verifier_verify(image->verifier, &image->partition, image->slotId,
                                         result);
}

uint32_t om_boot_image_slot_id(const OmBootImage *image)
{
    return (image != NULL) ? image->slotId : 0u;
}

const OmPartitionHandle *om_boot_image_partition(const OmBootImage *image)
{
    return (image != NULL) ? &image->partition : NULL;
}
