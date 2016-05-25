/*
 *
 * Copyright (c) 2014-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/slab.h>
#include <linux/msm_ion.h>
#include <soc/qcom/secure_buffer.h>
#include "ion.h"
#include "ion_priv.h"

struct ion_system_secure_heap {
	struct ion_heap *sys_heap;
	struct ion_heap heap;
};

static bool is_cp_flag_present(unsigned long flags)
{
	return flags && (ION_FLAG_CP_TOUCH ||
			ION_FLAG_CP_BITSTREAM ||
			ION_FLAG_CP_PIXEL ||
			ION_FLAG_CP_NON_PIXEL ||
			ION_FLAG_CP_CAMERA);
}

int ion_system_secure_heap_unassign_sg(struct sg_table *sgt, int source_vmid)
{
	u32 dest_vmid = VMID_HLOS;
	u32 dest_perms = PERM_READ | PERM_WRITE | PERM_EXEC;
	struct scatterlist *sg;
	int ret, i;

	ret = hyp_assign_table(sgt, &source_vmid, 1,
				&dest_vmid, &dest_perms, 1);
	if (ret) {
		pr_err("%s: Not freeing memory since assign call failed. VMID %d\n",
						__func__, source_vmid);
		return -ENXIO;
	}

	for_each_sg(sgt->sgl, sg, sgt->nents, i)
		ClearPagePrivate(sg_page(sg));
	return 0;
}

int ion_system_secure_heap_assign_sg(struct sg_table *sgt, int dest_vmid)
{
	u32 source_vmid = VMID_HLOS;
	u32 dest_perms = PERM_READ | PERM_WRITE;
	struct scatterlist *sg;
	int ret, i;

	ret = hyp_assign_table(sgt, &source_vmid, 1,
				&dest_vmid, &dest_perms, 1);
	if (ret) {
		pr_err("%s: Assign call failed. VMID %d\n",
						__func__, dest_vmid);
		return -EINVAL;
	}

	for_each_sg(sgt->sgl, sg, sgt->nents, i)
		SetPagePrivate(sg_page(sg));
	return 0;
}

static void ion_system_secure_heap_free(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_system_secure_heap *secure_heap = container_of(heap,
						struct ion_system_secure_heap,
						heap);
	buffer->heap = secure_heap->sys_heap;
	secure_heap->sys_heap->ops->free(buffer);
}

static int ion_system_secure_heap_allocate(struct ion_heap *heap,
					struct ion_buffer *buffer,
					unsigned long size, unsigned long align,
					unsigned long flags)
{
	int ret = 0;
	struct ion_system_secure_heap *secure_heap = container_of(heap,
						struct ion_system_secure_heap,
						heap);

	if (!ion_heap_is_system_secure_heap_type(secure_heap->heap.type) ||
		!is_cp_flag_present(flags)) {
		pr_info("%s: Incorrect heap type or incorrect flags\n",
								__func__);
		return -EINVAL;
	}

	ret = secure_heap->sys_heap->ops->allocate(secure_heap->sys_heap,
						buffer, size, align, flags);
	if (ret) {
		pr_info("%s: Failed to get allocation for %s, ret = %d\n",
			__func__, heap->name, ret);
		return ret;
	}
	return ret;
}

static struct sg_table *ion_system_secure_heap_map_dma(struct ion_heap *heap,
					struct ion_buffer *buffer)
{
	struct ion_system_secure_heap *secure_heap = container_of(heap,
						struct ion_system_secure_heap,
						heap);

	return secure_heap->sys_heap->ops->map_dma(secure_heap->sys_heap,
							buffer);
}

static void ion_system_secure_heap_unmap_dma(struct ion_heap *heap,
					struct ion_buffer *buffer)
{
	struct ion_system_secure_heap *secure_heap = container_of(heap,
						struct ion_system_secure_heap,
						heap);

	secure_heap->sys_heap->ops->map_dma(secure_heap->sys_heap,
							buffer);
}

static void *ion_system_secure_heap_map_kernel(struct ion_heap *heap,
					struct ion_buffer *buffer)
{
	pr_info("%s: Kernel mapping from secure heap %s disallowed\n",
		__func__, heap->name);
	return ERR_PTR(-EINVAL);
}

static void ion_system_secure_heap_unmap_kernel(struct ion_heap *heap,
				struct ion_buffer *buffer)
{
}

static int ion_system_secure_heap_map_user(struct ion_heap *mapper,
					struct ion_buffer *buffer,
					struct vm_area_struct *vma)
{
	pr_info("%s: Mapping from secure heap %s disallowed\n",
		__func__, mapper->name);
	return -EINVAL;
}

static int ion_system_secure_heap_shrink(struct ion_heap *heap, gfp_t gfp_mask,
						int nr_to_scan)
{
	struct ion_system_secure_heap *secure_heap = container_of(heap,
						struct ion_system_secure_heap,
						heap);

	return secure_heap->sys_heap->ops->shrink(secure_heap->sys_heap,
						gfp_mask, nr_to_scan);
}

static struct ion_heap_ops system_secure_heap_ops = {
	.allocate = ion_system_secure_heap_allocate,
	.free = ion_system_secure_heap_free,
	.map_dma = ion_system_secure_heap_map_dma,
	.unmap_dma = ion_system_secure_heap_unmap_dma,
	.map_kernel = ion_system_secure_heap_map_kernel,
	.unmap_kernel = ion_system_secure_heap_unmap_kernel,
	.map_user = ion_system_secure_heap_map_user,
	.shrink = ion_system_secure_heap_shrink,
};

struct ion_heap *ion_system_secure_heap_create(struct ion_platform_heap *unused)
{
	struct ion_system_secure_heap *heap;

	heap = kzalloc(sizeof(struct ion_system_secure_heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->heap.ops = &system_secure_heap_ops;
	heap->heap.type = ION_HEAP_TYPE_SYSTEM_SECURE;
	heap->heap.flags = ION_HEAP_FLAG_DEFER_FREE;
	heap->sys_heap = get_ion_heap(ION_SYSTEM_HEAP_ID);
	return &heap->heap;
}

void ion_system_secure_heap_destroy(struct ion_heap *heap)
{
	kfree(heap);
}
