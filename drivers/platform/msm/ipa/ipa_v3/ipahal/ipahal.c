/* Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "ipahal.h"
#include "ipahal_i.h"
#include "ipahal_reg_i.h"

static int ipahal_imm_cmd_init(enum ipa_hw_type ipa_hw_type);

struct ipahal_context *ipahal_ctx;

static const char *ipahal_imm_cmd_name_to_str[IPA_IMM_CMD_MAX] = {
	__stringify(IPA_IMM_CMD_IP_V4_FILTER_INIT),
	__stringify(IPA_IMM_CMD_IP_V6_FILTER_INIT),
	__stringify(IPA_IMM_CMD_IP_V4_NAT_INIT),
	__stringify(IPA_IMM_CMD_IP_V4_ROUTING_INIT),
	__stringify(IPA_IMM_CMD_IP_V6_ROUTING_INIT),
	__stringify(IPA_IMM_CMD_HDR_INIT_LOCAL),
	__stringify(IPA_IMM_CMD_HDR_INIT_SYSTEM),
	__stringify(IPA_IMM_CMD_REGISTER_WRITE),
	__stringify(IPA_IMM_CMD_NAT_DMA),
	__stringify(IPA_IMM_CMD_IP_PACKET_INIT),
	__stringify(IPA_IMM_CMD_DMA_SHARED_MEM),
	__stringify(IPA_IMM_CMD_IP_PACKET_TAG_STATUS),
	__stringify(IPA_IMM_CMD_DMA_TASK_32B_ADDR),
};

#define IPAHAL_MEM_ALLOC(__size, __is_atomic_ctx) \
		(kzalloc((__size), ((__is_atomic_ctx)?GFP_ATOMIC:GFP_KERNEL)))


static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_dma_task_32b_addr(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_dma_task_32b_addr *data;
	struct ipahal_imm_cmd_dma_task_32b_addr *dma_params =
		(struct ipahal_imm_cmd_dma_task_32b_addr *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_dma_task_32b_addr *)pyld->data;

	if (unlikely(dma_params->size1 & ~0xFFFF)) {
		IPAHAL_ERR("Size1 is bigger than 16bit width 0x%x\n",
			dma_params->size1);
		WARN_ON(1);
	}
	if (unlikely(dma_params->packet_size & ~0xFFFF)) {
		IPAHAL_ERR("Pkt size is bigger than 16bit width 0x%x\n",
			dma_params->packet_size);
		WARN_ON(1);
	}
	data->cmplt = dma_params->cmplt ? 1 : 0;
	data->eof = dma_params->eof ? 1 : 0;
	data->flsh = dma_params->flsh ? 1 : 0;
	data->lock = dma_params->lock ? 1 : 0;
	data->unlock = dma_params->unlock ? 1 : 0;
	data->size1 = dma_params->size1;
	data->addr1 = dma_params->addr1;
	data->packet_size = dma_params->packet_size;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_ip_packet_tag_status(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_packet_tag_status *data;
	struct ipahal_imm_cmd_ip_packet_tag_status *tag_params =
		(struct ipahal_imm_cmd_ip_packet_tag_status *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_ip_packet_tag_status *)pyld->data;

	if (unlikely(tag_params->tag & ~0xFFFFFFFFFFFF)) {
		IPAHAL_ERR("tag is bigger than 48bit width 0x%llx\n",
			tag_params->tag);
		WARN_ON(1);
	}
	data->tag = tag_params->tag;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_dma_shared_mem(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_dma_shared_mem *data;
	struct ipahal_imm_cmd_dma_shared_mem *mem_params =
		(struct ipahal_imm_cmd_dma_shared_mem *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_dma_shared_mem *)pyld->data;

	if (unlikely(mem_params->size & ~0xFFFF)) {
		IPAHAL_ERR("Size is bigger than 16bit width 0x%x\n",
			mem_params->size);
		WARN_ON(1);
	}
	if (unlikely(mem_params->local_addr & ~0xFFFF)) {
		IPAHAL_ERR("Local addr is bigger than 16bit width 0x%x\n",
			mem_params->local_addr);
		WARN_ON(1);
	}
	data->direction = mem_params->is_read ? 1 : 0;
	data->size = mem_params->size;
	data->local_addr = mem_params->local_addr;
	data->system_addr = mem_params->system_addr;
	data->skip_pipeline_clear = mem_params->skip_pipeline_clear ? 1 : 0;
	switch (mem_params->pipeline_clear_options) {
	case IPAHAL_HPS_CLEAR:
		data->pipeline_clear_options = 0;
		break;
	case IPAHAL_SRC_GRP_CLEAR:
		data->pipeline_clear_options = 1;
		break;
	case IPAHAL_FULL_PIPELINE_CLEAR:
		data->pipeline_clear_options = 2;
		break;
	default:
		IPAHAL_ERR("unsupported pipline clear option %d\n",
			mem_params->pipeline_clear_options);
		WARN_ON(1);
	};

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_register_write(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_register_write *data;
	struct ipahal_imm_cmd_register_write *regwrt_params =
		(struct ipahal_imm_cmd_register_write *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_register_write *)pyld->data;

	if (unlikely(regwrt_params->offset & ~0xFFFF)) {
		IPAHAL_ERR("Offset is bigger than 16bit width 0x%x\n",
			regwrt_params->offset);
		WARN_ON(1);
	}
	data->offset = regwrt_params->offset;
	data->value = regwrt_params->value;
	data->value_mask = regwrt_params->value_mask;

	data->skip_pipeline_clear = regwrt_params->skip_pipeline_clear ? 1 : 0;
	switch (regwrt_params->pipeline_clear_options) {
	case IPAHAL_HPS_CLEAR:
		data->pipeline_clear_options = 0;
		break;
	case IPAHAL_SRC_GRP_CLEAR:
		data->pipeline_clear_options = 1;
		break;
	case IPAHAL_FULL_PIPELINE_CLEAR:
		data->pipeline_clear_options = 2;
		break;
	default:
		IPAHAL_ERR("unsupported pipline clear option %d\n",
			regwrt_params->pipeline_clear_options);
		WARN_ON(1);
	};

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_ip_packet_init(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_packet_init *data;
	struct ipahal_imm_cmd_ip_packet_init *pktinit_params =
		(struct ipahal_imm_cmd_ip_packet_init *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_ip_packet_init *)pyld->data;

	if (unlikely(pktinit_params->destination_pipe_index & ~0x1F)) {
		IPAHAL_ERR("Dst pipe idx is bigger than 5bit width 0x%x\n",
			pktinit_params->destination_pipe_index);
		WARN_ON(1);
	}
	data->destination_pipe_index = pktinit_params->destination_pipe_index;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_nat_dma(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_nat_dma *data;
	struct ipahal_imm_cmd_nat_dma *nat_params =
		(struct ipahal_imm_cmd_nat_dma *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_nat_dma *)pyld->data;

	data->table_index = nat_params->table_index;
	data->base_addr = nat_params->base_addr;
	data->offset = nat_params->offset;
	data->data = nat_params->data;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_hdr_init_system(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_hdr_init_system *data;
	struct ipahal_imm_cmd_hdr_init_system *syshdr_params =
		(struct ipahal_imm_cmd_hdr_init_system *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_hdr_init_system *)pyld->data;

	data->hdr_table_addr = syshdr_params->hdr_table_addr;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_hdr_init_local(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_hdr_init_local *data;
	struct ipahal_imm_cmd_hdr_init_local *lclhdr_params =
		(struct ipahal_imm_cmd_hdr_init_local *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_hdr_init_local *)pyld->data;

	if (unlikely(lclhdr_params->size_hdr_table & ~0xFFF)) {
		IPAHAL_ERR("Hdr tble size is bigger than 12bit width 0x%x\n",
			lclhdr_params->size_hdr_table);
		WARN_ON(1);
	}
	data->hdr_table_addr = lclhdr_params->hdr_table_addr;
	data->size_hdr_table = lclhdr_params->size_hdr_table;
	data->hdr_addr = lclhdr_params->hdr_addr;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_ip_v6_routing_init(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_v6_routing_init *data;
	struct ipahal_imm_cmd_ip_v6_routing_init *rt6_params =
		(struct ipahal_imm_cmd_ip_v6_routing_init *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_ip_v6_routing_init *)pyld->data;

	data->hash_rules_addr = rt6_params->hash_rules_addr;
	data->hash_rules_size = rt6_params->hash_rules_size;
	data->hash_local_addr = rt6_params->hash_local_addr;
	data->nhash_rules_addr = rt6_params->nhash_rules_addr;
	data->nhash_rules_size = rt6_params->nhash_rules_size;
	data->nhash_local_addr = rt6_params->nhash_local_addr;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_ip_v4_routing_init(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_v4_routing_init *data;
	struct ipahal_imm_cmd_ip_v4_routing_init *rt4_params =
		(struct ipahal_imm_cmd_ip_v4_routing_init *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_ip_v4_routing_init *)pyld->data;

	data->hash_rules_addr = rt4_params->hash_rules_addr;
	data->hash_rules_size = rt4_params->hash_rules_size;
	data->hash_local_addr = rt4_params->hash_local_addr;
	data->nhash_rules_addr = rt4_params->nhash_rules_addr;
	data->nhash_rules_size = rt4_params->nhash_rules_size;
	data->nhash_local_addr = rt4_params->nhash_local_addr;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_ip_v4_nat_init(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_v4_nat_init *data;
	struct ipahal_imm_cmd_ip_v4_nat_init *nat4_params =
		(struct ipahal_imm_cmd_ip_v4_nat_init *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_ip_v4_nat_init *)pyld->data;

	data->ipv4_rules_addr = nat4_params->ipv4_rules_addr;
	data->ipv4_expansion_rules_addr =
		nat4_params->ipv4_expansion_rules_addr;
	data->index_table_addr = nat4_params->index_table_addr;
	data->index_table_expansion_addr =
		nat4_params->index_table_expansion_addr;
	data->table_index = nat4_params->table_index;
	data->ipv4_rules_addr_type =
		nat4_params->ipv4_rules_addr_shared ? 1 : 0;
	data->ipv4_expansion_rules_addr_type =
		nat4_params->ipv4_expansion_rules_addr_shared ? 1 : 0;
	data->index_table_addr_type =
		nat4_params->index_table_addr_shared ? 1 : 0;
	data->index_table_expansion_addr_type =
		nat4_params->index_table_expansion_addr_shared ? 1 : 0;
	data->size_base_tables = nat4_params->size_base_tables;
	data->size_expansion_tables = nat4_params->size_expansion_tables;
	data->public_ip_addr = nat4_params->public_ip_addr;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_ip_v6_filter_init(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_v6_filter_init *data;
	struct ipahal_imm_cmd_ip_v6_filter_init *flt6_params =
		(struct ipahal_imm_cmd_ip_v6_filter_init *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_ip_v6_filter_init *)pyld->data;

	data->hash_rules_addr = flt6_params->hash_rules_addr;
	data->hash_rules_size = flt6_params->hash_rules_size;
	data->hash_local_addr = flt6_params->hash_local_addr;
	data->nhash_rules_addr = flt6_params->nhash_rules_addr;
	data->nhash_rules_size = flt6_params->nhash_rules_size;
	data->nhash_local_addr = flt6_params->nhash_local_addr;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *ipa_imm_cmd_construct_ip_v4_filter_init(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_v4_filter_init *data;
	struct ipahal_imm_cmd_ip_v4_filter_init *flt4_params =
		(struct ipahal_imm_cmd_ip_v4_filter_init *)params;

	pyld = IPAHAL_MEM_ALLOC(sizeof(*pyld) + sizeof(*data), is_atomic_ctx);
	if (unlikely(!pyld)) {
		IPAHAL_ERR("kzalloc err\n");
		return pyld;
	}
	pyld->len = sizeof(*data);
	data = (struct ipa_imm_cmd_hw_ip_v4_filter_init *)pyld->data;

	data->hash_rules_addr = flt4_params->hash_rules_addr;
	data->hash_rules_size = flt4_params->hash_rules_size;
	data->hash_local_addr = flt4_params->hash_local_addr;
	data->nhash_rules_addr = flt4_params->nhash_rules_addr;
	data->nhash_rules_size = flt4_params->nhash_rules_size;
	data->nhash_local_addr = flt4_params->nhash_local_addr;

	return pyld;
}

/*
 * struct ipahal_imm_cmd_obj - immediate command H/W information for
 *  specific IPA version
 * @construct - CB to construct imm command payload from abstracted structure
 * @opcode - Immediate command OpCode
 * @dyn_op - Does this command supports Dynamic opcode?
 *  Some commands opcode are dynamic where the part of the opcode is
 *  supplied as param. This flag indicates if the specific command supports it
 *  or not.
 */
