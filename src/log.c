#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/in.h>
#include <linux/ipv6.h>
#include <linux/version.h>
#include "netlog.h"
#include "log.h"
#include "retro-compat.h"

/* Log structure of records stored the buffer */
struct netlog_log {
	size_t len;
	size_t path_len;
	u64 nsec;
	pid_t pid;
	uid_t uid;
	u8 action;
	u8 protocol;
	unsigned short family;
	int src_port;
	int dst_port;
	union {
		struct in_addr ip4;
		struct in6_addr ip6;
		u8 raw[16];
	} dst;
	union {
		struct in_addr ip4;
		struct in6_addr ip6;
		u8 raw[16];
	} src;
};
#define LOG_ALIGN __alignof__(struct netlog_log)

/* Buffer */
static char log_buf[LOG_BUF_LEN];

/* index and sequence number of the first record stored in the buffer */
static u64 log_first_seq;
static u32 log_first_idx;

/* index and sequence number of the next record to store in the buffer */
static u64 log_next_seq;
static u32 log_next_idx;

/* Buffer protection */
static DEFINE_SPINLOCK(log_lock);

/* Poll queue */
static DECLARE_WAIT_QUEUE_HEAD(log_wait);

static char first_read = 1;

/* Get the path of a log */
static char *log_path(struct netlog_log *log) __must_hold(log_lock)
{
	return ((char*)log) + sizeof(struct netlog_log);
}

static u32 next_record(u32 idx) __must_hold(log_lock)
{
	size_t *len;

	len = (size_t*)(log_buf + idx);
	if (*len == 0) {
		/* We need to wrap around */
		return 0;
	}
	return idx + *len;
}

/* Small tool */
static void copy_ip(void *dst, const void *src, unsigned short family)
{
	switch(family)
	{
		case AF_INET:
			memcpy(dst, src, sizeof(struct in_addr));
			break;
		case AF_INET6:
			memcpy(dst, src, sizeof(struct in6_addr));
			break;
		default:
			break;
	}
}

void
store_record(pid_t pid, uid_t uid, const char* path, u8 action,
             u8 protocol, unsigned short family,
             const void *src_ip, int src_port, const void *dst_ip, int dst_port) {
	struct netlog_log *record;
	size_t path_len, record_size;
	unsigned long flags;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	struct timespec ts;
#endif
	path_len = strlen(path);
	if (unlikely(path_len > (LOG_BUF_LEN >> 4) ||
	             path_len > INT_MAX)) {
		printk(KERN_INFO MODULE_NAME ": Warning, troncating path\n");
		path_len = min((LOG_BUF_LEN >> 4), INT_MAX);
	}
	record_size = sizeof(struct netlog_log) + path_len + 1;
	record_size += (-record_size) & (LOG_ALIGN - 1);

	spin_lock_irqsave(&log_lock, flags);

	while (log_first_seq < log_next_seq) {
		size_t free;

		if (log_next_idx > log_first_idx)
			free = max(LOG_BUF_LEN - log_next_idx, log_first_idx);
		else
			free = log_first_idx - log_next_idx;

		if (free > record_size + sizeof(size_t))
			break;

		/* Drop old messages until we have enough contiuous space */
		log_first_idx = next_record(log_first_idx);
		log_first_seq++;
	}

	if (unlikely(log_next_idx + record_size + sizeof(size_t) >= LOG_BUF_LEN)) {
		/*
		 * As free > size + sizeof(size_t), this mean that we had
		 * free = max(log_buf_len - log_next_idx, log_first_idx)
		 * But as we are too close to the end, it means that the max
		 * is log_first_idx, thus we must wrap around.
		 * Add an empty size_t to indicate the wrap around
		 */
		*((size_t*)(log_buf + log_next_idx)) = 0;
		log_next_idx = 0;
	}

	record = (struct netlog_log*)(log_buf + log_next_idx);
	/* Store the data in the recored */
	record->len = record_size;
	record->path_len = path_len;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	getnstimeofday(&ts);
	record->nsec = (u64) timespec_to_ns(&ts);
#else
	record->nsec = local_clock();
#endif
	record->pid = pid;
	record->uid = uid;
	record->action = action;
	record->protocol = protocol;
	record->family = family;
	if (src_ip == NULL)
		memset(record->src.raw, 0, 16);
	else
		copy_ip(record->src.raw, src_ip, family);
	if (dst_ip == NULL)
		memset(record->dst.raw, 0, 16);
	else
		copy_ip(record->dst.raw, dst_ip, family);
	record->src_port = src_port;
	record->dst_port = dst_port;
	memcpy(log_path(record), path, path_len);

	/* Update the next position */
	log_next_idx += record_size;
	log_next_seq++;

	spin_unlock_irqrestore(&log_lock, flags);

	/* Wake-up reading threads */
	wake_up_interruptible(&log_wait);
}

