// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/fs_parser.h>
#include <linux/ipc_logging.h>
#include <linux/usb/functionfs.h>

#include "u_fs.h"

/* Copied from f_fs.c */
struct ffs_io_data {
	bool aio;
	bool read;

	struct kiocb *kiocb;
	struct iov_iter data;
	const void *to_free;
	char *buf;

	struct mm_struct *mm;
	struct work_struct work;

	struct usb_ep *ep;
	struct usb_request *req;
	struct sg_table sgt;
	bool use_sg;

	struct ffs_data *ffs;

	int status;
	struct completion done;
};

/* Copied from f_fs.c */
struct ffs_epfile {
	/* Protects ep->ep and ep->req. */
	struct mutex			mutex;

	struct ffs_data			*ffs;
	struct ffs_ep			*ep;		/* P: ffs->eps_lock */
	struct ffs_epfile		*epfile_in;	/* P: ffs->eps_lock */
	struct ffs_epfile		*epfile_out;	/* P: ffs->eps_lock */

	struct dentry			*dentry;

	/*
	 * Buffer for holding data from partial reads which may happen since
	 * we’re rounding user read requests to a multiple of a max packet size.
	 *
	 * The pointer is initialised with NULL value and may be set by
	 * __ffs_epfile_read_data function to point to a temporary buffer.
	 *
	 * In normal operation, calls to __ffs_epfile_read_buffered will consume
	 * data from said buffer and eventually free it.  Importantly, while the
	 * function is using the buffer, it sets the pointer to NULL.  This is
	 * all right since __ffs_epfile_read_data and __ffs_epfile_read_buffered
	 * can never run concurrently (they are synchronised by epfile->mutex)
	 * so the latter will not assign a new value to the pointer.
	 *
	 * Meanwhile ffs_func_eps_disable frees the buffer (if the pointer is
	 * valid) and sets the pointer to READ_BUFFER_DROP value.  This special
	 * value is crux of the synchronisation between ffs_func_eps_disable and
	 * __ffs_epfile_read_data.
	 *
	 * Once __ffs_epfile_read_data is about to finish it will try to set the
	 * pointer back to its old value (as described above), but seeing as the
	 * pointer is not-NULL (namely READ_BUFFER_DROP) it will instead free
	 * the buffer.
	 *
	 * == State transitions ==
	 *
	 * • ptr == NULL:  (initial state)
	 *   ◦ __ffs_epfile_read_buffer_free: go to ptr == DROP
	 *   ◦ __ffs_epfile_read_buffered:    nop
	 *   ◦ __ffs_epfile_read_data allocates temp buffer: go to ptr == buf
	 *   ◦ reading finishes:              n/a, not in ‘and reading’ state
	 * • ptr == DROP:
	 *   ◦ __ffs_epfile_read_buffer_free: nop
	 *   ◦ __ffs_epfile_read_buffered:    go to ptr == NULL
	 *   ◦ __ffs_epfile_read_data allocates temp buffer: free buf, nop
	 *   ◦ reading finishes:              n/a, not in ‘and reading’ state
	 * • ptr == buf:
	 *   ◦ __ffs_epfile_read_buffer_free: free buf, go to ptr == DROP
	 *   ◦ __ffs_epfile_read_buffered:    go to ptr == NULL and reading
	 *   ◦ __ffs_epfile_read_data:        n/a, __ffs_epfile_read_buffered
	 *                                    is always called first
	 *   ◦ reading finishes:              n/a, not in ‘and reading’ state
	 * • ptr == NULL and reading:
	 *   ◦ __ffs_epfile_read_buffer_free: go to ptr == DROP and reading
	 *   ◦ __ffs_epfile_read_buffered:    n/a, mutex is held
	 *   ◦ __ffs_epfile_read_data:        n/a, mutex is held
	 *   ◦ reading finishes and …
	 *     … all data read:               free buf, go to ptr == NULL
	 *     … otherwise:                   go to ptr == buf and reading
	 * • ptr == DROP and reading:
	 *   ◦ __ffs_epfile_read_buffer_free: nop
	 *   ◦ __ffs_epfile_read_buffered:    n/a, mutex is held
	 *   ◦ __ffs_epfile_read_data:        n/a, mutex is held
	 *   ◦ reading finishes:              free buf, go to ptr == DROP
	 */
	struct ffs_buffer		*read_buffer;
#define READ_BUFFER_DROP ((struct ffs_buffer *)ERR_PTR(-ESHUTDOWN))