struct ipahal_imm_cmd_obj {
	struct ipahal_imm_cmd_pyld *(*construct)(enum ipahal_imm_cmd_name cmd,
		const void *params, bool is_atomic_ctx);
	u16 opcode;
	bool dyn_op;
};

/*
 * This table contains the info regard each immediate command for IPAv3
 *  and later.
 * Information like: opcode and construct functions.
 * All the information on the IMM on IPAv3 are statically defined below.
 * If information is missing regard some IMM on some IPA version,
 *  the init function will fill it with the information from the previous
 *  IPA version.
 * Information is considered missing if all of the fields are 0
 * If opcode is -1, this means that the IMM is removed on the
 *  specific version
 */
static struct ipahal_imm_cmd_obj
		ipahal_imm_cmd_objs[IPA_HW_MAX][IPA_IMM_CMD_MAX] = {
	/* IPAv3 */
	[IPA_HW_v3_0][IPA_IMM_CMD_IP_V4_FILTER_INIT] = {
		ipa_imm_cmd_construct_ip_v4_filter_init,
		3, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_IP_V6_FILTER_INIT] = {
		ipa_imm_cmd_construct_ip_v6_filter_init,
		4, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_IP_V4_NAT_INIT] = {
		ipa_imm_cmd_construct_ip_v4_nat_init,
		5, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_IP_V4_ROUTING_INIT] = {
		ipa_imm_cmd_construct_ip_v4_routing_init,
		7, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_IP_V6_ROUTING_INIT] = {
		ipa_imm_cmd_construct_ip_v6_routing_init,
		8, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_HDR_INIT_LOCAL] = {
		ipa_imm_cmd_construct_hdr_init_local,
		9, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_HDR_INIT_SYSTEM] = {
		ipa_imm_cmd_construct_hdr_init_system,
		10, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_REGISTER_WRITE] = {
		ipa_imm_cmd_construct_register_write,
		12, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_NAT_DMA] = {
		ipa_imm_cmd_construct_nat_dma,
		14, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_IP_PACKET_INIT] = {
		ipa_imm_cmd_construct_ip_packet_init,
		16, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_DMA_TASK_32B_ADDR] = {
		ipa_imm_cmd_construct_dma_task_32b_addr,
		17, true},
	[IPA_HW_v3_0][IPA_IMM_CMD_DMA_SHARED_MEM] = {
		ipa_imm_cmd_construct_dma_shared_mem,
		19, false},
	[IPA_HW_v3_0][IPA_IMM_CMD_IP_PACKET_TAG_STATUS] = {
		ipa_imm_cmd_construct_ip_packet_tag_status,
		20, false},
};

