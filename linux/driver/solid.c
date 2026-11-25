#include <asm/byteorder.h>
#include <linux/blk-mq.h>
#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "solid_ipc.h"

#define SECTORS_PER_BLOCK (SOLID_IPC_BLOCK_BYTES / 512U)

static unsigned int capacity_blocks;
module_param(capacity_blocks, uint, 0444);
MODULE_PARM_DESC(capacity_blocks, "Usable 2 KiB logical blocks reported by the STM32 FTL");
static bool allow_writes;
module_param(allow_writes, bool, 0444);
MODULE_PARM_DESC(allow_writes, "Enable only when successful STM32 WRITE replies are durable");

struct bridge_request {
	__le32 id, op, lba, length;
};

struct bridge_message {
	struct bridge_request header;
	u8 data[SOLID_IPC_MAX_BYTES];
};

struct bridge_reply {
	__le32 id, status;
	u8 data[SOLID_IPC_MAX_BYTES];
};

static_assert(sizeof(struct bridge_request) == SOLID_IPC_REQUEST_HEADER);
static_assert(offsetof(struct bridge_message, data) == SOLID_IPC_REQUEST_HEADER);
static_assert(offsetof(struct bridge_reply, data) == SOLID_IPC_REPLY_HEADER);

static struct gendisk *disk;
static DEFINE_MUTEX(dispatch_lock);
static DEFINE_MUTEX(bridge_lock);
static DECLARE_WAIT_QUEUE_HEAD(request_wait);
static DECLARE_COMPLETION(reply_done);
static struct bridge_message message;
static struct bridge_reply reply;
static bool bridge_open, active, pending, delivered, replied;
static u32 next_id;

static int control_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	mutex_lock(&bridge_lock);
	if (bridge_open)
		ret = -EBUSY;
	else
		bridge_open = true;
	mutex_unlock(&bridge_lock);
	return ret;
}

static int control_release(struct inode *inode, struct file *file)
{
	mutex_lock(&bridge_lock);
	bridge_open = false;
	if (active) {
		WRITE_ONCE(pending, false);
		complete(&reply_done);
	}
	mutex_unlock(&bridge_lock);
	return 0;
}

static ssize_t control_read(struct file *file, char __user *buf,
			    size_t len, loff_t *pos)
{
	ssize_t ret;
	size_t message_size;

	if (len < sizeof(message.header))
		return -EMSGSIZE;

	for (;;) {
		if (file->f_flags & O_NONBLOCK) {
			if (!READ_ONCE(pending))
				return -EAGAIN;
		} else {
			ret = wait_event_interruptible(request_wait, READ_ONCE(pending));
			if (ret)
				return ret;
		}

		mutex_lock(&bridge_lock);
		if (pending)
			break;
		mutex_unlock(&bridge_lock);
	}

	message_size = sizeof(message.header);
	if (le32_to_cpu(message.header.op) == SOLID_IPC_OP_WRITE)
		message_size += le32_to_cpu(message.header.length);
	if (len < message_size) {
		ret = -EMSGSIZE;
	} else if (copy_to_user(buf, &message, message_size)) {
		ret = -EFAULT;
	} else {
		WRITE_ONCE(pending, false);
		delivered = true;
		ret = message_size;
	}
	mutex_unlock(&bridge_lock);
	return ret;
}

static ssize_t control_write(struct file *file, const char __user *buf,
			     size_t len, loff_t *pos)
{
	__le32 header[2];
	size_t expected = SOLID_IPC_REPLY_HEADER;
	ssize_t ret;

	if (len < sizeof(header) || len > sizeof(reply))
		return -EMSGSIZE;
	if (copy_from_user(header, buf, sizeof(header)))
		return -EFAULT;

	mutex_lock(&bridge_lock);
	if (!active || !delivered || replied || header[0] != message.header.id) {
		ret = -EPROTO;
		goto done;
	}
	if (!le32_to_cpu(header[1]) &&
	    le32_to_cpu(message.header.op) == SOLID_IPC_OP_READ)
		expected += le32_to_cpu(message.header.length);
	if (len != expected) {
		ret = -EMSGSIZE;
		goto done;
	}
	if (len > sizeof(header) &&
	    copy_from_user(reply.data, buf + sizeof(header), len - sizeof(header))) {
		ret = -EFAULT;
		goto done;
	}
	reply.id = header[0];
	reply.status = header[1];
	replied = true;
	complete(&reply_done);
	ret = len;
done:
	mutex_unlock(&bridge_lock);
	return ret;
}

static const struct file_operations control_ops = {
	.owner = THIS_MODULE,
	.open = control_open,
	.release = control_release,
	.read = control_read,
	.write = control_write,
	.llseek = no_llseek,
};

static struct miscdevice control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "solidctl",
	.fops = &control_ops,
	.mode = 0600,
};