	char				name[8];

	unsigned char			in;	/* P: ffs->eps_lock */
	unsigned char			isoc;	/* P: ffs->eps_lock */

	u8				zlp_enabled; /* P: ffs->eps_lock */
	bool				is_rw_proxy;

	/* Protects dmabufs */
	struct mutex			dmabufs_mutex;
	struct list_head		dmabufs; /* P: dmabufs_mutex */
	atomic_t			seqno;
};

/* Copied from f_fs.c */
struct ffs_ep {
	struct usb_ep			*ep;	/* P: ffs->eps_lock */
	struct usb_request		*req;	/* P: epfile->mutex */

	/* [0]: full speed, [1]: high speed, [2]: super speed */
	struct usb_endpoint_descriptor	*descs[3];

	u8				num;
};

/* Copied from f_fs.c */
struct ffs_sb_fill_data {
	struct ffs_file_perms perms;
	umode_t root_mode;
	const char *dev_name;
	bool no_disconnect;
	struct ffs_data *ffs_data;
};

/* Copied from f_fs.c */
struct ffs_function {
	struct usb_configuration	*conf;
	struct usb_gadget		*gadget;
	struct ffs_data			*ffs;

	struct ffs_ep			*eps;
	u8				eps_revmap[16];
	short				*interfaces_nums;

	struct usb_function		function;
	int				cur_alt[MAX_CONFIG_INTERFACES];
};

/* Copied from f_fs.c */
enum ffs_os_desc_type {
	FFS_OS_DESC, FFS_OS_DESC_EXT_COMPAT, FFS_OS_DESC_EXT_PROP
};

#define kprobe_log(context, fmt, ...) \
	ipc_log_string(context, "%s: " fmt, \
		get_kretprobe(ri)->kp.symbol_name, ##__VA_ARGS__)

#define MAX_IPC_INSTANCES 9

/* per-probe private data */
struct kprobe_data {
	void *context;
	void *x0;
	void *x1;
	void *x2;
};

#define MAX_DEV_NAME_SIZE	41 /* it match size defined in ffs_dev */

struct ipc_log_work {
	struct work_struct ctxt_work;
	char dev_name[MAX_DEV_NAME_SIZE];
};

/* per-device IPC log data */
struct ipc_log {
	void *context;
	char dev_name[MAX_DEV_NAME_SIZE];
};

static struct workqueue_struct *ipc_wq;
static struct ipc_log ipc_log_s[MAX_IPC_INSTANCES];

/* Number of devices for f_fs driver */
static int num_devices;
static DEFINE_SPINLOCK(ipc_lock);

static int ipc_inst_dev_exists(const char *dev_name)
{
	int i;

	for (i = 0; i < num_devices; i++) {
		if (ipc_log_s[i].dev_name[0] == '\0')
			return -ENODEV;

		if (!strcmp(ipc_log_s[i].dev_name, dev_name))
			return i;
	}

	return -ENODEV;
}

static void create_ipc_context_work(struct work_struct *w)
{
	struct ipc_log_work *ipc_w = container_of(w, struct ipc_log_work,
						ctxt_work);
	char ipcname[50] = "usb_ffs_";
	void *ctx;
	int i;
	unsigned long flags;

	i = ipc_inst_dev_exists(ipc_w->dev_name);
	if (i >= 0) {
		goto exit;
	}

	if (num_devices >= MAX_IPC_INSTANCES) {
		pr_err("Can't create any more FFS log contexts\n");
		goto exit;
	}

	strlcat(ipcname, ipc_w->dev_name, sizeof(ipcname));
	ctx = ipc_log_context_create(10, ipcname, 0);
	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("%s: Could not create IPC log context for device %s\n",
			__func__, ipc_w->dev_name);
		goto exit;
	}

	spin_lock_irqsave(&ipc_lock, flags);
	ipc_log_s[num_devices].context = ctx;
	strscpy(ipc_log_s[num_devices].dev_name, ipc_w->dev_name, MAX_DEV_NAME_SIZE);
	num_devices++;
	spin_unlock_irqrestore(&ipc_lock, flags);

exit:
	kfree(ipc_w);
}

