#include <asm/byteorder.h>
#include <linux/build_bug.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define SOLID_BLOCK_BYTES 2048U
#define SOLID_TEST_ID 42U
#define SOLID_OP_READ 2U

struct solidctl_request {
	__le32 id;
	__le32 op;
	__le32 lba;
	__le32 length;
};

struct solidctl_reply {
	__le32 id;
	__le32 status;
	u8 data[SOLID_BLOCK_BYTES];
};

static_assert(sizeof(struct solidctl_request) == 16);
static_assert(sizeof(struct solidctl_reply) == 2056);

struct solidctl_session {
	struct mutex lock;
	bool delivered;
	bool completed;
	struct solidctl_reply reply;
};

static int solidctl_open(struct inode *inode, struct file *file)
{
	struct solidctl_session *session = kzalloc(sizeof(*session), GFP_KERNEL);

	if (!session)
		return -ENOMEM;
	mutex_init(&session->lock);
	file->private_data = session;
	return 0;
}

static int solidctl_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t solidctl_read(struct file *file, char __user *buf,
			     size_t count, loff_t *offset)
{
	struct solidctl_session *session = file->private_data;
	const struct solidctl_request request = {
		.id = cpu_to_le32(SOLID_TEST_ID),
		.op = cpu_to_le32(SOLID_OP_READ),
		.lba = cpu_to_le32(2),
		.length = cpu_to_le32(SOLID_BLOCK_BYTES),
	};
	ssize_t result;

	if (count < sizeof(request))
		return -EMSGSIZE;
	mutex_lock(&session->lock);
	if (session->delivered) {
		result = 0;
	} else if (copy_to_user(buf, &request, sizeof(request))) {
		result = -EFAULT;
	} else {
		session->delivered = true;
		result = sizeof(request);
	}
	mutex_unlock(&session->lock);
	return result;
}

static ssize_t solidctl_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *offset)
{
	struct solidctl_session *session = file->private_data;
	struct solidctl_reply *reply = &session->reply;
	ssize_t result;
	unsigned int i;

	if (count != sizeof(*reply))
		return -EMSGSIZE;
	mutex_lock(&session->lock);
	if (!session->delivered || session->completed) {
		result = -EPROTO;
		goto done;
	}
	if (copy_from_user(reply, buf, sizeof(*reply))) {
		result = -EFAULT;
		goto done;
	}
	if (le32_to_cpu(reply->id) != SOLID_TEST_ID || le32_to_cpu(reply->status) != 0) {
		result = -EPROTO;
		goto done;
	}
	for (i = 0; i < SOLID_BLOCK_BYTES; ++i) {
		if (reply->data[i] != (u8)(i ^ (i >> 8))) {
			result = -EBADMSG;
			goto done;
		}
	}
	session->completed = true;
	pr_info("solidctl_lab: request %u, %u bytes matched\n",
		SOLID_TEST_ID, SOLID_BLOCK_BYTES);
	result = count;
done:
	mutex_unlock(&session->lock);
	return result;
}

static const struct file_operations solidctl_ops = {
	.owner = THIS_MODULE,
	.open = solidctl_open,
	.release = solidctl_release,
	.read = solidctl_read,
	.write = solidctl_write,
	.llseek = no_llseek,
};

static struct miscdevice solidctl = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "solidctl_lab",
	.fops = &solidctl_ops,
	.mode = 0600,
};

module_misc_device(solidctl);
MODULE_LICENSE("GPL");