static void copy_request_data(struct request *rq, u8 *data, bool to_request)
{
	struct req_iterator iter;
	struct bio_vec vec;

	rq_for_each_segment(vec, rq, iter) {
		void *page = kmap_local_page(vec.bv_page);
		u8 *buffer = (u8 *)page + vec.bv_offset;

		if (to_request)
			memcpy(buffer, data, vec.bv_len);
		else
			memcpy(data, buffer, vec.bv_len);
		kunmap_local(page);
		data += vec.bv_len;
	}
}

static blk_status_t queue_io(struct blk_mq_hw_ctx *hctx,
			     const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	unsigned int op = req_op(rq);
	blk_status_t result = BLK_STS_IOERR;
	unsigned long remaining;

	sector_t sector = blk_rq_pos(rq);
	unsigned int bytes = blk_rq_bytes(rq);
	sector_t lba = sector / SECTORS_PER_BLOCK;

	if (op != REQ_OP_READ && op != REQ_OP_WRITE)
		return BLK_STS_NOTSUPP;
	if (op == REQ_OP_WRITE && !allow_writes)
		return BLK_STS_IOERR;
	if (sector % SECTORS_PER_BLOCK || !bytes || bytes > SOLID_IPC_MAX_BYTES ||
	    bytes % SOLID_IPC_BLOCK_BYTES || lba >= capacity_blocks ||
	    bytes / SOLID_IPC_BLOCK_BYTES > capacity_blocks - lba)
		return BLK_STS_IOERR;

	mutex_lock(&dispatch_lock);
	mutex_lock(&bridge_lock);
	if (!bridge_open) {
		mutex_unlock(&bridge_lock);
		mutex_unlock(&dispatch_lock);
		return BLK_STS_IOERR;
	}

	blk_mq_start_request(rq);
	reinit_completion(&reply_done);
	message.header.id = cpu_to_le32(++next_id);
	message.header.op = cpu_to_le32(op == REQ_OP_WRITE ? SOLID_IPC_OP_WRITE : SOLID_IPC_OP_READ);
	message.header.lba = cpu_to_le32((u32)lba);
	message.header.length = cpu_to_le32(bytes);
	if (op == REQ_OP_WRITE)
		copy_request_data(rq, message.data, false);
	active = true;
	delivered = false;
	replied = false;
	WRITE_ONCE(pending, true);
	mutex_unlock(&bridge_lock);
	wake_up_interruptible(&request_wait);

	/* A blocking queue allows the bridge to complete the request in process context. */
	remaining = wait_for_completion_timeout(&reply_done,
			msecs_to_jiffies(SOLID_IPC_TIMEOUT_MS));
	mutex_lock(&bridge_lock);
	if (remaining && replied && !le32_to_cpu(reply.status)) {
		if (op == REQ_OP_READ)
			copy_request_data(rq, reply.data, true);
		result = BLK_STS_OK;
	} else if (!remaining) {
		result = BLK_STS_TIMEOUT;
	}
	active = false;
	WRITE_ONCE(pending, false);
	mutex_unlock(&bridge_lock);
	blk_mq_end_request(rq, result);
	mutex_unlock(&dispatch_lock);
	return BLK_STS_OK;
}

static const struct blk_mq_ops queue_ops = { .queue_rq = queue_io };
static const struct block_device_operations disk_ops = { .owner = THIS_MODULE };
static struct blk_mq_tag_set tags = {
	.ops = &queue_ops,
	.nr_hw_queues = 1,
	.queue_depth = 8,
	.numa_node = NUMA_NO_NODE,
	.flags = BLK_MQ_F_BLOCKING,
};

static int __init solid_init(void)
{
	int ret;

	if (!capacity_blocks)
		return -EINVAL;
	ret = blk_mq_alloc_tag_set(&tags);

	if (ret)
		return ret;
	disk = blk_mq_alloc_disk(&tags, NULL);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto free_tags;
	}
	disk->fops = &disk_ops;
#ifdef GENHD_FL_NO_PART
	disk->flags |= GENHD_FL_NO_PART;
#else
	disk->flags |= GENHD_FL_NO_PART_SCAN;
#endif
	strscpy(disk->disk_name, "solid0", sizeof(disk->disk_name));
	blk_queue_logical_block_size(disk->queue, SOLID_IPC_BLOCK_BYTES);
	blk_queue_physical_block_size(disk->queue, SOLID_IPC_BLOCK_BYTES);
	blk_queue_max_hw_sectors(disk->queue, SOLID_IPC_MAX_BYTES / 512U);
	set_capacity(disk, (sector_t)capacity_blocks * SECTORS_PER_BLOCK);
	set_disk_ro(disk, !allow_writes);

	ret = add_disk(disk);
	if (ret)
		goto free_disk;
	ret = misc_register(&control_device);
	if (!ret)
		return 0;
	del_gendisk(disk);
free_disk:
	blk_cleanup_disk(disk);
free_tags:
	blk_mq_free_tag_set(&tags);
	return ret;
}

static void __exit solid_exit(void)
{
	misc_deregister(&control_device);
	del_gendisk(disk);
	blk_cleanup_disk(disk);
	blk_mq_free_tag_set(&tags);
}

module_init(solid_init);
module_exit(solid_exit);
MODULE_LICENSE("GPL");