/*
 * ipahal_imm_cmd_init() - Build the Immediate command information table
 *  See ipahal_imm_cmd_objs[][] comments
 */
static int ipahal_imm_cmd_init(enum ipa_hw_type ipa_hw_type)
{
	int i;
	int j;
	struct ipahal_imm_cmd_obj zero_obj;

	IPAHAL_DBG("Entry - HW_TYPE=%d\n", ipa_hw_type);

	memset(&zero_obj, 0, sizeof(zero_obj));
	for (i = IPA_HW_v3_0 ; i < ipa_hw_type ; i++) {
		for (j = 0; j < IPA_IMM_CMD_MAX ; j++) {
			if (!memcmp(&ipahal_imm_cmd_objs[i+1][j], &zero_obj,
				sizeof(struct ipahal_imm_cmd_obj))) {
				memcpy(&ipahal_imm_cmd_objs[i+1][j],
					&ipahal_imm_cmd_objs[i][j],
					sizeof(struct ipahal_imm_cmd_obj));
			} else {
				/*
				 * explicitly overridden immediate command.
				 * Check validity
				 */
				 if (!ipahal_imm_cmd_objs[i+1][j].opcode) {
					IPAHAL_ERR(
					  "imm_cmd=%s with zero opcode\n",
					  ipahal_imm_cmd_name_str(j));
					WARN_ON(1);
				 }
				 if (!ipahal_imm_cmd_objs[i+1][j].construct) {
					IPAHAL_ERR(
					  "imm_cmd=%s with NULL construct fun\n",
					  ipahal_imm_cmd_name_str(j));
					WARN_ON(1);
				 }
			}
		}
	}

	return 0;
}

