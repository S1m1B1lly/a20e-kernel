/*
 * Copyright (c) 2011-2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _NVME_H
#define _NVME_H

#include <linux/mutex.h>
#include <linux/nvme.h>
#include <linux/pci.h>
#include <linux/kref.h>
#include <linux/blk-mq.h>

extern unsigned char nvme_io_timeout;
#define NVME_IO_TIMEOUT	(nvme_io_timeout * HZ)

enum {
	NVME_NS_LBA		= 0,
	NVME_NS_LIGHTNVM	= 1,
};

/* The below value is the specific amount of delay needed before checking
 * readiness in case of the PCI_DEVICE(0x1c58, 0x0003), which needs the
 * NVME_QUIRK_DELAY_BEFORE_CHK_RDY quirk enabled. The value (in ms) was
 * found empirically.
 */
#define NVME_QUIRK_DELAY_AMOUNT		2000

/*
 * Represents an NVM Express device.  Each nvme_dev is a PCI function.
 */
struct nvme_dev {
	struct list_head node;
	struct nvme_queue **queues;
	struct request_queue *admin_q;
	struct blk_mq_tag_set tagset;
	struct blk_mq_tag_set admin_tagset;
	u32 __iomem *dbs;
	struct device *dev;
	struct dma_pool *prp_page_pool;
	struct dma_pool *prp_small_pool;
	int instance;
	unsigned queue_count;
	unsigned online_queues;
	unsigned max_qid;
	int q_depth;
	u32 db_stride;
	u32 ctrl_config;
	struct msix_entry *entry;
	struct nvme_bar __iomem *bar;
	struct list_head namespaces;
	struct kref kref;
	struct device *device;
	struct work_struct reset_work;
	struct work_struct probe_work;
	struct work_struct scan_work;
	struct mutex shutdown_lock;
	char name[12];
	char serial[20];
	char model[40];
	char firmware_rev[8];
	bool subsystem;
	u32 max_hw_sectors;
	u32 stripe_size;
	u32 page_size;
	void __iomem *cmb;
	dma_addr_t cmb_dma_addr;
	u64 cmb_size;
	u32 cmbsz;
	u16 oncs;
	u16 abort_limit;
	u8 event_limit;
	u8 vwc;
};

/*
 * An NVM Express namespace is equivalent to a SCSI LUN
 */
struct nvme_ns {
	struct list_head list;

	struct nvme_dev *dev;
	struct request_queue *queue;
	struct gendisk *disk;
	struct kref kref;

	unsigned ns_id;
	int lba_shift;
	u16 ms;
	bool ext;
	u8 pi_type;
	int type;
	u64 mode_select_num_blocks;
	u32 mode_select_block_len;
};

static inline u64 nvme_block_nr(struct nvme_ns *ns, sector_t sector)
{
	return (sector >> (ns->lba_shift - 9));
}

static inline void nvme_setup_flush(struct nvme_ns *ns,
		struct nvme_command *cmnd)
{
	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->common.opcode = nvme_cmd_flush;
	cmnd->common.nsid = cpu_to_le32(ns->ns_id);
}

static inline void nvme_setup_rw(struct nvme_ns *ns, struct request *req,
		struct nvme_command *cmnd)
{
	u16 control = 0;
	u32 dsmgmt = 0;

	if (req->cmd_flags & REQ_FUA)
		control |= NVME_RW_FUA;
	if (req->cmd_flags & (REQ_FAILFAST_DEV | REQ_RAHEAD))
		control |= NVME_RW_LR;

	if (req->cmd_flags & REQ_RAHEAD)
		dsmgmt |= NVME_RW_DSM_FREQ_PREFETCH;

	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->rw.opcode = (rq_data_dir(req) ? nvme_cmd_write : nvme_cmd_read);
	cmnd->rw.command_id = req->tag;
	cmnd->rw.nsid = cpu_to_le32(ns->ns_id);
	cmnd->rw.slba = cpu_to_le64(nvme_block_nr(ns, blk_rq_pos(req)));
	cmnd->rw.length = cpu_to_le16((blk_rq_bytes(req) >> ns->lba_shift) - 1);

	if (ns->ms) {
		switch (ns->pi_type) {
		case NVME_NS_DPS_PI_TYPE3:
			control |= NVME_RW_PRINFO_PRCHK_GUARD;
			break;
		case NVME_NS_DPS_PI_TYPE1:
		case NVME_NS_DPS_PI_TYPE2:
			control |= NVME_RW_PRINFO_PRCHK_GUARD |
					NVME_RW_PRINFO_PRCHK_REF;
			cmnd->rw.reftag = cpu_to_le32(
					nvme_block_nr(ns, blk_rq_pos(req)));
			break;
		}
		if (!blk_integrity_rq(req))
			control |= NVME_RW_PRINFO_PRACT;
	}

	cmnd->rw.control = cpu_to_le16(control);
	cmnd->rw.dsmgmt = cpu_to_le32(dsmgmt);
}


static inline int nvme_error_status(u16 status)
{
	switch (status & 0x7ff) {
	case NVME_SC_SUCCESS:
		return 0;
	case NVME_SC_CAP_EXCEEDED:
		return -ENOSPC;
	default:
		return -EIO;
	}
}

int nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		void *buf, unsigned bufflen);
int __nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		void *buffer, void __user *ubuffer, unsigned bufflen,
		u32 *result, unsigned timeout);
int nvme_identify_ctrl(struct nvme_dev *dev, struct nvme_id_ctrl **id);
int nvme_identify_ns(struct nvme_dev *dev, unsigned nsid,
		struct nvme_id_ns **id);
int nvme_get_log_page(struct nvme_dev *dev, struct nvme_smart_log **log);
int nvme_get_features(struct nvme_dev *dev, unsigned fid, unsigned nsid,
			dma_addr_t dma_addr, u32 *result);
int nvme_set_features(struct nvme_dev *dev, unsigned fid, unsigned dword11,
			dma_addr_t dma_addr, u32 *result);

struct sg_io_hdr;

int nvme_sg_io(struct nvme_ns *ns, struct sg_io_hdr __user *u_hdr);
int nvme_sg_io32(struct nvme_ns *ns, unsigned long arg);
int nvme_sg_get_version_num(int __user *ip);

#ifdef CONFIG_NVM
int nvme_nvm_ns_supported(struct nvme_ns *ns, struct nvme_id_ns *id);
int nvme_nvm_register(struct request_queue *q, char *disk_name);
void nvme_nvm_unregister(struct request_queue *q, char *disk_name);
#else
static inline int nvme_nvm_register(struct request_queue *q, char *disk_name)
{
	return 0;
}

static inline void nvme_nvm_unregister(struct request_queue *q, char *disk_name) {};

static inline int nvme_nvm_ns_supported(struct nvme_ns *ns, struct nvme_id_ns *id)
{
	return 0;
}
#endif /* CONFIG_NVM */

#endif /* _NVME_H */
