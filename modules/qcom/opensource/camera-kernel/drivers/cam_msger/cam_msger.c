// MIUI ADD: Camera_CameraOpt
#include "cam_msger.h"
#include "cam_sched.h"
#include "cam_pid.h"

#define CREATE_READER_ERROR_EXIST -1002
#define CREATE_READER_ERROR_NOSPACE -1003
#define CREATE_READER_ERROR_NAME_TOO_LONG -1004

#define EXIT_SIGNALE_ERROR_READER_NOT_FOUND -1005

#define RELEASE_ERROR_FILE_ADDR_INVALID -1006
#define RELEASE_ERROR_READER_NOT_FOUND -1007

#define WRITE_ERROR_UNSPORT_MSG_TYPE -1008
#define WRITE_ERROR_BUF_ADDR_INVALID -1009
#define WRITE_ERROR_FORMAT_INVALID -1010
#define WRITE_ERROR_NO_RECEVER -1011
#define WRITE_ERROR_NO_MSG_SPACE -1012
#define WRITE_ERROR_LEN_INVALID -1013
#define WRITE_ERROR_CPY_FROM_USER -1014

#define READED_ERROR_NO_REG -1015
#define READER_ERROR_EMPTY -1016
#define READER_ERROR_COPY_TO_USER_FAILED -1017

#define WRITE_ERROR_NO_PERMISSION -1018

#define NORMAL_APP_MIN_UID 10000

#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uidgid.h>

unsigned int msger_siwtches = COC_ENABLE;
static cammsger_dev c_msger_dev;
static bool msger_ready;
static bool cameramsger_is_ready(void)
{
	/* Pair with initialization publication before accessing queues or locks. */
	return smp_load_acquire(&msger_ready);
}

static submodule submodules[] = {
	//{init, write, read, pool, ioctl, release}
	{ cam_sched_init, cam_sched_w_pids, NULL, NULL, cam_sched_ioctl,
	  NULL }, //cam_sched.h
	{ NULL, get_tgid_by_cmdline, NULL, NULL, NULL, NULL } //cam_pid.h
};

u8 debug(int log_type)
{
	return log_type == READ_ONCE(c_msger_dev.debug);
}

static void dump(void)
{
	int j = 0;
	int idx = 0;
	mutex_lock(&c_msger_dev.g_lock);
	printk(KERN_INFO "Reader count%d debug:%d\n",
	       c_msger_dev.r_entity_count, c_msger_dev.debug);
	for (idx = 0; idx < READER_COUNT_MAX; idx++) {
		if (!c_msger_dev.readers[idx].used)
			continue;
		printk(KERN_INFO "Reader %s:\n", c_msger_dev.readers[idx].name);
		printk(KERN_INFO "   used:%d pid:%d tid:%d fp:%p name:%s:\n",
		       c_msger_dev.readers[idx].used,
		       c_msger_dev.readers[idx].tgid,
		       c_msger_dev.readers[idx].pid,
		       c_msger_dev.readers[idx].fp,
		       c_msger_dev.readers[idx].name);
		printk(KERN_INFO "    front:%d rear:%d Messages:\n",
		       c_msger_dev.readers[idx].front,
		       c_msger_dev.readers[idx].rear);
		printk(KERN_INFO "Message List:\n");
		for (j = 0; j < MSG_COUNT_MAX; j++) {
			printk(KERN_INFO "    %d:from:%s to:%s [%s]\n", j,
			       c_msger_dev.readers[idx].msgs[j].from,
			       c_msger_dev.readers[idx].msgs[j].to,
			       c_msger_dev.readers[idx].msgs[j].data);
		}
	}
	mutex_unlock(&c_msger_dev.g_lock);
}