/*
 * ipahal_imm_cmd_name_str() - returns string that represent the imm cmd
 * @cmd_name: [in] Immediate command name
 */
const char *ipahal_imm_cmd_name_str(enum ipahal_imm_cmd_name cmd_name)
{
	if (cmd_name < 0 || cmd_name >= IPA_IMM_CMD_MAX) {
		IPAHAL_ERR("requested name of invalid imm_cmd=%d\n", cmd_name);
		return "Invalid IMM_CMD";
	}

	return ipahal_imm_cmd_name_to_str[cmd_name];
}

/*
 * ipahal_imm_cmd_get_opcode() - Get the fixed opcode of the immediate command
 */
u16 ipahal_imm_cmd_get_opcode(enum ipahal_imm_cmd_name cmd)
{
	u32 opcode;

	if (cmd >= IPA_IMM_CMD_MAX) {
		IPAHAL_ERR("Invalid immediate command imm_cmd=%u\n", cmd);
		BUG();
		return -EFAULT;
	}

	IPAHAL_DBG("Get opcode of IMM_CMD=%s\n", ipahal_imm_cmd_name_str(cmd));
	opcode = ipahal_imm_cmd_objs[ipahal_ctx->hw_type][cmd].opcode;
	if (opcode == -1) {
		IPAHAL_ERR("Try to get opcode of obsolete IMM_CMD=%s\n",
			ipahal_imm_cmd_name_str(cmd));
		BUG();
		return -EFAULT;
	}

	return opcode;
}