static void create_ipc_context(const char *dev_name)
{
	struct ipc_log_work *ipc_w;

	ipc_w = kzalloc(sizeof(*ipc_w), GFP_ATOMIC);
	if (!ipc_w)
		return;

	INIT_WORK(&ipc_w->ctxt_work, create_ipc_context_work);
	strscpy(ipc_w->dev_name, dev_name, MAX_DEV_NAME_SIZE);

	queue_work(ipc_wq, &ipc_w->ctxt_work);
}

/* Contexts stay resident until all probes and creation work have drained. */
static void *get_ipc_context(struct ffs_data *ffs)
{
	void *context = NULL;
	unsigned long flags;
	int idx;

	if (!ffs || !ffs->dev_name)
		return NULL;
	spin_lock_irqsave(&ipc_lock, flags);
	idx = ipc_inst_dev_exists(ffs->dev_name);
	if (idx >= 0)
		context = ipc_log_s[idx].context;
	spin_unlock_irqrestore(&ipc_lock, flags);
	return context;
}

static int entry_ffs_user_copy_worker(struct kretprobe_instance *ri,
				      struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct work_struct *work = (struct work_struct *)regs->regs[0];
	struct ffs_io_data *io_data = container_of(work, struct ffs_io_data, work);
	int ret = io_data->status;
	struct ffs_data *ffs = io_data->ffs;
	void *context = get_ipc_context(ffs);

	data->x0 = work;
	kprobe_log(context, "enter: ret %d for %s",
		ret, io_data->read ? "read" : "write");
	return 1;
}



static int entry_ffs_epfile_async_io_complete(struct kretprobe_instance *ri,
					      struct pt_regs *regs)
{
	struct usb_request *req = (struct usb_request *)regs->regs[1];
	struct ffs_io_data *io_data = req->context;
	void *context = get_ipc_context(io_data->ffs);

	kprobe_log(context, "enter");
	return 1;
}

static int entry_ffs_epfile_write_iter(struct kretprobe_instance *ri,
				       struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct kiocb *kiocb = (struct kiocb *)regs->regs[0];
	struct ffs_epfile *epfile = kiocb->ki_filp->private_data;
	void *context = get_ipc_context(epfile->ffs);

	data->x0 = kiocb;
	data->context = context;
	kprobe_log(context, "enter");
	return 0;
}

static int exit_ffs_epfile_write_iter(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}

static int entry_ffs_epfile_read_iter(struct kretprobe_instance *ri,
				      struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct kiocb *kiocb = (struct kiocb *)regs->regs[0];
	struct ffs_epfile *epfile = kiocb->ki_filp->private_data;
	void *context = get_ipc_context(epfile->ffs);

	data->x0 = kiocb;
	data->context = context;
	kprobe_log(context, "enter");
	return 0;
}

static int exit_ffs_epfile_read_iter(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}

static int entry_ffs_data_put(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct ffs_data *ffs = (struct ffs_data *)regs->regs[0];
	void *context = get_ipc_context(ffs);
	unsigned int refcount = refcount_read(&ffs->ref);

	kprobe_log(context, "ref %u", refcount);

	return 1;
}

static int entry_ffs_sb_fill(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct fs_context *fc = (struct fs_context *)regs->regs[1];
	struct ffs_sb_fill_data *data = fc->fs_private;

	create_ipc_context(data->ffs_data->dev_name);

	return 1;
}