static void clear_r_entity_locked(int idx)
{
	if (idx < 0 || idx >= READER_COUNT_MAX || !c_msger_dev.readers[idx].used)
		return;

	c_msger_dev.readers[idx].pid = 0;
	c_msger_dev.readers[idx].tgid = 0;
	c_msger_dev.readers[idx].used = 0;
	c_msger_dev.readers[idx].rear = 0;
	c_msger_dev.readers[idx].front = 0;
	c_msger_dev.readers[idx].fp = NULL;
	memset(&(c_msger_dev.readers[idx].name), 0,
	       sizeof(char) * READER_ID_MAX);
	memset(&(c_msger_dev.readers[idx].msgs), 0,
	       sizeof(m_item) * MSG_COUNT_MAX);
	c_msger_dev.r_entity_count--;

	wake_up_interruptible(&(c_msger_dev.readers[idx].wait_queue));
}

static int find_reader_by_fp(struct file *filp)
{
	int idx = 0;
	if (filp == NULL)
		return READER_COUNT_MAX;
	for (idx = 0; idx < READER_COUNT_MAX; idx++) {
		if (!c_msger_dev.readers[idx].used)
			continue;
		if (c_msger_dev.readers[idx].fp == filp)
			return idx;
	}
	return idx;
}

static int find_reader_by_name_or_tgid(char *name, int tgid)
{
	int idx = 0;
	for (idx = 0; idx < READER_COUNT_MAX; idx++) {
		if (!c_msger_dev.readers[idx].used)
			continue;
		if (name != NULL &&
		    strcmp(name, c_msger_dev.readers[idx].name) == 0)
			return idx;
		if (tgid > 0 && c_msger_dev.readers[idx].tgid == tgid)
			return idx;
	}
	return idx;
}

static int find_new_reader_idx(void)
{
	int idx = 0;
	m_pr(LTYPE_COC, "find_new_reader_idx \n");
	for (idx = 0; idx < READER_COUNT_MAX; idx++) {
		if (!c_msger_dev.readers[idx].used)
			return idx;
	}
	return idx;
}

static int cameramsger_open(struct inode *inode, struct file *filp)
{
	return cameramsger_is_ready() ? 0 : -ENODEV;
}

static __poll_t cameramsger_poll(struct file *file, struct poll_table_struct *poll_table)
{
	__poll_t mask = 0;
	int idx;

	if (!cameramsger_is_ready())
		return EPOLLERR;
	mutex_lock(&c_msger_dev.g_lock);
	idx = find_reader_by_fp(file);
	if ((READ_ONCE(msger_siwtches) & COC_ENABLE) && idx < READER_COUNT_MAX &&
	    current->tgid == c_msger_dev.readers[idx].tgid) {
		poll_wait(file, &c_msger_dev.readers[idx].wait_queue, poll_table);
		if (c_msger_dev.readers[idx].front != c_msger_dev.readers[idx].rear)
			mask = EPOLLIN | EPOLLRDNORM;
	}
	mutex_unlock(&c_msger_dev.g_lock);
	return mask;
}

static ssize_t cameramsger_read(struct file *fp, char __user *buff, size_t length,
				loff_t *ppos)
{
	r_entity *reader;
	size_t len;
	int idx, ret = READER_ERROR_EMPTY;

	if (!length)
		return 0;
	if (!cameramsger_is_ready())
		return -ENODEV;
	mutex_lock(&c_msger_dev.g_lock);
	idx = find_reader_by_fp(fp);
	if (!(READ_ONCE(msger_siwtches) & COC_ENABLE) || idx == READER_COUNT_MAX)
		goto out;
	reader = &c_msger_dev.readers[idx];
	if (current->tgid != reader->tgid || reader->front == reader->rear)
		goto out;
	len = strnlen(reader->msgs[reader->front].data, MSG_LEN_MAX);
	if (length < len) {
		ret = -EMSGSIZE;
		goto out;
	}
	/* Keep complete records queued after short reads or user-copy faults. */
	if (copy_to_user(buff, reader->msgs[reader->front].data,
			 min_t(size_t, length, len + 1))) {
		ret = READER_ERROR_COPY_TO_USER_FAILED;
		goto out;
	}
	reader->front = (reader->front + 1) % MSG_COUNT_MAX;
	ret = len;
out:
	mutex_unlock(&c_msger_dev.g_lock);
	return ret;
}