/*
 * ipahal_imm_cmd_get_opcode_param() - Get the opcode of an immediate command
 *  that supports dynamic opcode
 * Some commands opcode are not totaly fixed, but part of it is
 *  a supplied parameter. E.g. Low-Byte is fixed and Hi-Byte
 *  is a given parameter.
 * This API will return the composed opcode of the command given
 *  the parameter
 * Note: Use this API only for immediate comamnds that support Dynamic Opcode
 */
u16 ipahal_imm_cmd_get_opcode_param(enum ipahal_imm_cmd_name cmd, int param)
{
	u32 opcode;

	if (cmd >= IPA_IMM_CMD_MAX) {
		IPAHAL_ERR("Invalid immediate command IMM_CMD=%u\n", cmd);
		BUG();
		return -EFAULT;
	}

	IPAHAL_DBG("Get opcode of IMM_CMD=%s\n", ipahal_imm_cmd_name_str(cmd));

	if (!ipahal_imm_cmd_objs[ipahal_ctx->hw_type][cmd].dyn_op) {
		IPAHAL_ERR("IMM_CMD=%s does not support dynamic opcode\n",
			ipahal_imm_cmd_name_str(cmd));
		BUG();
		return -EFAULT;
	}

	/* Currently, dynamic opcode commands uses params to be set
	 *  on the Opcode hi-byte (lo-byte is fixed).
	 * If this to be changed in the future, make the opcode calculation
	 *  a CB per command
	 */
	if (param & ~0xFFFF) {
		IPAHAL_ERR("IMM_CMD=%s opcode param is invalid\n",
			ipahal_imm_cmd_name_str(cmd));
		BUG();
		return -EFAULT;
	}
	opcode = ipahal_imm_cmd_objs[ipahal_ctx->hw_type][cmd].opcode;
	if (opcode == -1) {
		IPAHAL_ERR("Try to get opcode of obsolete IMM_CMD=%s\n",
			ipahal_imm_cmd_name_str(cmd));
		BUG();
		return -EFAULT;
	}
	if (opcode & ~0xFFFF) {
		IPAHAL_ERR("IMM_CMD=%s opcode will be overridden\n",
			ipahal_imm_cmd_name_str(cmd));
		BUG();
		return -EFAULT;
	}
	return (opcode + (param<<8));
}