static int entry___ffs_ep0_queue_wait(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct ffs_data *ffs = (struct ffs_data *)regs->regs[0];
	void *context = get_ipc_context(ffs);

	data->x0 = ffs;
	data->context = context;
	kprobe_log(context, "enter: state %d setup_state %d flags %lu",
		ffs->state, ffs->setup_state, ffs->flags);
	return 0;
}

static int exit___ffs_ep0_queue_wait(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}

static int entry_ffs_ep0_write(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct file *file = (struct file *)regs->regs[0];
	struct ffs_data *ffs = file->private_data;
	unsigned int len = (unsigned int)regs->regs[2];
	void *context = get_ipc_context(ffs);

	data->x0 = file;
	data->context = context;
	kprobe_log(context, "enter:len %u state %d setup_state %d flags %lu",
		len, ffs->state, ffs->setup_state, ffs->flags);
	return 0;
}

static int exit_ffs_ep0_write(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}

static int entry_ffs_ep0_read(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct file *file = (struct file *)regs->regs[0];
	size_t len = (size_t)regs->regs[2];
	struct ffs_data *ffs = file->private_data;
	void *context = get_ipc_context(ffs);
	size_t n;

	data->x0 = file;
	n = min((len / sizeof(struct usb_functionfs_event)), (size_t)ffs->ev.count);
	data->context = context;
	kprobe_log(context, "enter:len %zu state %d setup_state %d flags %lu n %zu",
		len, ffs->state, ffs->setup_state, ffs->flags, n);
	return 0;
}

static int exit_ffs_ep0_read(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}

static int entry_ffs_ep0_open(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct inode *inode = (struct inode *)regs->regs[0];
	struct ffs_data *ffs = inode->i_private;
	void *context = get_ipc_context(ffs);

	kprobe_log(context, "state %d setup_state %d flags %lu opened %d",
		ffs->state, ffs->setup_state, ffs->flags,
		atomic_read(&ffs->opened));
	return 1;
}

static int entry_ffs_ep0_release(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct file *file = (struct file *)regs->regs[1];
	struct ffs_data *ffs = file->private_data;
	void *context = get_ipc_context(ffs);

	kprobe_log(context, "state %d setup_state %d flags %lu opened %d",
		ffs->state, ffs->setup_state, ffs->flags,
		atomic_read(&ffs->opened));
	return 1;
}

static int entry_ffs_ep0_ioctl(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct file *file = (struct file *)regs->regs[0];
	struct ffs_data *ffs = file->private_data;
	void *context = get_ipc_context(ffs);

	kprobe_log(context, "state %d setup_state %d flags %lu opened %d",
		ffs->state, ffs->setup_state, ffs->flags,
		atomic_read(&ffs->opened));
	return 1;
}

static int entry_ffs_epfile_open(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct inode *inode = (struct inode *)regs->regs[0];
	struct ffs_epfile *epfile = inode->i_private;
	void *context = get_ipc_context(epfile->ffs);

	kprobe_log(context, "%s: state %d setup_state %d flag %lu opened %u",
		epfile->name, epfile->ffs->state, epfile->ffs->setup_state,
		epfile->ffs->flags, atomic_read(&epfile->ffs->opened));
	return 1;
}

static int entry_ffs_aio_cancel(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct kiocb *kiocb = (struct kiocb *)regs->regs[0];
	struct ffs_epfile *epfile = kiocb->ki_filp->private_data;
	void *context = get_ipc_context(epfile->ffs);

	data->x0 = kiocb;
	data->context = context;
	kprobe_log(context, "enter:state %d setup_state %d flag %lu",
		epfile->ffs->state, epfile->ffs->setup_state,
		epfile->ffs->flags);
	return 0;
}

static int exit_ffs_aio_cancel(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}

static int entry_ffs_epfile_release(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct inode *inode = (struct inode *)regs->regs[0];
	struct ffs_epfile *epfile = inode->i_private;
	void *context = get_ipc_context(epfile->ffs);

	kprobe_log(context, "%s: state %d setup_state %d flag %lu opened %u",
		epfile->name, epfile->ffs->state, epfile->ffs->setup_state,
		epfile->ffs->flags, atomic_read(&epfile->ffs->opened));
	return 1;
}