static int switch_update(struct file *fp, char *buff, int length)
{
	unsigned int value;
	int ret;

	if (length < 5 || buff[3] != '|' || kstrtouint(buff + 4, 10, &value) ||
	    (value & ~(COC_ENABLE | SCD_ENABLE | PID_ENABLE | BID_ENABLE)))
		return WRITE_ERROR_FORMAT_INVALID;
	if (value & SCD_ENABLE) {
		ret = cam_sched_enable();
		if (ret)
			return ret;
	}
	WRITE_ONCE(msger_siwtches, value);
	return length;
}

/*Acquires c_msger_dev.g_lock before invoking reg_reader*/
static int reg_reader(struct file *fp, char *buff, int length)
{
	int reader_idx = -1;
	r_entity *reader = NULL;

	m_pr(LTYPE_COC, "%s\n", __func__);

	if (length < 5)
		return WRITE_ERROR_LEN_INVALID;
	if (buff[3] != '|')
		return WRITE_ERROR_FORMAT_INVALID;
	if (strlen(&buff[4]) < 1)
		return WRITE_ERROR_FORMAT_INVALID;
	if (strlen(&buff[4]) >= READER_ID_MAX)
		return CREATE_READER_ERROR_NAME_TOO_LONG;
	m_pr(LTYPE_COC, "%s data:%s reader_name:%s\n", __func__, buff,
	     &buff[4]);

	//check exist
	if (find_reader_by_fp(fp) < READER_COUNT_MAX)
		return CREATE_READER_ERROR_EXIST;
	reader_idx = find_reader_by_name_or_tgid(&buff[4], current->tgid);
	m_pr(LTYPE_COC, "%s reader_already_registed reader_idx:%d\n", __func__,
	     reader_idx);
	if (reader_idx < READER_COUNT_MAX)
		return CREATE_READER_ERROR_EXIST;

	//check have space
	reader_idx = find_new_reader_idx();
	if (reader_idx >= READER_COUNT_MAX)
		return CREATE_READER_ERROR_NOSPACE;
	//add reader
	reader = &(c_msger_dev.readers[reader_idx]);
	reader->fp = fp;
	reader->used = 1;
	reader->pid = current->pid;
	reader->tgid = current->tgid;
	c_msger_dev.r_entity_count++;
	snprintf(reader->name, READER_ID_MAX, "%s", &buff[4]);

	m_pr(LTYPE_COC, "reader reg success! tid:%d, pid:%d, name:%s, fp:%p \n",
	     reader->pid, reader->tgid, reader->name, reader->fp);
	return length;
}

/*Acquires c_msger_dev.g_lock before invoking add_message*/
static int add_message(int reader_idx, m_item *item)
{
	int rear = 0;
	r_entity *reader = NULL;

	if (reader_idx < 0 || reader_idx >= READER_COUNT_MAX)
		return WRITE_ERROR_NO_MSG_SPACE;
	reader = &(c_msger_dev.readers[reader_idx]);
	if (!reader->used)
		return WRITE_ERROR_NO_RECEVER;

	rear = (reader->rear + 1) % MSG_COUNT_MAX;
	if (rear == reader->front) {
		reader->front = (reader->front + 1) % MSG_COUNT_MAX;
	}
	memcpy(&(reader->msgs[reader->rear]), item, sizeof(m_item));
	reader->rear = rear;

	wake_up_interruptible(&(reader->wait_queue));

	m_pr(LTYPE_COC, "%s add message to %d success\n", __func__,
	     reader->pid);
	return 0;
}