struct user_data {
	u64 log_curr_seq;
	u32 log_curr_idx;
	struct mutex lock;
	char buf[8192];
};

static loff_t netlog_log_llseek(struct file *file, loff_t offset, int whence)
{
	struct user_data *data = file->private_data;
	unsigned long flags;

	if (unlikely(data == NULL))
		return -EBADF;

	/* We do not support custom offset */
	if (unlikely(offset != 0))
		return -ESPIPE;

	/* Set the 'offset' to the desired value */
	spin_lock_irqsave(&log_lock, flags);
	switch (whence) {
		case SEEK_SET:
			data->log_curr_seq = log_first_seq;
			data->log_curr_idx = log_first_idx;
			break;
		case SEEK_CUR:
			break;
		case SEEK_END:
			data->log_curr_seq = log_next_seq;
			data->log_curr_idx = log_next_idx;
			break;
		default:
			spin_unlock_irqrestore(&log_lock, flags);
			return -EINVAL;
	}
	spin_unlock_irqrestore(&log_lock, flags);

	return 0;
}

static inline const char* log_protocol(struct netlog_log *log) __must_hold(log_lock)
{
	switch(log->protocol) {
		case PROTO_TCP:
			return "TCP";
		case PROTO_UDP:
			return "UDP";
		default:
			return "UNK";
	}
}

static ssize_t netlog_log_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	struct user_data *data = file->private_data;
	struct netlog_log *record;
	u64 ts;
	unsigned long rem_nsec;
	unsigned long flags;
	size_t len;
	ssize_t err, ret;

	if (unlikely(data == NULL))
		return -EBADF;

	/* Is the user already reading ? */
	err = mutex_lock_interruptible(&data->lock);
	if (err)
		return err;

	spin_lock_irqsave(&log_lock, flags);
	/* Wait until we have something to read */
	while (data->log_curr_seq == log_next_seq) {
		/* Too bad, this call cannot be non-blocking */
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			spin_unlock_irqrestore(&log_lock, flags);
			goto out;
		}

		/* We need to wait, unlock */
		spin_unlock_irqrestore(&log_lock, flags);
		ret = wait_event_interruptible(log_wait, data->log_curr_seq != log_next_seq);
		if (ret)
			goto out;
		spin_lock_irqsave(&log_lock, flags);
	}

	/* Perhaps we waited for too long and some data is lost */
	if (unlikely(data->log_curr_seq < log_first_seq)) {
		/* Rest the position and alert the user */
		data->log_curr_seq = log_first_seq;
		data->log_curr_idx = log_first_idx;
		spin_unlock_irqrestore(&log_lock, flags);
		ret = -EPIPE;
		goto out;
	}

	/* Get the current record */
	record = (struct netlog_log*)(log_buf + data->log_curr_idx);

	/* Now we are good to go (locked, with something to print */
	/* Fill the header */
	ts = record->nsec;
	rem_nsec = do_div(ts, 1000000000);
	len = sprintf(data->buf, "<%u>1 - - %s - - - [%5lu.%06lu]: ",
	              (LOG_FACILITY << 3) | LOG_LEVEL, MODULE_NAME,
                      (unsigned long)ts, rem_nsec / 1000);

	len += sprintf(data->buf + len, "%.*s[%d] %s ",
	               (int)record->path_len, log_path(record),
	               record->pid, log_protocol(record));
	switch(record->family) {
		case AF_INET:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
			len += print_ipv4(data->buf + len, &record->src.ip4);
			len += sprintf(data->buf + len, ":%d", record->src_port);
#else
			len += sprintf(data->buf + len, "%pI4:%d",
			               &record->src.ip4, record->src_port);
#endif
			break;
		case AF_INET6:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
			len += print_ipv6(data->buf + len, &record->src.ip6);
			len += sprintf(data->buf + len, ":%d", record->src_port);
#else
			len += sprintf(data->buf + len, "[%pI6c]:%d",
			               &record->src.ip6, record->src_port);
#endif
			break;
		default:
			len += sprintf(data->buf + len, "Unknown");
			break;
	}
	switch(record->action) {
		case ACTION_CONNECT:
			len += sprintf(data->buf + len, " -> ");
			break;
		case ACTION_ACCEPT:
			len += sprintf(data->buf + len, " <- ");
			break;
		case ACTION_CLOSE:
			len += sprintf(data->buf + len, " <!> ");
			break;
		case ACTION_BIND:
			len += sprintf(data->buf + len, " BIND ");
			goto uid;
		default:
			len += sprintf(data->buf + len, " UNK ");
			goto uid;
	}
	switch(record->family) {
		case AF_INET:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
			len += print_ipv4(data->buf + len, &record->dst.ip4);
			len += sprintf(data->buf + len, ":%d", record->dst_port);
#else
			len += sprintf(data->buf + len, "%pI4:%d",
			               &record->dst.ip4, record->dst_port);
#endif
			break;
		case AF_INET6:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
			len += print_ipv6(data->buf + len, &record->dst.ip6);
			len += sprintf(data->buf + len, ":%d", record->dst_port);
#else
			len += sprintf(data->buf + len, "[%pI6c]:%d",
			               &record->dst.ip6, record->dst_port);
#endif
			break;
		default:
			len += sprintf(data->buf + len, "Unknown");
			break;
	}