static int entry_ffs_epfile_ioctl(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct file *file = (struct file *)regs->regs[0];
	unsigned int code = (unsigned int)regs->regs[1];
	unsigned long value = (unsigned long)regs->regs[2];
	struct ffs_epfile *epfile = file->private_data;
	void *context = get_ipc_context(epfile->ffs);

	data->x0 = file;
	data->context = context;
	kprobe_log(context,
		"%s: code 0x%08x value %#lx state %d setup_state %d flag %lu",
		epfile->name, code, value, epfile->ffs->state,
		epfile->ffs->setup_state, epfile->ffs->flags);
	return 0;
}

static int exit_ffs_epfile_ioctl(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}


static int entry_ffs_data_closed(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct ffs_data *ffs = (struct ffs_data *)regs->regs[0];
	void *context = get_ipc_context(ffs);

	kprobe_log(context, "state %d setup_state %d flag %lu opened %d",
		ffs->state, ffs->setup_state, ffs->flags,
		atomic_read(&ffs->opened));
	return 1;
}

static int entry_ffs_data_clear(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct ffs_data *ffs = (struct ffs_data *)regs->regs[0];
	void *context = get_ipc_context(ffs);

	kprobe_log(context, "enter: state %d setup_state %d flag %lu",
		ffs->state, ffs->setup_state, ffs->flags);
	return 1;
}



static int entry_ffs_func_eps_disable(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct ffs_function *func = (struct ffs_function *)regs->regs[0];
	void *context = get_ipc_context(func->ffs);

	kprobe_log(context, "enter: state %d setup_state %d flag %lu",
		func->ffs->state, func->ffs->setup_state, func->ffs->flags);
	return 1;
}



static int entry_ffs_reset_work(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct work_struct *work = (struct work_struct *)regs->regs[0];
	struct ffs_data *ffs = container_of(work, struct ffs_data, reset_work);
	void *context = get_ipc_context(ffs);

	kprobe_log(context, "enter");
	return 1;
}

static int entry_ffs_func_set_alt(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct usb_function *f = (struct usb_function *)regs->regs[0];
	struct ffs_function *func = container_of(f, struct ffs_function, function);
	unsigned int alt = (unsigned int)regs->regs[2];
	void *context = get_ipc_context(func->ffs);

	data->x0 = func;
	kprobe_log(context, "enter: alt %d", (int)alt);
	return 1;
}

static int entry_ffs_func_disable(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct usb_function *f = (struct usb_function *)regs->regs[0];
	struct ffs_function *func = container_of(f, struct ffs_function, function);
	void *context = get_ipc_context(func->ffs);

	kprobe_log(context, "enter");
	return 1;
}

static int entry_ffs_func_setup(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct usb_function *f = (struct usb_function *)regs->regs[0];
	struct ffs_function *func = container_of(f, struct ffs_function, function);
	struct usb_ctrlrequest *creq = (struct usb_ctrlrequest *)regs->regs[1];
	struct ffs_data *ffs = func->ffs;
	void *context = get_ipc_context(func->ffs);

	kprobe_log(context,
		"enter: state %d reqtype=%02x req=%02x wv=%04x wi=%04x wl=%04x",
			ffs->state, creq->bRequestType, creq->bRequest,
			le16_to_cpu(creq->wValue), le16_to_cpu(creq->wIndex),
			le16_to_cpu(creq->wLength));
	return 1;
}

static int entry_ffs_func_suspend(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct usb_function *f = (struct usb_function *)regs->regs[0];
	struct ffs_function *func = container_of(f, struct ffs_function, function);
	void *context = get_ipc_context(func->ffs);

	kprobe_log(context, "enter");
	return 1;
}

static int entry_ffs_func_resume(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct usb_function *f = (struct usb_function *)regs->regs[0];
	struct ffs_function *func = container_of(f, struct ffs_function, function);
	void *context = get_ipc_context(func->ffs);

	kprobe_log(context, "enter");
	return 1;
}

