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

#define BLOCK_BYTES 2048U
#define SECTORS_PER_BLOCK (BLOCK_BYTES / 512U)
#define TEST_LBA 2U
#define WRITE_OPCODE 1U
#define READ_OPCODE 2U

struct bridge_request {
	__le32 id, op, lba, length;
};

struct bridge_message {
	struct bridge_request header;
	u8 data[BLOCK_BYTES];
};

struct bridge_reply {
	__le32 id, status;
	u8 data[BLOCK_BYTES];
};

static_assert(sizeof(struct bridge_request) == 16);
static_assert(sizeof(struct bridge_message) == 2064);
static_assert(sizeof(struct bridge_reply) == 2056);

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

	message_size = le32_to_cpu(message.header.op) == WRITE_OPCODE ?
		sizeof(message) : sizeof(message.header);
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
	ssize_t ret;

	if (len != sizeof(reply))
		return -EMSGSIZE;
	mutex_lock(&bridge_lock);
	if (!active || !delivered || replied) {
		ret = -EPROTO;
	} else if (copy_from_user(&reply, buf, sizeof(reply))) {
		ret = -EFAULT;
	} else if (reply.id != message.header.id) {
		ret = -EPROTO;
	} else {
		replied = true;
		complete(&reply_done);
		ret = len;
	}
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
	.name = "solidblkctl_lab",
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

	/* Exercise one real request without letting device probes use the bridge. */
	if ((op != REQ_OP_READ && op != REQ_OP_WRITE) ||
	    blk_rq_pos(rq) != TEST_LBA * SECTORS_PER_BLOCK ||
	    blk_rq_bytes(rq) != BLOCK_BYTES)
		return BLK_STS_NOTSUPP;

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
	message.header.op = cpu_to_le32(op == REQ_OP_WRITE ? WRITE_OPCODE : READ_OPCODE);
	message.header.lba = cpu_to_le32(blk_rq_pos(rq) / SECTORS_PER_BLOCK);
	message.header.length = cpu_to_le32(blk_rq_bytes(rq));
	if (op == REQ_OP_WRITE)
		copy_request_data(rq, message.data, false);
	active = true;
	delivered = false;
	replied = false;
	WRITE_ONCE(pending, true);
	mutex_unlock(&bridge_lock);
	wake_up_interruptible(&request_wait);

	/* This lab uses a blocking blk-mq queue with one request in flight. */
	remaining = wait_for_completion_timeout(&reply_done, 3 * HZ);
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

static int __init solidblk_init(void)
{
	int ret = blk_mq_alloc_tag_set(&tags);

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
	strscpy(disk->disk_name, "solidblk0", sizeof(disk->disk_name));
	blk_queue_logical_block_size(disk->queue, BLOCK_BYTES);
	blk_queue_physical_block_size(disk->queue, BLOCK_BYTES);
	set_capacity(disk, 8192);

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

static void __exit solidblk_exit(void)
{
	misc_deregister(&control_device);
	del_gendisk(disk);
	blk_cleanup_disk(disk);
	blk_mq_free_tag_set(&tags);
}

module_init(solidblk_init);
module_exit(solidblk_exit);
MODULE_LICENSE("GPL");