uid:
	len += sprintf(data->buf + len, " (uid=%d)\n", record->uid);

	/* Prepare for next iteration */
	data->log_curr_idx = next_record(data->log_curr_idx);
	++data->log_curr_seq;

	/* Unlock */
	spin_unlock_irqrestore(&log_lock, flags);

	/* The user buffer is too small, abort */
	if (unlikely(len > count)) {
		ret = -EINVAL;
		goto out;
	}

	/* Copy the data into userspace */
	if (unlikely(copy_to_user(buf, data->buf, len))) {
		/* Copy failed */
		ret = -EFAULT;
		goto out;
	}
	ret = len;
out:
	mutex_unlock(&data->lock);
	return ret;
}

static unsigned int netlog_log_poll(struct file *file, poll_table *wait)
{
	struct user_data *data = file->private_data;
	unsigned long flags;
	int ret = 0;

	if (unlikely(data == NULL))
		return POLLERR|POLLNVAL;

	/* Update the poll state */
	poll_wait(file, &log_wait, wait);

	/* Check if there is anything to read */
	spin_lock_irqsave(&log_lock, flags);
	if (data->log_curr_seq < log_next_seq) {
		/* Return error when data has vanished underneath us */
		if (data->log_curr_seq < log_first_seq)
			ret = POLLIN|POLLRDNORM|POLLERR|POLLPRI;
		else
			ret = POLLIN|POLLRDNORM;
	}
	spin_unlock_irqrestore(&log_lock, flags);

	return ret;
}

static int netlog_log_open(struct inode *inode, struct file *file)
{
	struct user_data *data;
	unsigned long flags;

	/* Allocate private data */
	data = kmalloc(sizeof(struct user_data), GFP_KERNEL);
	if (unlikely(data == NULL))
		return -ENOMEM;

	/* Initialize read mutex */
	mutex_init(&data->lock);

	/* Get current state */
	spin_lock_irqsave(&log_lock, flags);
	if (first_read) {
		data->log_curr_seq = log_first_seq;
		data->log_curr_idx = log_first_idx;
		first_read = 0;
	} else {
		data->log_curr_seq = log_next_seq;
		data->log_curr_idx = log_next_idx;
	}
	spin_unlock_irqrestore(&log_lock, flags);


	/* Store private data */
	file->private_data = data;

	return 0;
}

static int netlog_log_release(struct inode *inode, struct file *file)
{
	struct user_data *data = file->private_data;

	if (data == NULL)
		return 0;

	mutex_destroy(&data->lock);
	kfree(data);

	return 0;
}


static const struct file_operations netlog_log_fops = {
	.owner = THIS_MODULE,
	.open = netlog_log_open,
	.read = netlog_log_read,
	.llseek = netlog_log_llseek,
	.poll = netlog_log_poll,
	.release = netlog_log_release,
};


/* Device identifiers */
static dev_t netlog_dev;
static struct cdev netlog_c_dev;
static struct class *netlog_class;


int init_netlog_dev(void)
{
	int err;
	struct device *dev;

	netlog_class = class_create(THIS_MODULE, MODULE_NAME);
	if (IS_ERR(netlog_class))
		return PTR_ERR(netlog_class);

	err =  alloc_chrdev_region(&netlog_dev, 0, 1, MODULE_NAME);
	if (err < 0)
		goto clean_class;

	cdev_init(&netlog_c_dev, &netlog_log_fops);
	err = cdev_add(&netlog_c_dev, netlog_dev, 1);
	if (err < 0)
		goto clean_chrdev_region;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	dev = device_create(netlog_class, NULL, netlog_dev,  MODULE_NAME);
#else
	dev = device_create(netlog_class, NULL, netlog_dev, NULL, MODULE_NAME);
#endif
	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto clean_cdev;
	}

	printk(KERN_INFO MODULE_NAME ":\t[+]Created /dev/"MODULE_NAME" for logs\n");
	return 0;

clean_cdev:
	cdev_del(&netlog_c_dev);
clean_chrdev_region:
	unregister_chrdev_region(netlog_dev, 1);
clean_class:
	class_destroy(netlog_class);
	return err;
}

void destroy_netlog_dev(void)
{
	printk(KERN_INFO MODULE_NAME ":\t[+]Removing /dev/"MODULE_NAME"\n");
	device_destroy(netlog_class, netlog_dev);
	cdev_del(&netlog_c_dev);
	unregister_chrdev_region(netlog_dev, 1);
	class_destroy(netlog_class);
	return;
}