static int entry_ffs_func_unbind(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;
	struct usb_function *f = (struct usb_function *)regs->regs[1];
	struct ffs_function *func = container_of(f, struct ffs_function, function);
	struct ffs_data *ffs = func->ffs;
	struct f_fs_opts *opts =
		container_of(f->fi, struct f_fs_opts, func_inst);
	void *context = get_ipc_context(ffs);

	data->x1 = ffs;
	data->context = context;
	kprobe_log(context, "enter: state %d setup_state %d flag %lu refcnt %u",
		ffs->state, ffs->setup_state, ffs->flags, opts->refcnt);
	return 0;
}

static int exit_ffs_func_unbind(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct kprobe_data *data = (struct kprobe_data *)ri->data;

	kprobe_log(data->context, "exit: ret %ld", (long)regs_return_value(regs));
	return 0;
}



#define ENTRY_EXIT(name) {\
	.handler = exit_##name,\
	.entry_handler = entry_##name,\
	.data_size = sizeof(struct kprobe_data),\
	.maxactive = MAX_IPC_INSTANCES,\
	.kp.symbol_name = #name,\
}

#define ENTRY(name) {\
	.entry_handler = entry_##name,\
	.data_size = sizeof(struct kprobe_data),\
	.maxactive = MAX_IPC_INSTANCES,\
	.kp.symbol_name = #name,\
}

static struct kretprobe ffsprobes[] = {
	ENTRY(ffs_user_copy_worker),
	ENTRY(ffs_epfile_async_io_complete),
	ENTRY_EXIT(ffs_epfile_write_iter),
	ENTRY_EXIT(ffs_epfile_read_iter),
	ENTRY(ffs_data_put),
	ENTRY(ffs_sb_fill),
	ENTRY_EXIT(__ffs_ep0_queue_wait),
	ENTRY_EXIT(ffs_ep0_write),
	ENTRY_EXIT(ffs_ep0_read),
	ENTRY(ffs_ep0_open),
	ENTRY(ffs_ep0_release),
	ENTRY(ffs_ep0_ioctl),
	ENTRY(ffs_epfile_open),
	ENTRY_EXIT(ffs_aio_cancel),
	ENTRY(ffs_epfile_release),
	ENTRY_EXIT(ffs_epfile_ioctl),
	ENTRY(ffs_data_closed),
	ENTRY(ffs_data_clear),
	ENTRY(ffs_func_eps_disable),
	ENTRY(ffs_reset_work),
	ENTRY(ffs_func_set_alt),
	ENTRY(ffs_func_disable),
	ENTRY(ffs_func_setup),
	ENTRY(ffs_func_suspend),
	ENTRY(ffs_func_resume),
	ENTRY_EXIT(ffs_func_unbind),
};

static int __init kretprobe_init(void)
{
	int ret, i;

	ipc_wq = alloc_ordered_workqueue("ffs_ipc_wq", 0);
	if (!ipc_wq)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(ffsprobes); i++) {
		ret = register_kretprobe(&ffsprobes[i]);
		if (ret) {
			pr_err("FunctionFS probe %s failed: %d\n",
			       ffsprobes[i].kp.symbol_name, ret);
			while (i--)
				unregister_kretprobe(&ffsprobes[i]);
			destroy_workqueue(ipc_wq);
			for (i = 0; i < num_devices; i++)
				ipc_log_context_destroy(ipc_log_s[i].context);
			return ret;
		}
	}
	return 0;
}

static void __exit kretprobe_exit(void)
{
	int i;

	/* Stop producers/readers before destroying their workqueue or contexts. */
	for (i = 0; i < ARRAY_SIZE(ffsprobes); i++)
		unregister_kretprobe(&ffsprobes[i]);
	destroy_workqueue(ipc_wq);
	for (i = 0; i < num_devices; i++)
		ipc_log_context_destroy(ipc_log_s[i].context);
}

module_init(kretprobe_init)
module_exit(kretprobe_exit)
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FunctionFS IPC diagnostics for pinned Android 17 Linux 6.18");