/*
 * ipahal_construct_imm_cmd() - Construct immdiate command
 * This function builds imm cmd bulk that can be be sent to IPA
 * The command will be allocated dynamically.
 * After done using it, call ipahal_destroy_imm_cmd() to release it
 */
struct ipahal_imm_cmd_pyld *ipahal_construct_imm_cmd(
	enum ipahal_imm_cmd_name cmd, const void *params, bool is_atomic_ctx)
{
	if (!params) {
		IPAHAL_ERR("Input error: params=%p\n", params);
		BUG();
		return NULL;
	}

	if (cmd >= IPA_IMM_CMD_MAX) {
		IPAHAL_ERR("Invalid immediate command %u\n", cmd);
		BUG();
		return NULL;
	}

	IPAHAL_DBG("construct IMM_CMD:%s\n", ipahal_imm_cmd_name_str(cmd));
	return ipahal_imm_cmd_objs[ipahal_ctx->hw_type][cmd].construct(
		cmd, params, is_atomic_ctx);
}

/*
 * ipahal_construct_nop_imm_cmd() - Construct immediate comamnd for NO-Op
 * Core driver may want functionality to inject NOP commands to IPA
 *  to ensure e.g., PIPLINE clear before someother operation.
 * The functionality given by this function can be reached by
 *  ipahal_construct_imm_cmd(). This function is helper to the core driver
 *  to reach this NOP functionlity easily.
 * @skip_pipline_clear: if to skip pipeline clear waiting (don't wait)
 * @pipline_clr_opt: options for pipeline clear waiting
 * @is_atomic_ctx: is called in atomic context or can sleep?
 */
struct ipahal_imm_cmd_pyld *ipahal_construct_nop_imm_cmd(
	bool skip_pipline_clear,
	enum ipahal_pipeline_clear_option pipline_clr_opt,
	bool is_atomic_ctx)
{
	struct ipahal_imm_cmd_register_write cmd;
	struct ipahal_imm_cmd_pyld *cmd_pyld;

	memset(&cmd, 0, sizeof(cmd));
	cmd.skip_pipeline_clear = skip_pipline_clear;
	cmd.pipeline_clear_options = pipline_clr_opt;
	cmd.value_mask = 0x0;

	cmd_pyld = ipahal_construct_imm_cmd(IPA_IMM_CMD_REGISTER_WRITE,
		&cmd, is_atomic_ctx);

	if (!cmd_pyld)
		IPAHAL_ERR("failed to construct register_write imm cmd\n");

	return cmd_pyld;
}

int ipahal_init(enum ipa_hw_type ipa_hw_type, void __iomem *base)
{
	int result;

	IPAHAL_DBG("Entry - IPA HW TYPE=%d base=%p\n",
		ipa_hw_type, base);

	ipahal_ctx = kzalloc(sizeof(*ipahal_ctx), GFP_KERNEL);
	if (!ipahal_ctx) {
		IPAHAL_ERR("kzalloc err for ipahal_ctx\n");
		result = -ENOMEM;
		goto bail_err_exit;
	}

	if (ipa_hw_type < IPA_HW_v3_0) {
		IPAHAL_ERR("ipahal supported on IPAv3 and later only\n");
		result = -EINVAL;
		goto bail_free_ctx;
	}

	if (!base) {
		IPAHAL_ERR("invalid memory io mapping addr\n");
		result = -EINVAL;
		goto bail_free_ctx;
	}

	ipahal_ctx->hw_type = ipa_hw_type;
	ipahal_ctx->base = base;

	if (ipahal_reg_init(ipa_hw_type)) {
		IPAHAL_ERR("failed to init ipahal reg\n");
		result = -EFAULT;
		goto bail_free_ctx;
	}

	if (ipahal_imm_cmd_init(ipa_hw_type)) {
		IPAHAL_ERR("failed to init ipahal imm cmd\n");
		result = -EFAULT;
		goto bail_free_ctx;
	}

	return 0;

bail_free_ctx:
	kfree(ipahal_ctx);
	ipahal_ctx = NULL;
bail_err_exit:
	return result;
}

void ipahal_destroy(void)
{
	IPAHAL_DBG("Entry\n");

	kfree(ipahal_ctx);
	ipahal_ctx = NULL;
}