/*Acquires c_msger_dev.g_lock before invoking snd_message*/
static int snd_message(char *buff, int length)
{
	m_item *item;
	char receiver[READER_ID_MAX];
	char *first, *second, *third;
	size_t receiver_len, sender_len, message_len;
	int idx, ret;

	if (length <= 7 || length >= MSG_LEN_MAX || strncmp(buff, "snd|", 4))
		return WRITE_ERROR_FORMAT_INVALID;
	first = buff + 3;
	second = strchr(first + 1, '|');
	if (!second)
		return WRITE_ERROR_FORMAT_INVALID;
	third = strchr(second + 1, '|');
	if (!third)
		return WRITE_ERROR_FORMAT_INVALID;
	receiver_len = second - first - 1;
	sender_len = third - second - 1;
	message_len = length - (second + 1 - buff);
	if (!receiver_len || receiver_len >= READER_ID_MAX || !sender_len ||
	    sender_len >= READER_ID_MAX || message_len >= MSG_LEN_MAX)
		return WRITE_ERROR_FORMAT_INVALID;
	memcpy(receiver, first + 1, receiver_len);
	receiver[receiver_len] = '\0';
	idx = find_reader_by_name_or_tgid(receiver, -1);
	if (idx >= READER_COUNT_MAX)
		return WRITE_ERROR_NO_RECEVER;
	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;
	memcpy(item->to, receiver, receiver_len);
	memcpy(item->from, second + 1, sender_len);
	/* Native readers receive sender|payload, including the sender field. */
	memcpy(item->data, second + 1, message_len);
	ret = add_message(idx, item);
	kfree(item);
	return ret ? ret : length;
}

static ssize_t cameramsger_write(struct file *fp, const char __user *buff,
				 size_t length, loff_t *ppos)
{
	u8 hit = 0;
	int cpy_len = -1, len_submodules = 0;
	int idx = -1;
	int retv = WRITE_ERROR_UNSPORT_MSG_TYPE;

	m_pr(LTYPE_COC, "%s len:%zu", __func__, length);
	if (length <= 4 || length >= MSG_LEN_MAX)
		return WRITE_ERROR_LEN_INVALID;
	if (!cameramsger_is_ready())
		return -ENODEV;
	if (buff == NULL)
		return WRITE_ERROR_BUF_ADDR_INVALID;

	m_pr(LTYPE_COC, "%s current thread tid:%d uid:%d\n", __func__,
	     current->pid, from_kuid(&init_user_ns, current_uid()));

	if (from_kuid(&init_user_ns, current_uid()) >= NORMAL_APP_MIN_UID)
		return WRITE_ERROR_NO_PERMISSION;

	mutex_lock(&c_msger_dev.g_lock);
	memset(&(c_msger_dev.d_cache), 0, sizeof(char) * MSG_LEN_MAX);
	cpy_len = copy_from_user(&(c_msger_dev.d_cache[0]), buff, length);
	if (cpy_len) {
		retv = WRITE_ERROR_CPY_FROM_USER;
		goto __func_done;
	}
	cpy_len = strnlen(c_msger_dev.d_cache, length);
	if (cpy_len < length - 1) {
		retv = WRITE_ERROR_FORMAT_INVALID;
		goto __func_done;
	}
	length = cpy_len;

	m_pr(LTYPE_COC, "%s: copy after:%s len:%zu cpy_len:%d", __func__,
	     &(c_msger_dev.d_cache[0]), length, cpy_len);

	if (strncmp(&(c_msger_dev.d_cache[0]), "swh", 3) == 0) {
		retv = switch_update(fp, &(c_msger_dev.d_cache[0]), length);
		goto __func_done;
	}

	if (READ_ONCE(msger_siwtches) & COC_ENABLE)
	{
		if (strncmp(&(c_msger_dev.d_cache[0]), "reg", 3) == 0) {
			retv = reg_reader(fp, &(c_msger_dev.d_cache[0]),
					  length);
			goto __func_done;
		}
		if (strncmp(&(c_msger_dev.d_cache[0]), "snd", 3) == 0) {
			retv = snd_message(&(c_msger_dev.d_cache[0]), length);
			goto __func_done;
		}
	}
	//check submodules
	len_submodules = ARRAY_SIZE(submodules);
	for (idx = 0; idx < len_submodules; idx++) {
		if (submodules[idx].write != NULL) {
			retv = submodules[idx].write(
				fp, &(c_msger_dev.d_cache[0]), length, &hit);
			m_pr(LTYPE_COC,
			     "%s submodules[%d].write:%p hit:%d retv:%d",
			     __func__, idx, submodules[idx].write, hit, retv);
			if (hit)
				break;
		}
	}

__func_done:
	mutex_unlock(&c_msger_dev.g_lock);
	return retv;
}

static int cameramsger_release(struct inode *inode, struct file *filp)
{
	int idx = 0;
	int rl_pid = 0;
	int reader_idx = 0, len_submodules = 0;
	bool is_rl_task_die = false;
	struct task_struct *rl_task = NULL;

	if (in_interrupt())
		return 0;
	if (filp == NULL)
		return RELEASE_ERROR_FILE_ADDR_INVALID;

	{
		mutex_lock(&c_msger_dev.g_lock);
		reader_idx = find_reader_by_fp(filp);
		if (reader_idx < READER_COUNT_MAX) {
			m_pr(LTYPE_COC,
			     "cameramsger_release find reader:pid:%d tid:%d fp:%p name:%s:\n",
			     c_msger_dev.readers[reader_idx].tgid,
			     c_msger_dev.readers[reader_idx].pid,
			     c_msger_dev.readers[reader_idx].fp,
			     c_msger_dev.readers[reader_idx].name);

			rl_pid = c_msger_dev.readers[reader_idx].tgid;
			rcu_read_lock();
			rl_task = get_pid_task(find_vpid(rl_pid), PIDTYPE_PID);
			rcu_read_unlock();
			if (rl_task == NULL) {
				is_rl_task_die = true;
			} else {
				is_rl_task_die = !(pid_alive(rl_task));
				m_pr(LTYPE_COC, "[%s] pid_alive:%d", __func__,
				     pid_alive(rl_task));
				if (!is_rl_task_die &&
				    (rl_task->__state == TASK_DEAD ||
				     rl_task->exit_state == EXIT_DEAD ||
				     rl_task->exit_state == EXIT_ZOMBIE ||
				     rl_task->files == NULL)) {
					is_rl_task_die = true;
				}
				m_pr(LTYPE_COC,
				     "[%s] other state check result: task_die:%d",
				     __func__, is_rl_task_die);
				put_task_struct(rl_task);
			}

			if (is_rl_task_die) {
				for (idx = 0; idx < READER_COUNT_MAX; idx++) {
					if (!c_msger_dev.readers[idx].used ||
					    idx == reader_idx)
						continue;
					memset(&(c_msger_dev.d_cache), 0,
					       sizeof(char) * MSG_LEN_MAX);
					snprintf(c_msger_dev.d_cache,
						 MSG_LEN_MAX,
						 READER_DEATH_OMEN_TEMPLATE,
						 &(c_msger_dev.readers[idx]
							   .name[0]),
						 READER_DEATH_CAMMIND_MSG_CODE,
						 READER_DEATH_CAMMIND_MSG_CODE,
						 ktime_to_ns(ktime_get()),
						 c_msger_dev.readers[reader_idx]
							 .pid,
						 c_msger_dev.readers[reader_idx]
							 .tgid,
						 c_msger_dev.readers[reader_idx]
							 .name);

					snd_message(
						&(c_msger_dev.d_cache[0]),
						strlen(&(c_msger_dev
								 .d_cache[0])));
					m_pr(LTYPE_COC,
					     "[%s] notify pid:%d that proc-%d is death.\n",
					     __func__,
					     c_msger_dev.readers[idx].tgid,
					     c_msger_dev.readers[reader_idx]
						     .tgid);
				}
			}
			clear_r_entity_locked(reader_idx);
			mutex_unlock(&c_msger_dev.g_lock);
		} else {
			mutex_unlock(&c_msger_dev.g_lock);
		}
	}
	//check submodules
	len_submodules = ARRAY_SIZE(submodules);
	for (idx = 0; idx < len_submodules; idx++) {
		if (submodules[idx].release != NULL) {
			submodules[idx].release(inode, filp);
		}
	}
	return 0;
}

static long cameramsger_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	u8 hit = 0;
	int idx = 0, len_submodules = 0;
	if (!cameramsger_is_ready())
		return -ENODEV;
	m_pr(LTYPE_COC, "%s: %u %lu\n", __func__, cmd, arg);
	switch (cmd) {
	case IOC_CMD_PID_DEBUG_ON:
		mutex_lock(&c_msger_dev.g_lock);
		c_msger_dev.debug = LTYPE_PID;
		mutex_unlock(&c_msger_dev.g_lock);
		return 0;
	case IOC_CMD_COC_DEBUG_ON:
		mutex_lock(&c_msger_dev.g_lock);
		c_msger_dev.debug = LTYPE_COC;
		mutex_unlock(&c_msger_dev.g_lock);
		return 0;
	case IOC_CMD_SCHED_DEBUG_ON:
		mutex_lock(&c_msger_dev.g_lock);
		c_msger_dev.debug = LTYPE_SCHED;
		mutex_unlock(&c_msger_dev.g_lock);
		return 0;
	case IOC_CMD_DEBUG_OFF:
		mutex_lock(&c_msger_dev.g_lock);
		c_msger_dev.debug = 0;
		mutex_unlock(&c_msger_dev.g_lock);
		return 0;
	case IOC_CMD_MSGER_SWITCHES_DUMP:
		printk(KERN_INFO "[%s] msger_siwtches:%x\n", __func__,
		       msger_siwtches);
		return 0;
	case IOC_CMD_DUMP:
		dump();
		return 0;
	}
	//check submodules
	len_submodules = ARRAY_SIZE(submodules);
	for (idx = 0; idx < len_submodules; idx++) {
		if (submodules[idx].ioctl != NULL) {
			submodules[idx].ioctl(filp, cmd, arg, &hit);
			m_pr(LTYPE_COC, "%s: submodules[%d].ioctl:%p hit:%d.\n",
			     __func__, idx, submodules[idx].ioctl, hit);
			if (hit)
				break;
		}
	}
	if (!hit)
		return -ENOTTY;
	return 0;
}

static const struct file_operations cameramsger_fops = {
	.owner = THIS_MODULE,
	.open = cameramsger_open,
	.poll = cameramsger_poll,
	.read = cameramsger_read,
	.write = cameramsger_write,
	.release = cameramsger_release,
	.unlocked_ioctl = cameramsger_ioctl,
};

static int __init cameramsger_init(void)
{
	int idx, ret;

	strscpy(c_msger_dev.name, "cam_msger", sizeof(c_msger_dev.name));
	strscpy(c_msger_dev.version, "2.0", sizeof(c_msger_dev.version));
	mutex_init(&c_msger_dev.g_lock);
	for (idx = 0; idx < READER_COUNT_MAX; idx++)
		init_waitqueue_head(&c_msger_dev.readers[idx].wait_queue);

	ret = register_chrdev(0, c_msger_dev.name, &cameramsger_fops);
	if (ret < 0)
		return ret;
	c_msger_dev.cammsger_major = ret;
	c_msger_dev.cammsger_class = class_create(c_msger_dev.name);
	if (IS_ERR(c_msger_dev.cammsger_class)) {
		ret = PTR_ERR(c_msger_dev.cammsger_class);
		goto unregister;
	}
	c_msger_dev.devt = MKDEV(c_msger_dev.cammsger_major, 0);
	c_msger_dev.device = device_create(c_msger_dev.cammsger_class, NULL,
					 c_msger_dev.devt, NULL, "%s", c_msger_dev.name);
	if (IS_ERR(c_msger_dev.device)) {
		ret = PTR_ERR(c_msger_dev.device);
		goto destroy_class;
	}
	/* No fallible initialization may follow permanent restricted hook registration. */
	ret = cam_sched_init();
	if (ret)
		goto destroy_device;
	/* Publish all initialized queues and permanent hook state to openers. */
	smp_store_release(&msger_ready, true);
	return 0;
destroy_device:
	device_destroy(c_msger_dev.cammsger_class, c_msger_dev.devt);
destroy_class:
	class_destroy(c_msger_dev.cammsger_class);
unregister:
	unregister_chrdev(c_msger_dev.cammsger_major, c_msger_dev.name);
	return ret;
}

/* Native resident module: restricted scheduler hooks cannot be unregistered. */
module_init(cameramsger_init);
MODULE_DESCRIPTION("camera messager device driver");
MODULE_LICENSE("GPL v2");
// END Camera_CameraOpt
