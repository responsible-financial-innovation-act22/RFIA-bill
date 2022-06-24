// SPDX-License-Identifier: GPL-2.0
/*
 * Shared application/kernel submission and completion ring pairs, for
 * supporting fast/efficient IO.
 *
 * A note on the read/write ordering memory barriers that are matched between
 * the application and kernel side.
 *
 * After the application reads the CQ ring tail, it must use an
 * appropriate smp_rmb() to pair with the smp_wmb() the kernel uses
 * before writing the tail (using smp_load_acquire to read the tail will
 * do). It also needs a smp_mb() before updating CQ head (ordering the
 * entry load(s) with the head store), pairing with an implicit barrier
 * through a control-dependency in io_get_cqe (smp_store_release to
 * store head will do). Failure to do so could lead to reading invalid
 * CQ entries.
 *
 * Likewise, the application must use an appropriate smp_wmb() before
 * writing the SQ tail (ordering SQ entry stores with the tail store),
 * which pairs with smp_load_acquire in io_get_sqring (smp_store_release
 * to store the tail will do). And it needs a barrier ordering the SQ
 * head load before writing new SQ entries (smp_load_acquire to read
 * head will do).
 *
 * When using the SQ poll thread (IORING_SETUP_SQPOLL), the application
 * needs to check the SQ flags for IORING_SQ_NEED_WAKEUP *after*
 * updating the SQ tail; a full memory barrier smp_mb() is needed
 * between.
 *
 * Also see the examples in the liburing library:
 *
 *	git://git.kernel.dk/liburing
 *
 * io_uring also uses READ/WRITE_ONCE() for _any_ store or load that happens
 * from data shared between the kernel and application. This is done both
 * for ordering purposes, but also to ensure that once a value is loaded from
 * data that the application could potentially modify, it remains stable.
 *
 * Copyright (C) 2018-2019 Jens Axboe
 * Copyright (c) 2018-2019 Christoph Hellwig
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/syscalls.h>
#include <linux/compat.h>
#include <net/compat.h>
#include <linux/refcount.h>
#include <linux/uio.h>
#include <linux/bits.h>

#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/blk-mq.h>
#include <linux/bvec.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/af_unix.h>
#include <net/scm.h>
#include <linux/anon_inodes.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/nospec.h>
#include <linux/sizes.h>
#include <linux/hugetlb.h>
#include <linux/highmem.h>
#include <linux/namei.h>
#include <linux/fsnotify.h>
#include <linux/fadvise.h>
#include <linux/eventpoll.h>
#include <linux/splice.h>
#include <linux/task_work.h>
#include <linux/pagemap.h>
#include <linux/io_uring.h>
#include <linux/audit.h>
#include <linux/security.h>
#include <linux/xattr.h>

#define CREATE_TRACE_POINTS
#include <trace/events/io_uring.h>

#include <uapi/linux/io_uring.h>

#include "internal.h"
#include "io-wq.h"

#define IORING_MAX_ENTRIES	32768
#define IORING_MAX_CQ_ENTRIES	(2 * IORING_MAX_ENTRIES)
#define IORING_SQPOLL_CAP_ENTRIES_VALUE 8

/* only define max */
#define IORING_MAX_FIXED_FILES	(1U << 20)
#define IORING_MAX_RESTRICTIONS	(IORING_RESTRICTION_LAST + \
				 IORING_REGISTER_LAST + IORING_OP_LAST)

#define IO_RSRC_TAG_TABLE_SHIFT	(PAGE_SHIFT - 3)
#define IO_RSRC_TAG_TABLE_MAX	(1U << IO_RSRC_TAG_TABLE_SHIFT)
#define IO_RSRC_TAG_TABLE_MASK	(IO_RSRC_TAG_TABLE_MAX - 1)

#define IORING_MAX_REG_BUFFERS	(1U << 14)

#define SQE_COMMON_FLAGS (IOSQE_FIXED_FILE | IOSQE_IO_LINK | \
			  IOSQE_IO_HARDLINK | IOSQE_ASYNC)

#define SQE_VALID_FLAGS	(SQE_COMMON_FLAGS | IOSQE_BUFFER_SELECT | \
			IOSQE_IO_DRAIN | IOSQE_CQE_SKIP_SUCCESS)

#define IO_REQ_CLEAN_FLAGS (REQ_F_BUFFER_SELECTED | REQ_F_NEED_CLEANUP | \
				REQ_F_POLLED | REQ_F_INFLIGHT | REQ_F_CREDS | \
				REQ_F_ASYNC_DATA)

#define IO_REQ_CLEAN_SLOW_FLAGS (REQ_F_REFCOUNT | REQ_F_LINK | REQ_F_HARDLINK |\
				 IO_REQ_CLEAN_FLAGS)

#define IO_APOLL_MULTI_POLLED (REQ_F_APOLL_MULTISHOT | REQ_F_POLLED)

#define IO_TCTX_REFS_CACHE_NR	(1U << 10)

struct io_uring {
	u32 head ____cacheline_aligned_in_smp;
	u32 tail ____cacheline_aligned_in_smp;
};

/*
 * This data is shared with the application through the mmap at offsets
 * IORING_OFF_SQ_RING and IORING_OFF_CQ_RING.
 *
 * The offsets to the member fields are published through struct
 * io_sqring_offsets when calling io_uring_setup.
 */
struct io_rings {
	/*
	 * Head and tail offsets into the ring; the offsets need to be
	 * masked to get valid indices.
	 *
	 * The kernel controls head of the sq ring and the tail of the cq ring,
	 * and the application controls tail of the sq ring and the head of the
	 * cq ring.
	 */
	struct io_uring		sq, cq;
	/*
	 * Bitmasks to apply to head and tail offsets (constant, equals
	 * ring_entries - 1)
	 */
	u32			sq_ring_mask, cq_ring_mask;
	/* Ring sizes (constant, power of 2) */
	u32			sq_ring_entries, cq_ring_entries;
	/*
	 * Number of invalid entries dropped by the kernel due to
	 * invalid index stored in array
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application (i.e. get number of "new events" by comparing to
	 * cached value).
	 *
	 * After a new SQ head value was read by the application this
	 * counter includes all submissions that were dropped reaching
	 * the new SQ head (and possibly more).
	 */
	u32			sq_dropped;
	/*
	 * Runtime SQ flags
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application.
	 *
	 * The application needs a full memory barrier before checking
	 * for IORING_SQ_NEED_WAKEUP after updating the sq tail.
	 */
	atomic_t		sq_flags;
	/*
	 * Runtime CQ flags
	 *
	 * Written by the application, shouldn't be modified by the
	 * kernel.
	 */
	u32			cq_flags;
	/*
	 * Number of completion events lost because the queue was full;
	 * this should be avoided by the application by making sure
	 * there are not more requests pending than there is space in
	 * the completion queue.
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application (i.e. get number of "new events" by comparing to
	 * cached value).
	 *
	 * As completion events come in out of order this counter is not
	 * ordered with any other data.
	 */
	u32			cq_overflow;
	/*
	 * Ring buffer of completion events.
	 *
	 * The kernel writes completion events fresh every time they are
	 * produced, so the application is allowed to modify pending
	 * entries.
	 */
	struct io_uring_cqe	cqes[] ____cacheline_aligned_in_smp;
};

struct io_mapped_ubuf {
	u64		ubuf;
	u64		ubuf_end;
	unsigned int	nr_bvecs;
	unsigned long	acct_pages;
	struct bio_vec	bvec[];
};

struct io_ring_ctx;

struct io_overflow_cqe {
	struct list_head list;
	struct io_uring_cqe cqe;
};

/*
 * FFS_SCM is only available on 64-bit archs, for 32-bit we just define it as 0
 * and define IO_URING_SCM_ALL. For this case, we use SCM for all files as we
 * can't safely always dereference the file when the task has exited and ring
 * cleanup is done. If a file is tracked and part of SCM, then unix gc on
 * process exit may reap it before __io_sqe_files_unregister() is run.
 */
#define FFS_NOWAIT		0x1UL
#define FFS_ISREG		0x2UL
#if defined(CONFIG_64BIT)
#define FFS_SCM			0x4UL
#else
#define IO_URING_SCM_ALL
#define FFS_SCM			0x0UL
#endif
#define FFS_MASK		~(FFS_NOWAIT|FFS_ISREG|FFS_SCM)

struct io_fixed_file {
	/* file * with additional FFS_* flags */
	unsigned long file_ptr;
};

struct io_rsrc_put {
	struct list_head list;
	u64 tag;
	union {
		void *rsrc;
		struct file *file;
		struct io_mapped_ubuf *buf;
	};
};

struct io_file_table {
	struct io_fixed_file *files;
	unsigned long *bitmap;
	unsigned int alloc_hint;
};

struct io_rsrc_node {
	struct percpu_ref		refs;
	struct list_head		node;
	struct list_head		rsrc_list;
	struct io_rsrc_data		*rsrc_data;
	struct llist_node		llist;
	bool				done;
};

typedef void (rsrc_put_fn)(struct io_ring_ctx *ctx, struct io_rsrc_put *prsrc);

struct io_rsrc_data {
	struct io_ring_ctx		*ctx;

	u64				**tags;
	unsigned int			nr;
	rsrc_put_fn			*do_put;
	atomic_t			refs;
	struct completion		done;
	bool				quiesce;
};

#define IO_BUFFER_LIST_BUF_PER_PAGE (PAGE_SIZE / sizeof(struct io_uring_buf))
struct io_buffer_list {
	/*
	 * If ->buf_nr_pages is set, then buf_pages/buf_ring are used. If not,
	 * then these are classic provided buffers and ->buf_list is used.
	 */
	union {
		struct list_head buf_list;
		struct {
			struct page **buf_pages;
			struct io_uring_buf_ring *buf_ring;
		};
	};
	__u16 bgid;

	/* below is for ring provided buffers */
	__u16 buf_nr_pages;
	__u16 nr_entries;
	__u16 head;
	__u16 mask;
};

struct io_buffer {
	struct list_head list;
	__u64 addr;
	__u32 len;
	__u16 bid;
	__u16 bgid;
};

struct io_restriction {
	DECLARE_BITMAP(register_op, IORING_REGISTER_LAST);
	DECLARE_BITMAP(sqe_op, IORING_OP_LAST);
	u8 sqe_flags_allowed;
	u8 sqe_flags_required;
	bool registered;
};

enum {
	IO_SQ_THREAD_SHOULD_STOP = 0,
	IO_SQ_THREAD_SHOULD_PARK,
};

struct io_sq_data {
	refcount_t		refs;
	atomic_t		park_pending;
	struct mutex		lock;

	/* ctx's that are using this sqd */
	struct list_head	ctx_list;

	struct task_struct	*thread;
	struct wait_queue_head	wait;

	unsigned		sq_thread_idle;
	int			sq_cpu;
	pid_t			task_pid;
	pid_t			task_tgid;

	unsigned long		state;
	struct completion	exited;
};

#define IO_COMPL_BATCH			32
#define IO_REQ_CACHE_SIZE		32
#define IO_REQ_ALLOC_BATCH		8

struct io_submit_link {
	struct io_kiocb		*head;
	struct io_kiocb		*last;
};

struct io_submit_state {
	/* inline/task_work completion list, under ->uring_lock */
	struct io_wq_work_node	free_list;
	/* batch completion logic */
	struct io_wq_work_list	compl_reqs;
	struct io_submit_link	link;

	bool			plug_started;
	bool			need_plug;
	bool			flush_cqes;
	unsigned short		submit_nr;
	struct blk_plug		plug;
};

struct io_ev_fd {
	struct eventfd_ctx	*cq_ev_fd;
	unsigned int		eventfd_async: 1;
	struct rcu_head		rcu;
};

#define BGID_ARRAY	64

struct io_ring_ctx {
	/* const or read-mostly hot data */
	struct {
		struct percpu_ref	refs;

		struct io_rings		*rings;
		unsigned int		flags;
		enum task_work_notify_mode	notify_method;
		unsigned int		compat: 1;
		unsigned int		drain_next: 1;
		unsigned int		restricted: 1;
		unsigned int		off_timeout_used: 1;
		unsigned int		drain_active: 1;
		unsigned int		drain_disabled: 1;
		unsigned int		has_evfd: 1;
		unsigned int		syscall_iopoll: 1;
	} ____cacheline_aligned_in_smp;

	/* submission data */
	struct {
		struct mutex		uring_lock;

		/*
		 * Ring buffer of indices into array of io_uring_sqe, which is
		 * mmapped by the application using the IORING_OFF_SQES offset.
		 *
		 * This indirection could e.g. be used to assign fixed
		 * io_uring_sqe entries to operations and only submit them to
		 * the queue when needed.
		 *
		 * The kernel modifies neither the indices array nor the entries
		 * array.
		 */
		u32			*sq_array;
		struct io_uring_sqe	*sq_sqes;
		unsigned		cached_sq_head;
		unsigned		sq_entries;
		struct list_head	defer_list;

		/*
		 * Fixed resources fast path, should be accessed only under
		 * uring_lock, and updated through io_uring_register(2)
		 */
		struct io_rsrc_node	*rsrc_node;
		int			rsrc_cached_refs;
		atomic_t		cancel_seq;
		struct io_file_table	file_table;
		unsigned		nr_user_files;
		unsigned		nr_user_bufs;
		struct io_mapped_ubuf	**user_bufs;

		struct io_submit_state	submit_state;

		struct io_buffer_list	*io_bl;
		struct xarray		io_bl_xa;
		struct list_head	io_buffers_cache;

		struct list_head	timeout_list;
		struct list_head	ltimeout_list;
		struct list_head	cq_overflow_list;
		struct list_head	apoll_cache;
		struct xarray		personalities;
		u32			pers_next;
		unsigned		sq_thread_idle;
	} ____cacheline_aligned_in_smp;

	/* IRQ completion list, under ->completion_lock */
	struct io_wq_work_list	locked_free_list;
	unsigned int		locked_free_nr;

	const struct cred	*sq_creds;	/* cred used for __io_sq_thread() */
	struct io_sq_data	*sq_data;	/* if using sq thread polling */

	struct wait_queue_head	sqo_sq_wait;
	struct list_head	sqd_list;

	unsigned long		check_cq;

	struct {
		/*
		 * We cache a range of free CQEs we can use, once exhausted it
		 * should go through a slower range setup, see __io_get_cqe()
		 */
		struct io_uring_cqe	*cqe_cached;
		struct io_uring_cqe	*cqe_sentinel;

		unsigned		cached_cq_tail;
		unsigned		cq_entries;
		struct io_ev_fd	__rcu	*io_ev_fd;
		struct wait_queue_head	cq_wait;
		unsigned		cq_extra;
		atomic_t		cq_timeouts;
		unsigned		cq_last_tm_flush;
	} ____cacheline_aligned_in_smp;

	struct {
		spinlock_t		completion_lock;

		spinlock_t		timeout_lock;

		/*
		 * ->iopoll_list is protected by the ctx->uring_lock for
		 * io_uring instances that don't use IORING_SETUP_SQPOLL.
		 * For SQPOLL, only the single threaded io_sq_thread() will
		 * manipulate the list, hence no extra locking is needed there.
		 */
		struct io_wq_work_list	iopoll_list;
		struct hlist_head	*cancel_hash;
		unsigned		cancel_hash_bits;
		bool			poll_multi_queue;

		struct list_head	io_buffers_comp;
	} ____cacheline_aligned_in_smp;

	struct io_restriction		restrictions;

	/* slow path rsrc auxilary data, used by update/register */
	struct {
		struct io_rsrc_node		*rsrc_backup_node;
		struct io_mapped_ubuf		*dummy_ubuf;
		struct io_rsrc_data		*file_data;
		struct io_rsrc_data		*buf_data;

		struct delayed_work		rsrc_put_work;
		struct llist_head		rsrc_put_llist;
		struct list_head		rsrc_ref_list;
		spinlock_t			rsrc_ref_lock;

		struct list_head	io_buffers_pages;
	};

	/* Keep this last, we don't need it for the fast path */
	struct {
		#if defined(CONFIG_UNIX)
			struct socket		*ring_sock;
		#endif
		/* hashed buffered write serialization */
		struct io_wq_hash		*hash_map;

		/* Only used for accounting purposes */
		struct user_struct		*user;
		struct mm_struct		*mm_account;

		/* ctx exit and cancelation */
		struct llist_head		fallback_llist;
		struct delayed_work		fallback_work;
		struct work_struct		exit_work;
		struct list_head		tctx_list;
		struct completion		ref_comp;
		u32				iowq_limits[2];
		bool				iowq_limits_set;
	};
};

/*
 * Arbitrary limit, can be raised if need be
 */
#define IO_RINGFD_REG_MAX 16

struct io_uring_task {
	/* submission side */
	int			cached_refs;
	struct xarray		xa;
	struct wait_queue_head	wait;
	const struct io_ring_ctx *last;
	struct io_wq		*io_wq;
	struct percpu_counter	inflight;
	atomic_t		inflight_tracked;
	atomic_t		in_idle;

	spinlock_t		task_lock;
	struct io_wq_work_list	task_list;
	struct io_wq_work_list	prio_task_list;
	struct callback_head	task_work;
	struct file		**registered_rings;
	bool			task_running;
};

/*
 * First field must be the file pointer in all the
 * iocb unions! See also 'struct kiocb' in <linux/fs.h>
 */
struct io_poll_iocb {
	struct file			*file;
	struct wait_queue_head		*head;
	__poll_t			events;
	struct wait_queue_entry		wait;
};

struct io_poll_update {
	struct file			*file;
	u64				old_user_data;
	u64				new_user_data;
	__poll_t			events;
	bool				update_events;
	bool				update_user_data;
};

struct io_close {
	struct file			*file;
	int				fd;
	u32				file_slot;
};

struct io_timeout_data {
	struct io_kiocb			*req;
	struct hrtimer			timer;
	struct timespec64		ts;
	enum hrtimer_mode		mode;
	u32				flags;
};

struct io_accept {
	struct file			*file;
	struct sockaddr __user		*addr;
	int __user			*addr_len;
	int				flags;
	u32				file_slot;
	unsigned long			nofile;
};

struct io_socket {
	struct file			*file;
	int				domain;
	int				type;
	int				protocol;
	int				flags;
	u32				file_slot;
	unsigned long			nofile;
};

struct io_sync {
	struct file			*file;
	loff_t				len;
	loff_t				off;
	int				flags;
	int				mode;
};

struct io_cancel {
	struct file			*file;
	u64				addr;
	u32				flags;
	s32				fd;
};

struct io_timeout {
	struct file			*file;
	u32				off;
	u32				target_seq;
	struct list_head		list;
	/* head of the link, used by linked timeouts only */
	struct io_kiocb			*head;
	/* for linked completions */
	struct io_kiocb			*prev;
};

struct io_timeout_rem {
	struct file			*file;
	u64				addr;

	/* timeout update */
	struct timespec64		ts;
	u32				flags;
	bool				ltimeout;
};

struct io_rw {
	/* NOTE: kiocb has the file as the first member, so don't do it here */
	struct kiocb			kiocb;
	u64				addr;
	u32				len;
	rwf_t				flags;
};

struct io_connect {
	struct file			*file;
	struct sockaddr __user		*addr;
	int				addr_len;
};

struct io_sr_msg {
	struct file			*file;
	union {
		struct compat_msghdr __user	*umsg_compat;
		struct user_msghdr __user	*umsg;
		void __user			*buf;
	};
	int				msg_flags;
	size_t				len;
	size_t				done_io;
	unsigned int			flags;
};

struct io_open {
	struct file			*file;
	int				dfd;
	u32				file_slot;
	struct filename			*filename;
	struct open_how			how;
	unsigned long			nofile;
};

struct io_rsrc_update {
	struct file			*file;
	u64				arg;
	u32				nr_args;
	u32				offset;
};

struct io_fadvise {
	struct file			*file;
	u64				offset;
	u32				len;
	u32				advice;
};

struct io_madvise {
	struct file			*file;
	u64				addr;
	u32				len;
	u32				advice;
};

struct io_epoll {
	struct file			*file;
	int				epfd;
	int				op;
	int				fd;
	struct epoll_event		event;
};

struct io_splice {
	struct file			*file_out;
	loff_t				off_out;
	loff_t				off_in;
	u64				len;
	int				splice_fd_in;
	unsigned int			flags;
};

struct io_provide_buf {
	struct file			*file;
	__u64				addr;
	__u32				len;
	__u32				bgid;
	__u16				nbufs;
	__u16				bid;
};

struct io_statx {
	struct file			*file;
	int				dfd;
	unsigned int			mask;
	unsigned int			flags;
	struct filename			*filename;
	struct statx __user		*buffer;
};

struct io_shutdown {
	struct file			*file;
	int				how;
};

struct io_rename {
	struct file			*file;
	int				old_dfd;
	int				new_dfd;
	struct filename			*oldpath;
	struct filename			*newpath;
	int				flags;
};

struct io_unlink {
	struct file			*file;
	int				dfd;
	int				flags;
	struct filename			*filename;
};

struct io_mkdir {
	struct file			*file;
	int				dfd;
	umode_t				mode;
	struct filename			*filename;
};

struct io_symlink {
	struct file			*file;
	int				new_dfd;
	struct filename			*oldpath;
	struct filename			*newpath;
};

struct io_hardlink {
	struct file			*file;
	int				old_dfd;
	int				new_dfd;
	struct filename			*oldpath;
	struct filename			*newpath;
	int				flags;
};

struct io_msg {
	struct file			*file;
	u64 user_data;
	u32 len;
};

struct io_async_connect {
	struct sockaddr_storage		address;
};

struct io_async_msghdr {
	struct iovec			fast_iov[UIO_FASTIOV];
	/* points to an allocated iov, if NULL we use fast_iov instead */
	struct iovec			*free_iov;
	struct sockaddr __user		*uaddr;
	struct msghdr			msg;
	struct sockaddr_storage		addr;
};

struct io_rw_state {
	struct iov_iter			iter;
	struct iov_iter_state		iter_state;
	struct iovec			fast_iov[UIO_FASTIOV];
};

struct io_async_rw {
	struct io_rw_state		s;
	const struct iovec		*free_iovec;
	size_t				bytes_done;
	struct wait_page_queue		wpq;
};

struct io_xattr {
	struct file			*file;
	struct xattr_ctx		ctx;
	struct filename			*filename;
};

enum {
	REQ_F_FIXED_FILE_BIT	= IOSQE_FIXED_FILE_BIT,
	REQ_F_IO_DRAIN_BIT	= IOSQE_IO_DRAIN_BIT,
	REQ_F_LINK_BIT		= IOSQE_IO_LINK_BIT,
	REQ_F_HARDLINK_BIT	= IOSQE_IO_HARDLINK_BIT,
	REQ_F_FORCE_ASYNC_BIT	= IOSQE_ASYNC_BIT,
	REQ_F_BUFFER_SELECT_BIT	= IOSQE_BUFFER_SELECT_BIT,
	REQ_F_CQE_SKIP_BIT	= IOSQE_CQE_SKIP_SUCCESS_BIT,

	/* first byte is taken by user flags, shift it to not overlap */
	REQ_F_FAIL_BIT		= 8,
	REQ_F_INFLIGHT_BIT,
	REQ_F_CUR_POS_BIT,
	REQ_F_NOWAIT_BIT,
	REQ_F_LINK_TIMEOUT_BIT,
	REQ_F_NEED_CLEANUP_BIT,
	REQ_F_POLLED_BIT,
	REQ_F_BUFFER_SELECTED_BIT,
	REQ_F_BUFFER_RING_BIT,
	REQ_F_COMPLETE_INLINE_BIT,
	REQ_F_REISSUE_BIT,
	REQ_F_CREDS_BIT,
	REQ_F_REFCOUNT_BIT,
	REQ_F_ARM_LTIMEOUT_BIT,
	REQ_F_ASYNC_DATA_BIT,
	REQ_F_SKIP_LINK_CQES_BIT,
	REQ_F_SINGLE_POLL_BIT,
	REQ_F_DOUBLE_POLL_BIT,
	REQ_F_PARTIAL_IO_BIT,
	REQ_F_CQE32_INIT_BIT,
	REQ_F_APOLL_MULTISHOT_BIT,
	/* keep async read/write and isreg together and in order */
	REQ_F_SUPPORT_NOWAIT_BIT,
	REQ_F_ISREG_BIT,

	/* not a real bit, just to check we're not overflowing the space */
	__REQ_F_LAST_BIT,
};

enum {
	/* ctx owns file */
	REQ_F_FIXED_FILE	= BIT(REQ_F_FIXED_FILE_BIT),
	/* drain existing IO first */
	REQ_F_IO_DRAIN		= BIT(REQ_F_IO_DRAIN_BIT),
	/* linked sqes */
	REQ_F_LINK		= BIT(REQ_F_LINK_BIT),
	/* doesn't sever on completion < 0 */
	REQ_F_HARDLINK		= BIT(REQ_F_HARDLINK_BIT),
	/* IOSQE_ASYNC */
	REQ_F_FORCE_ASYNC	= BIT(REQ_F_FORCE_ASYNC_BIT),
	/* IOSQE_BUFFER_SELECT */
	REQ_F_BUFFER_SELECT	= BIT(REQ_F_BUFFER_SELECT_BIT),
	/* IOSQE_CQE_SKIP_SUCCESS */
	REQ_F_CQE_SKIP		= BIT(REQ_F_CQE_SKIP_BIT),

	/* fail rest of links */
	REQ_F_FAIL		= BIT(REQ_F_FAIL_BIT),
	/* on inflight list, should be cancelled and waited on exit reliably */
	REQ_F_INFLIGHT		= BIT(REQ_F_INFLIGHT_BIT),
	/* read/write uses file position */
	REQ_F_CUR_POS		= BIT(REQ_F_CUR_POS_BIT),
	/* must not punt to workers */
	REQ_F_NOWAIT		= BIT(REQ_F_NOWAIT_BIT),
	/* has or had linked timeout */
	REQ_F_LINK_TIMEOUT	= BIT(REQ_F_LINK_TIMEOUT_BIT),
	/* needs cleanup */
	REQ_F_NEED_CLEANUP	= BIT(REQ_F_NEED_CLEANUP_BIT),
	/* already went through poll handler */
	REQ_F_POLLED		= BIT(REQ_F_POLLED_BIT),
	/* buffer already selected */
	REQ_F_BUFFER_SELECTED	= BIT(REQ_F_BUFFER_SELECTED_BIT),
	/* buffer selected from ring, needs commit */
	REQ_F_BUFFER_RING	= BIT(REQ_F_BUFFER_RING_BIT),
	/* completion is deferred through io_comp_state */
	REQ_F_COMPLETE_INLINE	= BIT(REQ_F_COMPLETE_INLINE_BIT),
	/* caller should reissue async */
	REQ_F_REISSUE		= BIT(REQ_F_REISSUE_BIT),
	/* supports async reads/writes */
	REQ_F_SUPPORT_NOWAIT	= BIT(REQ_F_SUPPORT_NOWAIT_BIT),
	/* regular file */
	REQ_F_ISREG		= BIT(REQ_F_ISREG_BIT),
	/* has creds assigned */
	REQ_F_CREDS		= BIT(REQ_F_CREDS_BIT),
	/* skip refcounting if not set */
	REQ_F_REFCOUNT		= BIT(REQ_F_REFCOUNT_BIT),
	/* there is a linked timeout that has to be armed */
	REQ_F_ARM_LTIMEOUT	= BIT(REQ_F_ARM_LTIMEOUT_BIT),
	/* ->async_data allocated */
	REQ_F_ASYNC_DATA	= BIT(REQ_F_ASYNC_DATA_BIT),
	/* don't post CQEs while failing linked requests */
	REQ_F_SKIP_LINK_CQES	= BIT(REQ_F_SKIP_LINK_CQES_BIT),
	/* single poll may be active */
	REQ_F_SINGLE_POLL	= BIT(REQ_F_SINGLE_POLL_BIT),
	/* double poll may active */
	REQ_F_DOUBLE_POLL	= BIT(REQ_F_DOUBLE_POLL_BIT),
	/* request has already done partial IO */
	REQ_F_PARTIAL_IO	= BIT(REQ_F_PARTIAL_IO_BIT),
	/* fast poll multishot mode */
	REQ_F_APOLL_MULTISHOT	= BIT(REQ_F_APOLL_MULTISHOT_BIT),
	/* ->extra1 and ->extra2 are initialised */
	REQ_F_CQE32_INIT	= BIT(REQ_F_CQE32_INIT_BIT),
};

struct async_poll {
	struct io_poll_iocb	poll;
	struct io_poll_iocb	*double_poll;
};

typedef void (*io_req_tw_func_t)(struct io_kiocb *req, bool *locked);

struct io_task_work {
	union {
		struct io_wq_work_node	node;
		struct llist_node	fallback_node;
	};
	io_req_tw_func_t		func;
};

enum {
	IORING_RSRC_FILE		= 0,
	IORING_RSRC_BUFFER		= 1,
};

struct io_cqe {
	__u64	user_data;
	__s32	res;
	/* fd initially, then cflags for completion */
	union {
		__u32	flags;
		int	fd;
	};
};

enum {
	IO_CHECK_CQ_OVERFLOW_BIT,
	IO_CHECK_CQ_DROPPED_BIT,
};

/*
 * NOTE! Each of the iocb union members has the file pointer
 * as the first entry in their struct definition. So you can
 * access the file pointer through any of the sub-structs,
 * or directly as just 'file' in this struct.
 */
struct io_kiocb {
	union {
		struct file		*file;
		struct io_rw		rw;
		struct io_poll_iocb	poll;
		struct io_poll_update	poll_update;
		struct io_accept	accept;
		struct io_sync		sync;
		struct io_cancel	cancel;
		struct io_timeout	timeout;
		struct io_timeout_rem	timeout_rem;
		struct io_connect	connect;
		struct io_sr_msg	sr_msg;
		struct io_open		open;
		struct io_close		close;
		struct io_rsrc_update	rsrc_update;
		struct io_fadvise	fadvise;
		struct io_madvise	madvise;
		struct io_epoll		epoll;
		struct io_splice	splice;
		struct io_provide_buf	pbuf;
		struct io_statx		statx;
		struct io_shutdown	shutdown;
		struct io_rename	rename;
		struct io_unlink	unlink;
		struct io_mkdir		mkdir;
		struct io_symlink	symlink;
		struct io_hardlink	hardlink;
		struct io_msg		msg;
		struct io_xattr		xattr;
		struct io_socket	sock;
		struct io_uring_cmd	uring_cmd;
	};

	u8				opcode;
	/* polled IO has completed */
	u8				iopoll_completed;
	/*
	 * Can be either a fixed buffer index, or used with provided buffers.
	 * For the latter, before issue it points to the buffer group ID,
	 * and after selection it points to the buffer ID itself.
	 */
	u16				buf_index;
	unsigned int			flags;

	struct io_cqe			cqe;

	struct io_ring_ctx		*ctx;
	struct task_struct		*task;

	struct io_rsrc_node		*rsrc_node;

	union {
		/* store used ubuf, so we can prevent reloading */
		struct io_mapped_ubuf	*imu;

		/* stores selected buf, valid IFF REQ_F_BUFFER_SELECTED is set */
		struct io_buffer	*kbuf;

		/*
		 * stores buffer ID for ring provided buffers, valid IFF
		 * REQ_F_BUFFER_RING is set.
		 */
		struct io_buffer_list	*buf_list;
	};

	union {
		/* used by request caches, completion batching and iopoll */
		struct io_wq_work_node	comp_list;
		/* cache ->apoll->events */
		__poll_t apoll_events;
	};
	atomic_t			refs;
	atomic_t			poll_refs;
	struct io_task_work		io_task_work;
	/* for polled requests, i.e. IORING_OP_POLL_ADD and async armed poll */
	union {
		struct hlist_node	hash_node;
		struct {
			u64		extra1;
			u64		extra2;
		};
	};
	/* internal polling, see IORING_FEAT_FAST_POLL */
	struct async_poll		*apoll;
	/* opcode allocated if it needs to store data for async defer */
	void				*async_data;
	/* linked requests, IFF REQ_F_HARDLINK or REQ_F_LINK are set */
	struct io_kiocb			*link;
	/* custom credentials, valid IFF REQ_F_CREDS is set */
	const struct cred		*creds;
	struct io_wq_work		work;
};

struct io_tctx_node {
	struct list_head	ctx_node;
	struct task_struct	*task;
	struct io_ring_ctx	*ctx;
};

struct io_defer_entry {
	struct list_head	list;
	struct io_kiocb		*req;
	u32			seq;
};

struct io_cancel_data {
	struct io_ring_ctx *ctx;
	union {
		u64 data;
		struct file *file;
	};
	u32 flags;
	int seq;
};

/*
 * The URING_CMD payload starts at 'cmd' in the first sqe, and continues into
 * the following sqe if SQE128 is used.
 */
#define uring_cmd_pdu_size(is_sqe128)				\
	((1 + !!(is_sqe128)) * sizeof(struct io_uring_sqe) -	\
		offsetof(struct io_uring_sqe, cmd))

struct io_op_def {
	/* needs req->file assigned */
	unsigned		needs_file : 1;
	/* should block plug */
	unsigned		plug : 1;
	/* hash wq insertion if file is a regular file */
	unsigned		hash_reg_file : 1;
	/* unbound wq insertion if file is a non-regular file */
	unsigned		unbound_nonreg_file : 1;
	/* set if opcode supports polled "wait" */
	unsigned		pollin : 1;
	unsigned		pollout : 1;
	unsigned		poll_exclusive : 1;
	/* op supports buffer selection */
	unsigned		buffer_select : 1;
	/* do prep async if is going to be punted */
	unsigned		needs_async_setup : 1;
	/* opcode is not supported by this kernel */
	unsigned		not_supported : 1;
	/* skip auditing */
	unsigned		audit_skip : 1;
	/* supports ioprio */
	unsigned		ioprio : 1;
	/* supports iopoll */
	unsigned		iopoll : 1;
	/* size of async data needed, if any */
	unsigned short		async_size;
};

static const struct io_op_def io_op_defs[] = {
	[IORING_OP_NOP] = {
		.audit_skip		= 1,
		.iopoll			= 1,
	},
	[IORING_OP_READV] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollin			= 1,
		.buffer_select		= 1,
		.needs_async_setup	= 1,
		.plug			= 1,
		.audit_skip		= 1,
		.ioprio			= 1,
		.iopoll			= 1,
		.async_size		= sizeof(struct io_async_rw),
	},
	[IORING_OP_WRITEV] = {
		.needs_file		= 1,
		.hash_reg_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollout		= 1,
		.needs_async_setup	= 1,
		.plug			= 1,
		.audit_skip		= 1,
		.ioprio			= 1,
		.iopoll			= 1,
		.async_size		= sizeof(struct io_async_rw),
	},
	[IORING_OP_FSYNC] = {
		.needs_file		= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_READ_FIXED] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollin			= 1,
		.plug			= 1,
		.audit_skip		= 1,
		.ioprio			= 1,
		.iopoll			= 1,
		.async_size		= sizeof(struct io_async_rw),
	},
	[IORING_OP_WRITE_FIXED] = {
		.needs_file		= 1,
		.hash_reg_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollout		= 1,
		.plug			= 1,
		.audit_skip		= 1,
		.ioprio			= 1,
		.iopoll			= 1,
		.async_size		= sizeof(struct io_async_rw),
	},
	[IORING_OP_POLL_ADD] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_POLL_REMOVE] = {
		.audit_skip		= 1,
	},
	[IORING_OP_SYNC_FILE_RANGE] = {
		.needs_file		= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_SENDMSG] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollout		= 1,
		.needs_async_setup	= 1,
		.async_size		= sizeof(struct io_async_msghdr),
	},
	[IORING_OP_RECVMSG] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollin			= 1,
		.buffer_select		= 1,
		.needs_async_setup	= 1,
		.async_size		= sizeof(struct io_async_msghdr),
	},
	[IORING_OP_TIMEOUT] = {
		.audit_skip		= 1,
		.async_size		= sizeof(struct io_timeout_data),
	},
	[IORING_OP_TIMEOUT_REMOVE] = {
		/* used by timeout updates' prep() */
		.audit_skip		= 1,
	},
	[IORING_OP_ACCEPT] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollin			= 1,
		.poll_exclusive		= 1,
		.ioprio			= 1,	/* used for flags */
	},
	[IORING_OP_ASYNC_CANCEL] = {
		.audit_skip		= 1,
	},
	[IORING_OP_LINK_TIMEOUT] = {
		.audit_skip		= 1,
		.async_size		= sizeof(struct io_timeout_data),
	},
	[IORING_OP_CONNECT] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollout		= 1,
		.needs_async_setup	= 1,
		.async_size		= sizeof(struct io_async_connect),
	},
	[IORING_OP_FALLOCATE] = {
		.needs_file		= 1,
	},
	[IORING_OP_OPENAT] = {},
	[IORING_OP_CLOSE] = {},
	[IORING_OP_FILES_UPDATE] = {
		.audit_skip		= 1,
		.iopoll			= 1,
	},
	[IORING_OP_STATX] = {
		.audit_skip		= 1,
	},
	[IORING_OP_READ] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollin			= 1,
		.buffer_select		= 1,
		.plug			= 1,
		.audit_skip		= 1,
		.ioprio			= 1,
		.iopoll			= 1,
		.async_size		= sizeof(struct io_async_rw),
	},
	[IORING_OP_WRITE] = {
		.needs_file		= 1,
		.hash_reg_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollout		= 1,
		.plug			= 1,
		.audit_skip		= 1,
		.ioprio			= 1,
		.iopoll			= 1,
		.async_size		= sizeof(struct io_async_rw),
	},
	[IORING_OP_FADVISE] = {
		.needs_file		= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_MADVISE] = {},
	[IORING_OP_SEND] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollout		= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_RECV] = {
		.needs_file		= 1,
		.unbound_nonreg_file	= 1,
		.pollin			= 1,
		.buffer_select		= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_OPENAT2] = {
	},
	[IORING_OP_EPOLL_CTL] = {
		.unbound_nonreg_file	= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_SPLICE] = {
		.needs_file		= 1,
		.hash_reg_file		= 1,
		.unbound_nonreg_file	= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_PROVIDE_BUFFERS] = {
		.audit_skip		= 1,
		.iopoll			= 1,
	},
	[IORING_OP_REMOVE_BUFFERS] = {
		.audit_skip		= 1,
		.iopoll			= 1,
	},
	[IORING_OP_TEE] = {
		.needs_file		= 1,
		.hash_reg_file		= 1,
		.unbound_nonreg_file	= 1,
		.audit_skip		= 1,
	},
	[IORING_OP_SHUTDOWN] = {
		.needs_file		= 1,
	},
	[IORING_OP_RENAMEAT] = {},
	[IORING_OP_UNLINKAT] = {},
	[IORING_OP_MKDIRAT] = {},
	[IORING_OP_SYMLINKAT] = {},
	[IORING_OP_LINKAT] = {},
	[IORING_OP_MSG_RING] = {
		.needs_file		= 1,
		.iopoll			= 1,
	},
	[IORING_OP_FSETXATTR] = {
		.needs_file = 1
	},
	[IORING_OP_SETXATTR] = {},
	[IORING_OP_FGETXATTR] = {
		.needs_file = 1
	},
	[IORING_OP_GETXATTR] = {},
	[IORING_OP_SOCKET] = {
		.audit_skip		= 1,
	},
	[IORING_OP_URING_CMD] = {
		.needs_file		= 1,
		.plug			= 1,
		.needs_async_setup	= 1,
		.async_size		= uring_cmd_pdu_size(1),
	},
};

/* requests with any of those set should undergo io_disarm_next() */
#define IO_DISARM_MASK (REQ_F_ARM_LTIMEOUT | REQ_F_LINK_TIMEOUT | REQ_F_FAIL)
#define IO_REQ_LINK_FLAGS (REQ_F_LINK | REQ_F_HARDLINK)

static bool io_disarm_next(struct io_kiocb *req);
static void io_uring_del_tctx_node(unsigned long index);
static void io_uring_try_cancel_requests(struct io_ring_ctx *ctx,
					 struct task_struct *task,
					 bool cancel_all);
static void io_uring_cancel_generic(bool cancel_all, struct io_sq_data *sqd);

static void __io_req_complete_post(struct io_kiocb *req, s32 res, u32 cflags);
static void io_dismantle_req(struct io_kiocb *req);
static void io_queue_linked_timeout(struct io_kiocb *req);
static int __io_register_rsrc_update(struct io_ring_ctx *ctx, unsigned type,
				     struct io_uring_rsrc_update2 *up,
				     unsigned nr_args);
static void io_clean_op(struct io_kiocb *req);
static inline struct file *io_file_get_fixed(struct io_kiocb *req, int fd,
					     unsigned issue_flags);
static struct file *io_file_get_normal(struct io_kiocb *req, int fd);
static void io_queue_sqe(struct io_kiocb *req);
static void io_rsrc_put_work(struct work_struct *work);

static void io_req_task_queue(struct io_kiocb *req);
static void __io_submit_flush_completions(struct io_ring_ctx *ctx);
static int io_req_prep_async(struct io_kiocb *req);

static int io_install_fixed_file(struct io_kiocb *req, struct file *file,
				 unsigned int issue_flags, u32 slot_index);
static int __io_close_fixed(struct io_kiocb *req, unsigned int issue_flags,
			    unsigned int offset);
static inline int io_close_fixed(struct io_kiocb *req, unsigned int issue_flags);

static enum hrtimer_restart io_link_timeout_fn(struct hrtimer *timer);
static void io_eventfd_signal(struct io_ring_ctx *ctx);
static void io_req_tw_post_queue(struct io_kiocb *req, s32 res, u32 cflags);

static struct kmem_cache *req_cachep;

static const struct file_operations io_uring_fops;

const char *io_uring_get_opcode(u8 opcode)
{
	switch ((enum io_uring_op)opcode) {
	case IORING_OP_NOP:
		return "NOP";
	case IORING_OP_READV:
		return "READV";
	case IORING_OP_WRITEV:
		return "WRITEV";
	case IORING_OP_FSYNC:
		return "FSYNC";
	case IORING_OP_READ_FIXED:
		return "READ_FIXED";
	case IORING_OP_WRITE_FIXED:
		return "WRITE_FIXED";
	case IORING_OP_POLL_ADD:
		return "POLL_ADD";
	case IORING_OP_POLL_REMOVE:
		return "POLL_REMOVE";
	case IORING_OP_SYNC_FILE_RANGE:
		return "SYNC_FILE_RANGE";
	case IORING_OP_SENDMSG:
		return "SENDMSG";
	case IORING_OP_RECVMSG:
		return "RECVMSG";
	case IORING_OP_TIMEOUT:
		return "TIMEOUT";
	case IORING_OP_TIMEOUT_REMOVE:
		return "TIMEOUT_REMOVE";
	case IORING_OP_ACCEPT:
		return "ACCEPT";
	case IORING_OP_ASYNC_CANCEL:
		return "ASYNC_CANCEL";
	case IORING_OP_LINK_TIMEOUT:
		return "LINK_TIMEOUT";
	case IORING_OP_CONNECT:
		return "CONNECT";
	case IORING_OP_FALLOCATE:
		return "FALLOCATE";
	case IORING_OP_OPENAT:
		return "OPENAT";
	case IORING_OP_CLOSE:
		return "CLOSE";
	case IORING_OP_FILES_UPDATE:
		return "FILES_UPDATE";
	case IORING_OP_STATX:
		return "STATX";
	case IORING_OP_READ:
		return "READ";
	case IORING_OP_WRITE:
		return "WRITE";
	case IORING_OP_FADVISE:
		return "FADVISE";
	case IORING_OP_MADVISE:
		return "MADVISE";
	case IORING_OP_SEND:
		return "SEND";
	case IORING_OP_RECV:
		return "RECV";
	case IORING_OP_OPENAT2:
		return "OPENAT2";
	case IORING_OP_EPOLL_CTL:
		return "EPOLL_CTL";
	case IORING_OP_SPLICE:
		return "SPLICE";
	case IORING_OP_PROVIDE_BUFFERS:
		return "PROVIDE_BUFFERS";
	case IORING_OP_REMOVE_BUFFERS:
		return "REMOVE_BUFFERS";
	case IORING_OP_TEE:
		return "TEE";
	case IORING_OP_SHUTDOWN:
		return "SHUTDOWN";
	case IORING_OP_RENAMEAT:
		return "RENAMEAT";
	case IORING_OP_UNLINKAT:
		return "UNLINKAT";
	case IORING_OP_MKDIRAT:
		return "MKDIRAT";
	case IORING_OP_SYMLINKAT:
		return "SYMLINKAT";
	case IORING_OP_LINKAT:
		return "LINKAT";
	case IORING_OP_MSG_RING:
		return "MSG_RING";
	case IORING_OP_FSETXATTR:
		return "FSETXATTR";
	case IORING_OP_SETXATTR:
		return "SETXATTR";
	case IORING_OP_FGETXATTR:
		return "FGETXATTR";
	case IORING_OP_GETXATTR:
		return "GETXATTR";
	case IORING_OP_SOCKET:
		return "SOCKET";
	case IORING_OP_URING_CMD:
		return "URING_CMD";
	case IORING_OP_LAST:
		return "INVALID";
	}
	return "INVALID";
}

struct sock *io_uring_get_socket(struct file *file)
{
#if defined(CONFIG_UNIX)
	if (file->f_op == &io_uring_fops) {
		struct io_ring_ctx *ctx = file->private_data;

		return ctx->ring_sock->sk;
	}
#endif
	return NULL;
}
EXPORT_SYMBOL(io_uring_get_socket);

#if defined(CONFIG_UNIX)
static inline bool io_file_need_scm(struct file *filp)
{
#if defined(IO_URING_SCM_ALL)
	return true;
#else
	return !!unix_get_socket(filp);
#endif
}
#else
static inline bool io_file_need_scm(struct file *filp)
{
	return false;
}
#endif

static void io_ring_submit_unlock(struct io_ring_ctx *ctx, unsigned issue_flags)
{
	lockdep_assert_held(&ctx->uring_lock);
	if (issue_flags & IO_URING_F_UNLOCKED)
		mutex_unlock(&ctx->uring_lock);
}

static void io_ring_submit_lock(struct io_ring_ctx *ctx, unsigned issue_flags)
{
	/*
	 * "Normal" inline submissions always hold the uring_lock, since we
	 * grab it from the system call. Same is true for the SQPOLL offload.
	 * The only exception is when we've detached the request and issue it
	 * from an async worker thread, grab the lock for that case.
	 */
	if (issue_flags & IO_URING_F_UNLOCKED)
		mutex_lock(&ctx->uring_lock);
	lockdep_assert_held(&ctx->uring_lock);
}

static inline void io_tw_lock(struct io_ring_ctx *ctx, bool *locked)
{
	if (!*locked) {
		mutex_lock(&ctx->uring_lock);
		*locked = true;
	}
}

#define io_for_each_link(pos, head) \
	for (pos = (head); pos; pos = pos->link)

/*
 * Shamelessly stolen from the mm implementation of page reference checking,
 * see commit f958d7b528b1 for details.
 */
#define req_ref_zero_or_close_to_overflow(req)	\
	((unsigned int) atomic_read(&(req->refs)) + 127u <= 127u)

static inline bool req_ref_inc_not_zero(struct io_kiocb *req)
{
	WARN_ON_ONCE(!(req->flags & REQ_F_REFCOUNT));
	return atomic_inc_not_zero(&req->refs);
}

static inline bool req_ref_put_and_test(struct io_kiocb *req)
{
	if (likely(!(req->flags & REQ_F_REFCOUNT)))
		return true;

	WARN_ON_ONCE(req_ref_zero_or_close_to_overflow(req));
	return atomic_dec_and_test(&req->refs);
}

static inline void req_ref_get(struct io_kiocb *req)
{
	WARN_ON_ONCE(!(req->flags & REQ_F_REFCOUNT));
	WARN_ON_ONCE(req_ref_zero_or_close_to_overflow(req));
	atomic_inc(&req->refs);
}

static inline void io_submit_flush_completions(struct io_ring_ctx *ctx)
{
	if (!wq_list_empty(&ctx->submit_state.compl_reqs))
		__io_submit_flush_completions(ctx);
}

static inline void __io_req_set_refcount(struct io_kiocb *req, int nr)
{
	if (!(req->flags & REQ_F_REFCOUNT)) {
		req->flags |= REQ_F_REFCOUNT;
		atomic_set(&req->refs, nr);
	}
}

static inline void io_req_set_refcount(struct io_kiocb *req)
{
	__io_req_set_refcount(req, 1);
}

#define IO_RSRC_REF_BATCH	100

static void io_rsrc_put_node(struct io_rsrc_node *node, int nr)
{
	percpu_ref_put_many(&node->refs, nr);
}

static inline void io_req_put_rsrc_locked(struct io_kiocb *req,
					  struct io_ring_ctx *ctx)
	__must_hold(&ctx->uring_lock)
{
	struct io_rsrc_node *node = req->rsrc_node;

	if (node) {
		if (node == ctx->rsrc_node)
			ctx->rsrc_cached_refs++;
		else
			io_rsrc_put_node(node, 1);
	}
}

static inline void io_req_put_rsrc(struct io_kiocb *req)
{
	if (req->rsrc_node)
		io_rsrc_put_node(req->rsrc_node, 1);
}

static __cold void io_rsrc_refs_drop(struct io_ring_ctx *ctx)
	__must_hold(&ctx->uring_lock)
{
	if (ctx->rsrc_cached_refs) {
		io_rsrc_put_node(ctx->rsrc_node, ctx->rsrc_cached_refs);
		ctx->rsrc_cached_refs = 0;
	}
}

static void io_rsrc_refs_refill(struct io_ring_ctx *ctx)
	__must_hold(&ctx->uring_lock)
{
	ctx->rsrc_cached_refs += IO_RSRC_REF_BATCH;
	percpu_ref_get_many(&ctx->rsrc_node->refs, IO_RSRC_REF_BATCH);
}

static inline void io_req_set_rsrc_node(struct io_kiocb *req,
					struct io_ring_ctx *ctx,
					unsigned int issue_flags)
{
	if (!req->rsrc_node) {
		req->rsrc_node = ctx->rsrc_node;

		if (!(issue_flags & IO_URING_F_UNLOCKED)) {
			lockdep_assert_held(&ctx->uring_lock);
			ctx->rsrc_cached_refs--;
			if (unlikely(ctx->rsrc_cached_refs < 0))
				io_rsrc_refs_refill(ctx);
		} else {
			percpu_ref_get(&req->rsrc_node->refs);
		}
	}
}

static unsigned int __io_put_kbuf(struct io_kiocb *req, struct list_head *list)
{
	if (req->flags & REQ_F_BUFFER_RING) {
		if (req->buf_list)
			req->buf_list->head++;
		req->flags &= ~REQ_F_BUFFER_RING;
	} else {
		list_add(&req->kbuf->list, list);
		req->flags &= ~REQ_F_BUFFER_SELECTED;
	}

	return IORING_CQE_F_BUFFER | (req->buf_index << IORING_CQE_BUFFER_SHIFT);
}

static inline unsigned int io_put_kbuf_comp(struct io_kiocb *req)
{
	lockdep_assert_held(&req->ctx->completion_lock);

	if (!(req->flags & (REQ_F_BUFFER_SELECTED|REQ_F_BUFFER_RING)))
		return 0;
	return __io_put_kbuf(req, &req->ctx->io_buffers_comp);
}

static inline unsigned int io_put_kbuf(struct io_kiocb *req,
				       unsigned issue_flags)
{
	unsigned int cflags;

	if (!(req->flags & (REQ_F_BUFFER_SELECTED|REQ_F_BUFFER_RING)))
		return 0;

	/*
	 * We can add this buffer back to two lists:
	 *
	 * 1) The io_buffers_cache list. This one is protected by the
	 *    ctx->uring_lock. If we already hold this lock, add back to this
	 *    list as we can grab it from issue as well.
	 * 2) The io_buffers_comp list. This one is protected by the
	 *    ctx->completion_lock.
	 *
	 * We migrate buffers from the comp_list to the issue cache list
	 * when we need one.
	 */
	if (req->flags & REQ_F_BUFFER_RING) {
		/* no buffers to recycle for this case */
		cflags = __io_put_kbuf(req, NULL);
	} else if (issue_flags & IO_URING_F_UNLOCKED) {
		struct io_ring_ctx *ctx = req->ctx;

		spin_lock(&ctx->completion_lock);
		cflags = __io_put_kbuf(req, &ctx->io_buffers_comp);
		spin_unlock(&ctx->completion_lock);
	} else {
		lockdep_assert_held(&req->ctx->uring_lock);

		cflags = __io_put_kbuf(req, &req->ctx->io_buffers_cache);
	}

	return cflags;
}

static struct io_buffer_list *io_buffer_get_list(struct io_ring_ctx *ctx,
						 unsigned int bgid)
{
	if (ctx->io_bl && bgid < BGID_ARRAY)
		return &ctx->io_bl[bgid];

	return xa_load(&ctx->io_bl_xa, bgid);
}

static void io_kbuf_recycle(struct io_kiocb *req, unsigned issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	struct io_buffer *buf;

	if (!(req->flags & (REQ_F_BUFFER_SELECTED|REQ_F_BUFFER_RING)))
		return;
	/*
	 * For legacy provided buffer mode, don't recycle if we already did
	 * IO to this buffer. For ring-mapped provided buffer mode, we should
	 * increment ring->head to explicitly monopolize the buffer to avoid
	 * multiple use.
	 */
	if ((req->flags & REQ_F_BUFFER_SELECTED) &&
	    (req->flags & REQ_F_PARTIAL_IO))
		return;

	/*
	 * We don't need to recycle for REQ_F_BUFFER_RING, we can just clear
	 * the flag and hence ensure that bl->head doesn't get incremented.
	 * If the tail has already been incremented, hang on to it.
	 */
	if (req->flags & REQ_F_BUFFER_RING) {
		if (req->buf_list) {
			if (req->flags & REQ_F_PARTIAL_IO) {
				req->buf_list->head++;
				req->buf_list = NULL;
			} else {
				req->buf_index = req->buf_list->bgid;
				req->flags &= ~REQ_F_BUFFER_RING;
			}
		}
		return;
	}

	io_ring_submit_lock(ctx, issue_flags);

	buf = req->kbuf;
	bl = io_buffer_get_list(ctx, buf->bgid);
	list_add(&buf->list, &bl->buf_list);
	req->flags &= ~REQ_F_BUFFER_SELECTED;
	req->buf_index = buf->bgid;

	io_ring_submit_unlock(ctx, issue_flags);
}

static bool io_match_task(struct io_kiocb *head, struct task_struct *task,
			  bool cancel_all)
	__must_hold(&req->ctx->timeout_lock)
{
	struct io_kiocb *req;

	if (task && head->task != task)
		return false;
	if (cancel_all)
		return true;

	io_for_each_link(req, head) {
		if (req->flags & REQ_F_INFLIGHT)
			return true;
	}
	return false;
}

static bool io_match_linked(struct io_kiocb *head)
{
	struct io_kiocb *req;

	io_for_each_link(req, head) {
		if (req->flags & REQ_F_INFLIGHT)
			return true;
	}
	return false;
}

/*
 * As io_match_task() but protected against racing with linked timeouts.
 * User must not hold timeout_lock.
 */
static bool io_match_task_safe(struct io_kiocb *head, struct task_struct *task,
			       bool cancel_all)
{
	bool matched;

	if (task && head->task != task)
		return false;
	if (cancel_all)
		return true;

	if (head->flags & REQ_F_LINK_TIMEOUT) {
		struct io_ring_ctx *ctx = head->ctx;

		/* protect against races with linked timeouts */
		spin_lock_irq(&ctx->timeout_lock);
		matched = io_match_linked(head);
		spin_unlock_irq(&ctx->timeout_lock);
	} else {
		matched = io_match_linked(head);
	}
	return matched;
}

static inline bool req_has_async_data(struct io_kiocb *req)
{
	return req->flags & REQ_F_ASYNC_DATA;
}

static inline void req_set_fail(struct io_kiocb *req)
{
	req->flags |= REQ_F_FAIL;
	if (req->flags & REQ_F_CQE_SKIP) {
		req->flags &= ~REQ_F_CQE_SKIP;
		req->flags |= REQ_F_SKIP_LINK_CQES;
	}
}

static inline void req_fail_link_node(struct io_kiocb *req, int res)
{
	req_set_fail(req);
	req->cqe.res = res;
}

static inline void io_req_add_to_cache(struct io_kiocb *req, struct io_ring_ctx *ctx)
{
	wq_stack_add_head(&req->comp_list, &ctx->submit_state.free_list);
}

static __cold void io_ring_ctx_ref_free(struct percpu_ref *ref)
{
	struct io_ring_ctx *ctx = container_of(ref, struct io_ring_ctx, refs);

	complete(&ctx->ref_comp);
}

static inline bool io_is_timeout_noseq(struct io_kiocb *req)
{
	return !req->timeout.off;
}

static __cold void io_fallback_req_func(struct work_struct *work)
{
	struct io_ring_ctx *ctx = container_of(work, struct io_ring_ctx,
						fallback_work.work);
	struct llist_node *node = llist_del_all(&ctx->fallback_llist);
	struct io_kiocb *req, *tmp;
	bool locked = false;

	percpu_ref_get(&ctx->refs);
	llist_for_each_entry_safe(req, tmp, node, io_task_work.fallback_node)
		req->io_task_work.func(req, &locked);

	if (locked) {
		io_submit_flush_completions(ctx);
		mutex_unlock(&ctx->uring_lock);
	}
	percpu_ref_put(&ctx->refs);
}

static __cold struct io_ring_ctx *io_ring_ctx_alloc(struct io_uring_params *p)
{
	struct io_ring_ctx *ctx;
	int hash_bits;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	xa_init(&ctx->io_bl_xa);

	/*
	 * Use 5 bits less than the max cq entries, that should give us around
	 * 32 entries per hash list if totally full and uniformly spread.
	 */
	hash_bits = ilog2(p->cq_entries);
	hash_bits -= 5;
	if (hash_bits <= 0)
		hash_bits = 1;
	ctx->cancel_hash_bits = hash_bits;
	ctx->cancel_hash = kmalloc((1U << hash_bits) * sizeof(struct hlist_head),
					GFP_KERNEL);
	if (!ctx->cancel_hash)
		goto err;
	__hash_init(ctx->cancel_hash, 1U << hash_bits);

	ctx->dummy_ubuf = kzalloc(sizeof(*ctx->dummy_ubuf), GFP_KERNEL);
	if (!ctx->dummy_ubuf)
		goto err;
	/* set invalid range, so io_import_fixed() fails meeting it */
	ctx->dummy_ubuf->ubuf = -1UL;

	if (percpu_ref_init(&ctx->refs, io_ring_ctx_ref_free,
			    PERCPU_REF_ALLOW_REINIT, GFP_KERNEL))
		goto err;

	ctx->flags = p->flags;
	init_waitqueue_head(&ctx->sqo_sq_wait);
	INIT_LIST_HEAD(&ctx->sqd_list);
	INIT_LIST_HEAD(&ctx->cq_overflow_list);
	INIT_LIST_HEAD(&ctx->io_buffers_cache);
	INIT_LIST_HEAD(&ctx->apoll_cache);
	init_completion(&ctx->ref_comp);
	xa_init_flags(&ctx->personalities, XA_FLAGS_ALLOC1);
	mutex_init(&ctx->uring_lock);
	init_waitqueue_head(&ctx->cq_wait);
	spin_lock_init(&ctx->completion_lock);
	spin_lock_init(&ctx->timeout_lock);
	INIT_WQ_LIST(&ctx->iopoll_list);
	INIT_LIST_HEAD(&ctx->io_buffers_pages);
	INIT_LIST_HEAD(&ctx->io_buffers_comp);
	INIT_LIST_HEAD(&ctx->defer_list);
	INIT_LIST_HEAD(&ctx->timeout_list);
	INIT_LIST_HEAD(&ctx->ltimeout_list);
	spin_lock_init(&ctx->rsrc_ref_lock);
	INIT_LIST_HEAD(&ctx->rsrc_ref_list);
	INIT_DELAYED_WORK(&ctx->rsrc_put_work, io_rsrc_put_work);
	init_llist_head(&ctx->rsrc_put_llist);
	INIT_LIST_HEAD(&ctx->tctx_list);
	ctx->submit_state.free_list.next = NULL;
	INIT_WQ_LIST(&ctx->locked_free_list);
	INIT_DELAYED_WORK(&ctx->fallback_work, io_fallback_req_func);
	INIT_WQ_LIST(&ctx->submit_state.compl_reqs);
	return ctx;
err:
	kfree(ctx->dummy_ubuf);
	kfree(ctx->cancel_hash);
	kfree(ctx->io_bl);
	xa_destroy(&ctx->io_bl_xa);
	kfree(ctx);
	return NULL;
}

static void io_account_cq_overflow(struct io_ring_ctx *ctx)
{
	struct io_rings *r = ctx->rings;

	WRITE_ONCE(r->cq_overflow, READ_ONCE(r->cq_overflow) + 1);
	ctx->cq_extra--;
}

static bool req_need_defer(struct io_kiocb *req, u32 seq)
{
	if (unlikely(req->flags & REQ_F_IO_DRAIN)) {
		struct io_ring_ctx *ctx = req->ctx;

		return seq + READ_ONCE(ctx->cq_extra) != ctx->cached_cq_tail;
	}

	return false;
}

static inline bool io_req_ffs_set(struct io_kiocb *req)
{
	return req->flags & REQ_F_FIXED_FILE;
}

static inline void io_req_track_inflight(struct io_kiocb *req)
{
	if (!(req->flags & REQ_F_INFLIGHT)) {
		req->flags |= REQ_F_INFLIGHT;
		atomic_inc(&current->io_uring->inflight_tracked);
	}
}

static struct io_kiocb *__io_prep_linked_timeout(struct io_kiocb *req)
{
	if (WARN_ON_ONCE(!req->link))
		return NULL;

	req->flags &= ~REQ_F_ARM_LTIMEOUT;
	req->flags |= REQ_F_LINK_TIMEOUT;

	/* linked timeouts should have two refs once prep'ed */
	io_req_set_refcount(req);
	__io_req_set_refcount(req->link, 2);
	return req->link;
}

static inline struct io_kiocb *io_prep_linked_timeout(struct io_kiocb *req)
{
	if (likely(!(req->flags & REQ_F_ARM_LTIMEOUT)))
		return NULL;
	return __io_prep_linked_timeout(req);
}

static noinline void __io_arm_ltimeout(struct io_kiocb *req)
{
	io_queue_linked_timeout(__io_prep_linked_timeout(req));
}

static inline void io_arm_ltimeout(struct io_kiocb *req)
{
	if (unlikely(req->flags & REQ_F_ARM_LTIMEOUT))
		__io_arm_ltimeout(req);
}

static void io_prep_async_work(struct io_kiocb *req)
{
	const struct io_op_def *def = &io_op_defs[req->opcode];
	struct io_ring_ctx *ctx = req->ctx;

	if (!(req->flags & REQ_F_CREDS)) {
		req->flags |= REQ_F_CREDS;
		req->creds = get_current_cred();
	}

	req->work.list.next = NULL;
	req->work.flags = 0;
	req->work.cancel_seq = atomic_read(&ctx->cancel_seq);
	if (req->flags & REQ_F_FORCE_ASYNC)
		req->work.flags |= IO_WQ_WORK_CONCURRENT;

	if (req->flags & REQ_F_ISREG) {
		if (def->hash_reg_file || (ctx->flags & IORING_SETUP_IOPOLL))
			io_wq_hash_work(&req->work, file_inode(req->file));
	} else if (!req->file || !S_ISBLK(file_inode(req->file)->i_mode)) {
		if (def->unbound_nonreg_file)
			req->work.flags |= IO_WQ_WORK_UNBOUND;
	}
}

static void io_prep_async_link(struct io_kiocb *req)
{
	struct io_kiocb *cur;

	if (req->flags & REQ_F_LINK_TIMEOUT) {
		struct io_ring_ctx *ctx = req->ctx;

		spin_lock_irq(&ctx->timeout_lock);
		io_for_each_link(cur, req)
			io_prep_async_work(cur);
		spin_unlock_irq(&ctx->timeout_lock);
	} else {
		io_for_each_link(cur, req)
			io_prep_async_work(cur);
	}
}

static inline void io_req_add_compl_list(struct io_kiocb *req)
{
	struct io_submit_state *state = &req->ctx->submit_state;

	if (!(req->flags & REQ_F_CQE_SKIP))
		state->flush_cqes = true;
	wq_list_add_tail(&req->comp_list, &state->compl_reqs);
}

static void io_queue_iowq(struct io_kiocb *req, bool *dont_use)
{
	struct io_kiocb *link = io_prep_linked_timeout(req);
	struct io_uring_task *tctx = req->task->io_uring;

	BUG_ON(!tctx);
	BUG_ON(!tctx->io_wq);

	/* init ->work of the whole link before punting */
	io_prep_async_link(req);

	/*
	 * Not expected to happen, but if we do have a bug where this _can_
	 * happen, catch it here and ensure the request is marked as
	 * canceled. That will make io-wq go through the usual work cancel
	 * procedure rather than attempt to run this request (or create a new
	 * worker for it).
	 */
	if (WARN_ON_ONCE(!same_thread_group(req->task, current)))
		req->work.flags |= IO_WQ_WORK_CANCEL;

	trace_io_uring_queue_async_work(req->ctx, req, req->cqe.user_data,
					req->opcode, req->flags, &req->work,
					io_wq_is_hashed(&req->work));
	io_wq_enqueue(tctx->io_wq, &req->work);
	if (link)
		io_queue_linked_timeout(link);
}

static void io_kill_timeout(struct io_kiocb *req, int status)
	__must_hold(&req->ctx->completion_lock)
	__must_hold(&req->ctx->timeout_lock)
{
	struct io_timeout_data *io = req->async_data;

	if (hrtimer_try_to_cancel(&io->timer) != -1) {
		if (status)
			req_set_fail(req);
		atomic_set(&req->ctx->cq_timeouts,
			atomic_read(&req->ctx->cq_timeouts) + 1);
		list_del_init(&req->timeout.list);
		io_req_tw_post_queue(req, status, 0);
	}
}

static __cold void io_queue_deferred(struct io_ring_ctx *ctx)
{
	while (!list_empty(&ctx->defer_list)) {
		struct io_defer_entry *de = list_first_entry(&ctx->defer_list,
						struct io_defer_entry, list);

		if (req_need_defer(de->req, de->seq))
			break;
		list_del_init(&de->list);
		io_req_task_queue(de->req);
		kfree(de);
	}
}

static __cold void io_flush_timeouts(struct io_ring_ctx *ctx)
	__must_hold(&ctx->completion_lock)
{
	u32 seq = ctx->cached_cq_tail - atomic_read(&ctx->cq_timeouts);
	struct io_kiocb *req, *tmp;

	spin_lock_irq(&ctx->timeout_lock);
	list_for_each_entry_safe(req, tmp, &ctx->timeout_list, timeout.list) {
		u32 events_needed, events_got;

		if (io_is_timeout_noseq(req))
			break;

		/*
		 * Since seq can easily wrap around over time, subtract
		 * the last seq at which timeouts were flushed before comparing.
		 * Assuming not more than 2^31-1 events have happened since,
		 * these subtractions won't have wrapped, so we can check if
		 * target is in [last_seq, current_seq] by comparing the two.
		 */
		events_needed = req->timeout.target_seq - ctx->cq_last_tm_flush;
		events_got = seq - ctx->cq_last_tm_flush;
		if (events_got < events_needed)
			break;

		io_kill_timeout(req, 0);
	}
	ctx->cq_last_tm_flush = seq;
	spin_unlock_irq(&ctx->timeout_lock);
}

static inline void io_commit_cqring(struct io_ring_ctx *ctx)
{
	/* order cqe stores with ring update */
	smp_store_release(&ctx->rings->cq.tail, ctx->cached_cq_tail);
}

static void __io_commit_cqring_flush(struct io_ring_ctx *ctx)
{
	if (ctx->off_timeout_used || ctx->drain_active) {
		spin_lock(&ctx->completion_lock);
		if (ctx->off_timeout_used)
			io_flush_timeouts(ctx);
		if (ctx->drain_active)
			io_queue_deferred(ctx);
		io_commit_cqring(ctx);
		spin_unlock(&ctx->completion_lock);
	}
	if (ctx->has_evfd)
		io_eventfd_signal(ctx);
}

static inline bool io_sqring_full(struct io_ring_ctx *ctx)
{
	struct io_rings *r = ctx->rings;

	return READ_ONCE(r->sq.tail) - ctx->cached_sq_head == ctx->sq_entries;
}

static inline unsigned int __io_cqring_events(struct io_ring_ctx *ctx)
{
	return ctx->cached_cq_tail - READ_ONCE(ctx->rings->cq.head);
}

/*
 * writes to the cq entry need to come after reading head; the
 * control dependency is enough as we're using WRITE_ONCE to
 * fill the cq entry
 */
static noinline struct io_uring_cqe *__io_get_cqe(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;
	unsigned int off = ctx->cached_cq_tail & (ctx->cq_entries - 1);
	unsigned int shift = 0;
	unsigned int free, queued, len;

	if (ctx->flags & IORING_SETUP_CQE32)
		shift = 1;

	/* userspace may cheat modifying the tail, be safe and do min */
	queued = min(__io_cqring_events(ctx), ctx->cq_entries);
	free = ctx->cq_entries - queued;
	/* we need a contiguous range, limit based on the current array offset */
	len = min(free, ctx->cq_entries - off);
	if (!len)
		return NULL;

	ctx->cached_cq_tail++;
	ctx->cqe_cached = &rings->cqes[off];
	ctx->cqe_sentinel = ctx->cqe_cached + len;
	ctx->cqe_cached++;
	return &rings->cqes[off << shift];
}

static inline struct io_uring_cqe *io_get_cqe(struct io_ring_ctx *ctx)
{
	if (likely(ctx->cqe_cached < ctx->cqe_sentinel)) {
		struct io_uring_cqe *cqe = ctx->cqe_cached;

		if (ctx->flags & IORING_SETUP_CQE32) {
			unsigned int off = ctx->cqe_cached - ctx->rings->cqes;

			cqe += off;
		}

		ctx->cached_cq_tail++;
		ctx->cqe_cached++;
		return cqe;
	}

	return __io_get_cqe(ctx);
}

static void io_eventfd_signal(struct io_ring_ctx *ctx)
{
	struct io_ev_fd *ev_fd;

	rcu_read_lock();
	/*
	 * rcu_dereference ctx->io_ev_fd once and use it for both for checking
	 * and eventfd_signal
	 */
	ev_fd = rcu_dereference(ctx->io_ev_fd);

	/*
	 * Check again if ev_fd exists incase an io_eventfd_unregister call
	 * completed between the NULL check of ctx->io_ev_fd at the start of
	 * the function and rcu_read_lock.
	 */
	if (unlikely(!ev_fd))
		goto out;
	if (READ_ONCE(ctx->rings->cq_flags) & IORING_CQ_EVENTFD_DISABLED)
		goto out;

	if (!ev_fd->eventfd_async || io_wq_current_is_worker())
		eventfd_signal(ev_fd->cq_ev_fd, 1);
out:
	rcu_read_unlock();
}

static inline void io_cqring_wake(struct io_ring_ctx *ctx)
{
	/*
	 * wake_up_all() may seem excessive, but io_wake_function() and
	 * io_should_wake() handle the termination of the loop and only
	 * wake as many waiters as we need to.
	 */
	if (wq_has_sleeper(&ctx->cq_wait))
		wake_up_all(&ctx->cq_wait);
}

/*
 * This should only get called when at least one event has been posted.
 * Some applications rely on the eventfd notification count only changing
 * IFF a new CQE has been added to the CQ ring. There's no depedency on
 * 1:1 relationship between how many times this function is called (and
 * hence the eventfd count) and number of CQEs posted to the CQ ring.
 */
static inline void io_cqring_ev_posted(struct io_ring_ctx *ctx)
{
	if (unlikely(ctx->off_timeout_used || ctx->drain_active ||
		     ctx->has_evfd))
		__io_commit_cqring_flush(ctx);

	io_cqring_wake(ctx);
}

static void io_cqring_ev_posted_iopoll(struct io_ring_ctx *ctx)
{
	if (unlikely(ctx->off_timeout_used || ctx->drain_active ||
		     ctx->has_evfd))
		__io_commit_cqring_flush(ctx);

	if (ctx->flags & IORING_SETUP_SQPOLL)
		io_cqring_wake(ctx);
}

/* Returns true if there are no backlogged entries after the flush */
static bool __io_cqring_overflow_flush(struct io_ring_ctx *ctx, bool force)
{
	bool all_flushed, posted;
	size_t cqe_size = sizeof(struct io_uring_cqe);

	if (!force && __io_cqring_events(ctx) == ctx->cq_entries)
		return false;

	if (ctx->flags & IORING_SETUP_CQE32)
		cqe_size <<= 1;

	posted = false;
	spin_lock(&ctx->completion_lock);
	while (!list_empty(&ctx->cq_overflow_list)) {
		struct io_uring_cqe *cqe = io_get_cqe(ctx);
		struct io_overflow_cqe *ocqe;

		if (!cqe && !force)
			break;
		ocqe = list_first_entry(&ctx->cq_overflow_list,
					struct io_overflow_cqe, list);
		if (cqe)
			memcpy(cqe, &ocqe->cqe, cqe_size);
		else
			io_account_cq_overflow(ctx);

		posted = true;
		list_del(&ocqe->list);
		kfree(ocqe);
	}

	all_flushed = list_empty(&ctx->cq_overflow_list);
	if (all_flushed) {
		clear_bit(IO_CHECK_CQ_OVERFLOW_BIT, &ctx->check_cq);
		atomic_andnot(IORING_SQ_CQ_OVERFLOW, &ctx->rings->sq_flags);
	}

	io_commit_cqring(ctx);
	spin_unlock(&ctx->completion_lock);
	if (posted)
		io_cqring_ev_posted(ctx);
	return all_flushed;
}

static bool io_cqring_overflow_flush(struct io_ring_ctx *ctx)
{
	bool ret = true;

	if (test_bit(IO_CHECK_CQ_OVERFLOW_BIT, &ctx->check_cq)) {
		/* iopoll syncs against uring_lock, not completion_lock */
		if (ctx->flags & IORING_SETUP_IOPOLL)
			mutex_lock(&ctx->uring_lock);
		ret = __io_cqring_overflow_flush(ctx, false);
		if (ctx->flags & IORING_SETUP_IOPOLL)
			mutex_unlock(&ctx->uring_lock);
	}

	return ret;
}

static void __io_put_task(struct task_struct *task, int nr)
{
	struct io_uring_task *tctx = task->io_uring;

	percpu_counter_sub(&tctx->inflight, nr);
	if (unlikely(atomic_read(&tctx->in_idle)))
		wake_up(&tctx->wait);
	put_task_struct_many(task, nr);
}

/* must to be called somewhat shortly after putting a request */
static inline void io_put_task(struct task_struct *task, int nr)
{
	if (likely(task == current))
		task->io_uring->cached_refs += nr;
	else
		__io_put_task(task, nr);
}

static void io_task_refs_refill(struct io_uring_task *tctx)
{
	unsigned int refill = -tctx->cached_refs + IO_TCTX_REFS_CACHE_NR;

	percpu_counter_add(&tctx->inflight, refill);
	refcount_add(refill, &current->usage);
	tctx->cached_refs += refill;
}

static inline void io_get_task_refs(int nr)
{
	struct io_uring_task *tctx = current->io_uring;

	tctx->cached_refs -= nr;
	if (unlikely(tctx->cached_refs < 0))
		io_task_refs_refill(tctx);
}

static __cold void io_uring_drop_tctx_refs(struct task_struct *task)
{
	struct io_uring_task *tctx = task->io_uring;
	unsigned int refs = tctx->cached_refs;

	if (refs) {
		tctx->cached_refs = 0;
		percpu_counter_sub(&tctx->inflight, refs);
		put_task_struct_many(task, refs);
	}
}

static bool io_cqring_event_overflow(struct io_ring_ctx *ctx, u64 user_data,
				     s32 res, u32 cflags, u64 extra1,
				     u64 extra2)
{
	struct io_overflow_cqe *ocqe;
	size_t ocq_size = sizeof(struct io_overflow_cqe);
	bool is_cqe32 = (ctx->flags & IORING_SETUP_CQE32);

	if (is_cqe32)
		ocq_size += sizeof(struct io_uring_cqe);

	ocqe = kmalloc(ocq_size, GFP_ATOMIC | __GFP_ACCOUNT);
	trace_io_uring_cqe_overflow(ctx, user_data, res, cflags, ocqe);
	if (!ocqe) {
		/*
		 * If we're in ring overflow flush mode, or in task cancel mode,
		 * or cannot allocate an overflow entry, then we need to drop it
		 * on the floor.
		 */
		io_account_cq_overflow(ctx);
		set_bit(IO_CHECK_CQ_DROPPED_BIT, &ctx->check_cq);
		return false;
	}
	if (list_empty(&ctx->cq_overflow_list)) {
		set_bit(IO_CHECK_CQ_OVERFLOW_BIT, &ctx->check_cq);
		atomic_or(IORING_SQ_CQ_OVERFLOW, &ctx->rings->sq_flags);

	}
	ocqe->cqe.user_data = user_data;
	ocqe->cqe.res = res;
	ocqe->cqe.flags = cflags;
	if (is_cqe32) {
		ocqe->cqe.big_cqe[0] = extra1;
		ocqe->cqe.big_cqe[1] = extra2;
	}
	list_add_tail(&ocqe->list, &ctx->cq_overflow_list);
	return true;
}

static inline bool __io_fill_cqe_req(struct io_ring_ctx *ctx,
				     struct io_kiocb *req)
{
	struct io_uring_cqe *cqe;

	if (!(ctx->flags & IORING_SETUP_CQE32)) {
		trace_io_uring_complete(req->ctx, req, req->cqe.user_data,
					req->cqe.res, req->cqe.flags, 0, 0);

		/*
		 * If we can't get a cq entry, userspace overflowed the
		 * submission (by quite a lot). Increment the overflow count in
		 * the ring.
		 */
		cqe = io_get_cqe(ctx);
		if (likely(cqe)) {
			memcpy(cqe, &req->cqe, sizeof(*cqe));
			return true;
		}

		return io_cqring_event_overflow(ctx, req->cqe.user_data,
						req->cqe.res, req->cqe.flags,
						0, 0);
	} else {
		u64 extra1 = 0, extra2 = 0;

		if (req->flags & REQ_F_CQE32_INIT) {
			extra1 = req->extra1;
			extra2 = req->extra2;
		}

		trace_io_uring_complete(req->ctx, req, req->cqe.user_data,
					req->cqe.res, req->cqe.flags, extra1, extra2);

		/*
		 * If we can't get a cq entry, userspace overflowed the
		 * submission (by quite a lot). Increment the overflow count in
		 * the ring.
		 */
		cqe = io_get_cqe(ctx);
		if (likely(cqe)) {
			memcpy(cqe, &req->cqe, sizeof(struct io_uring_cqe));
			WRITE_ONCE(cqe->big_cqe[0], extra1);
			WRITE_ONCE(cqe->big_cqe[1], extra2);
			return true;
		}

		return io_cqring_event_overflow(ctx, req->cqe.user_data,
				req->cqe.res, req->cqe.flags,
				extra1, extra2);
	}
}

static noinline bool io_fill_cqe_aux(struct io_ring_ctx *ctx, u64 user_data,
				     s32 res, u32 cflags)
{
	struct io_uring_cqe *cqe;

	ctx->cq_extra++;
	trace_io_uring_complete(ctx, NULL, user_data, res, cflags, 0, 0);

	/*
	 * If we can't get a cq entry, userspace overflowed the
	 * submission (by quite a lot). Increment the overflow count in
	 * the ring.
	 */
	cqe = io_get_cqe(ctx);
	if (likely(cqe)) {
		WRITE_ONCE(cqe->user_data, user_data);
		WRITE_ONCE(cqe->res, res);
		WRITE_ONCE(cqe->flags, cflags);

		if (ctx->flags & IORING_SETUP_CQE32) {
			WRITE_ONCE(cqe->big_cqe[0], 0);
			WRITE_ONCE(cqe->big_cqe[1], 0);
		}
		return true;
	}
	return io_cqring_event_overflow(ctx, user_data, res, cflags, 0, 0);
}

static void __io_req_complete_put(struct io_kiocb *req)
{
	/*
	 * If we're the last reference to this request, add to our locked
	 * free_list cache.
	 */
	if (req_ref_put_and_test(req)) {
		struct io_ring_ctx *ctx = req->ctx;

		if (req->flags & IO_REQ_LINK_FLAGS) {
			if (req->flags & IO_DISARM_MASK)
				io_disarm_next(req);
			if (req->link) {
				io_req_task_queue(req->link);
				req->link = NULL;
			}
		}
		io_req_put_rsrc(req);
		/*
		 * Selected buffer deallocation in io_clean_op() assumes that
		 * we don't hold ->completion_lock. Clean them here to avoid
		 * deadlocks.
		 */
		io_put_kbuf_comp(req);
		io_dismantle_req(req);
		io_put_task(req->task, 1);
		wq_list_add_head(&req->comp_list, &ctx->locked_free_list);
		ctx->locked_free_nr++;
	}
}

static void __io_req_complete_post(struct io_kiocb *req, s32 res,
				   u32 cflags)
{
	if (!(req->flags & REQ_F_CQE_SKIP)) {
		req->cqe.res = res;
		req->cqe.flags = cflags;
		__io_fill_cqe_req(req->ctx, req);
	}
	__io_req_complete_put(req);
}

static void io_req_complete_post(struct io_kiocb *req, s32 res, u32 cflags)
{
	struct io_ring_ctx *ctx = req->ctx;

	spin_lock(&ctx->completion_lock);
	__io_req_complete_post(req, res, cflags);
	io_commit_cqring(ctx);
	spin_unlock(&ctx->completion_lock);
	io_cqring_ev_posted(ctx);
}

static inline void io_req_complete_state(struct io_kiocb *req, s32 res,
					 u32 cflags)
{
	req->cqe.res = res;
	req->cqe.flags = cflags;
	req->flags |= REQ_F_COMPLETE_INLINE;
}

static inline void __io_req_complete(struct io_kiocb *req, unsigned issue_flags,
				     s32 res, u32 cflags)
{
	if (issue_flags & IO_URING_F_COMPLETE_DEFER)
		io_req_complete_state(req, res, cflags);
	else
		io_req_complete_post(req, res, cflags);
}

static inline void io_req_complete(struct io_kiocb *req, s32 res)
{
	if (res < 0)
		req_set_fail(req);
	__io_req_complete(req, 0, res, 0);
}

static void io_req_complete_failed(struct io_kiocb *req, s32 res)
{
	req_set_fail(req);
	io_req_complete_post(req, res, io_put_kbuf(req, IO_URING_F_UNLOCKED));
}

/*
 * Don't initialise the fields below on every allocation, but do that in
 * advance and keep them valid across allocations.
 */
static void io_preinit_req(struct io_kiocb *req, struct io_ring_ctx *ctx)
{
	req->ctx = ctx;
	req->link = NULL;
	req->async_data = NULL;
	/* not necessary, but safer to zero */
	req->cqe.res = 0;
}

static void io_flush_cached_locked_reqs(struct io_ring_ctx *ctx,
					struct io_submit_state *state)
{
	spin_lock(&ctx->completion_lock);
	wq_list_splice(&ctx->locked_free_list, &state->free_list);
	ctx->locked_free_nr = 0;
	spin_unlock(&ctx->completion_lock);
}

static inline bool io_req_cache_empty(struct io_ring_ctx *ctx)
{
	return !ctx->submit_state.free_list.next;
}

/*
 * A request might get retired back into the request caches even before opcode
 * handlers and io_issue_sqe() are done with it, e.g. inline completion path.
 * Because of that, io_alloc_req() should be called only under ->uring_lock
 * and with extra caution to not get a request that is still worked on.
 */
static __cold bool __io_alloc_req_refill(struct io_ring_ctx *ctx)
	__must_hold(&ctx->uring_lock)
{
	gfp_t gfp = GFP_KERNEL | __GFP_NOWARN;
	void *reqs[IO_REQ_ALLOC_BATCH];
	int ret, i;

	/*
	 * If we have more than a batch's worth of requests in our IRQ side
	 * locked cache, grab the lock and move them over to our submission
	 * side cache.
	 */
	if (data_race(ctx->locked_free_nr) > IO_COMPL_BATCH) {
		io_flush_cached_locked_reqs(ctx, &ctx->submit_state);
		if (!io_req_cache_empty(ctx))
			return true;
	}

	ret = kmem_cache_alloc_bulk(req_cachep, gfp, ARRAY_SIZE(reqs), reqs);

	/*
	 * Bulk alloc is all-or-nothing. If we fail to get a batch,
	 * retry single alloc to be on the safe side.
	 */
	if (unlikely(ret <= 0)) {
		reqs[0] = kmem_cache_alloc(req_cachep, gfp);
		if (!reqs[0])
			return false;
		ret = 1;
	}

	percpu_ref_get_many(&ctx->refs, ret);
	for (i = 0; i < ret; i++) {
		struct io_kiocb *req = reqs[i];

		io_preinit_req(req, ctx);
		io_req_add_to_cache(req, ctx);
	}
	return true;
}

static inline bool io_alloc_req_refill(struct io_ring_ctx *ctx)
{
	if (unlikely(io_req_cache_empty(ctx)))
		return __io_alloc_req_refill(ctx);
	return true;
}

static inline struct io_kiocb *io_alloc_req(struct io_ring_ctx *ctx)
{
	struct io_wq_work_node *node;

	node = wq_stack_extract(&ctx->submit_state.free_list);
	return container_of(node, struct io_kiocb, comp_list);
}

static inline void io_put_file(struct file *file)
{
	if (file)
		fput(file);
}

static inline void io_dismantle_req(struct io_kiocb *req)
{
	unsigned int flags = req->flags;

	if (unlikely(flags & IO_REQ_CLEAN_FLAGS))
		io_clean_op(req);
	if (!(flags & REQ_F_FIXED_FILE))
		io_put_file(req->file);
}

static __cold void io_free_req(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;

	io_req_put_rsrc(req);
	io_dismantle_req(req);
	io_put_task(req->task, 1);

	spin_lock(&ctx->completion_lock);
	wq_list_add_head(&req->comp_list, &ctx->locked_free_list);
	ctx->locked_free_nr++;
	spin_unlock(&ctx->completion_lock);
}

static inline void io_remove_next_linked(struct io_kiocb *req)
{
	struct io_kiocb *nxt = req->link;

	req->link = nxt->link;
	nxt->link = NULL;
}

static struct io_kiocb *io_disarm_linked_timeout(struct io_kiocb *req)
	__must_hold(&req->ctx->completion_lock)
	__must_hold(&req->ctx->timeout_lock)
{
	struct io_kiocb *link = req->link;

	if (link && link->opcode == IORING_OP_LINK_TIMEOUT) {
		struct io_timeout_data *io = link->async_data;

		io_remove_next_linked(req);
		link->timeout.head = NULL;
		if (hrtimer_try_to_cancel(&io->timer) != -1) {
			list_del(&link->timeout.list);
			return link;
		}
	}
	return NULL;
}

static void io_fail_links(struct io_kiocb *req)
	__must_hold(&req->ctx->completion_lock)
{
	struct io_kiocb *nxt, *link = req->link;
	bool ignore_cqes = req->flags & REQ_F_SKIP_LINK_CQES;

	req->link = NULL;
	while (link) {
		long res = -ECANCELED;

		if (link->flags & REQ_F_FAIL)
			res = link->cqe.res;

		nxt = link->link;
		link->link = NULL;

		trace_io_uring_fail_link(req->ctx, req, req->cqe.user_data,
					req->opcode, link);

		if (ignore_cqes)
			link->flags |= REQ_F_CQE_SKIP;
		else
			link->flags &= ~REQ_F_CQE_SKIP;
		__io_req_complete_post(link, res, 0);
		link = nxt;
	}
}

static bool io_disarm_next(struct io_kiocb *req)
	__must_hold(&req->ctx->completion_lock)
{
	struct io_kiocb *link = NULL;
	bool posted = false;

	if (req->flags & REQ_F_ARM_LTIMEOUT) {
		link = req->link;
		req->flags &= ~REQ_F_ARM_LTIMEOUT;
		if (link && link->opcode == IORING_OP_LINK_TIMEOUT) {
			io_remove_next_linked(req);
			io_req_tw_post_queue(link, -ECANCELED, 0);
			posted = true;
		}
	} else if (req->flags & REQ_F_LINK_TIMEOUT) {
		struct io_ring_ctx *ctx = req->ctx;

		spin_lock_irq(&ctx->timeout_lock);
		link = io_disarm_linked_timeout(req);
		spin_unlock_irq(&ctx->timeout_lock);
		if (link) {
			posted = true;
			io_req_tw_post_queue(link, -ECANCELED, 0);
		}
	}
	if (unlikely((req->flags & REQ_F_FAIL) &&
		     !(req->flags & REQ_F_HARDLINK))) {
		posted |= (req->link != NULL);
		io_fail_links(req);
	}
	return posted;
}

static void __io_req_find_next_prep(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;
	bool posted;

	spin_lock(&ctx->completion_lock);
	posted = io_disarm_next(req);
	io_commit_cqring(ctx);
	spin_unlock(&ctx->completion_lock);
	if (posted)
		io_cqring_ev_posted(ctx);
}

static inline struct io_kiocb *io_req_find_next(struct io_kiocb *req)
{
	struct io_kiocb *nxt;

	/*
	 * If LINK is set, we have dependent requests in this chain. If we
	 * didn't fail this request, queue the first one up, moving any other
	 * dependencies to the next request. In case of failure, fail the rest
	 * of the chain.
	 */
	if (unlikely(req->flags & IO_DISARM_MASK))
		__io_req_find_next_prep(req);
	nxt = req->link;
	req->link = NULL;
	return nxt;
}

static void ctx_flush_and_put(struct io_ring_ctx *ctx, bool *locked)
{
	if (!ctx)
		return;
	if (ctx->flags & IORING_SETUP_TASKRUN_FLAG)
		atomic_andnot(IORING_SQ_TASKRUN, &ctx->rings->sq_flags);
	if (*locked) {
		io_submit_flush_completions(ctx);
		mutex_unlock(&ctx->uring_lock);
		*locked = false;
	}
	percpu_ref_put(&ctx->refs);
}

static inline void ctx_commit_and_unlock(struct io_ring_ctx *ctx)
{
	io_commit_cqring(ctx);
	spin_unlock(&ctx->completion_lock);
	io_cqring_ev_posted(ctx);
}

static void handle_prev_tw_list(struct io_wq_work_node *node,
				struct io_ring_ctx **ctx, bool *uring_locked)
{
	if (*ctx && !*uring_locked)
		spin_lock(&(*ctx)->completion_lock);

	do {
		struct io_wq_work_node *next = node->next;
		struct io_kiocb *req = container_of(node, struct io_kiocb,
						    io_task_work.node);

		prefetch(container_of(next, struct io_kiocb, io_task_work.node));

		if (req->ctx != *ctx) {
			if (unlikely(!*uring_locked && *ctx))
				ctx_commit_and_unlock(*ctx);

			ctx_flush_and_put(*ctx, uring_locked);
			*ctx = req->ctx;
			/* if not contended, grab and improve batching */
			*uring_locked = mutex_trylock(&(*ctx)->uring_lock);
			percpu_ref_get(&(*ctx)->refs);
			if (unlikely(!*uring_locked))
				spin_lock(&(*ctx)->completion_lock);
		}
		if (likely(*uring_locked))
			req->io_task_work.func(req, uring_locked);
		else
			__io_req_complete_post(req, req->cqe.res,
						io_put_kbuf_comp(req));
		node = next;
	} while (node);

	if (unlikely(!*uring_locked))
		ctx_commit_and_unlock(*ctx);
}

static void handle_tw_list(struct io_wq_work_node *node,
			   struct io_ring_ctx **ctx, bool *locked)
{
	do {
		struct io_wq_work_node *next = node->next;
		struct io_kiocb *req = container_of(node, struct io_kiocb,
						    io_task_work.node);

		prefetch(container_of(next, struct io_kiocb, io_task_work.node));

		if (req->ctx != *ctx) {
			ctx_flush_and_put(*ctx, locked);
			*ctx = req->ctx;
			/* if not contended, grab and improve batching */
			*locked = mutex_trylock(&(*ctx)->uring_lock);
			percpu_ref_get(&(*ctx)->refs);
		}
		req->io_task_work.func(req, locked);
		node = next;
	} while (node);
}

static void tctx_task_work(struct callback_head *cb)
{
	bool uring_locked = false;
	struct io_ring_ctx *ctx = NULL;
	struct io_uring_task *tctx = container_of(cb, struct io_uring_task,
						  task_work);

	while (1) {
		struct io_wq_work_node *node1, *node2;

		spin_lock_irq(&tctx->task_lock);
		node1 = tctx->prio_task_list.first;
		node2 = tctx->task_list.first;
		INIT_WQ_LIST(&tctx->task_list);
		INIT_WQ_LIST(&tctx->prio_task_list);
		if (!node2 && !node1)
			tctx->task_running = false;
		spin_unlock_irq(&tctx->task_lock);
		if (!node2 && !node1)
			break;

		if (node1)
			handle_prev_tw_list(node1, &ctx, &uring_locked);
		if (node2)
			handle_tw_list(node2, &ctx, &uring_locked);
		cond_resched();

		if (data_race(!tctx->task_list.first) &&
		    data_race(!tctx->prio_task_list.first) && uring_locked)
			io_submit_flush_completions(ctx);
	}

	ctx_flush_and_put(ctx, &uring_locked);

	/* relaxed read is enough as only the task itself sets ->in_idle */
	if (unlikely(atomic_read(&tctx->in_idle)))
		io_uring_drop_tctx_refs(current);
}

static void __io_req_task_work_add(struct io_kiocb *req,
				   struct io_uring_task *tctx,
				   struct io_wq_work_list *list)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_wq_work_node *node;
	unsigned long flags;
	bool running;

	spin_lock_irqsave(&tctx->task_lock, flags);
	wq_list_add_tail(&req->io_task_work.node, list);
	running = tctx->task_running;
	if (!running)
		tctx->task_running = true;
	spin_unlock_irqrestore(&tctx->task_lock, flags);

	/* task_work already pending, we're done */
	if (running)
		return;

	if (ctx->flags & IORING_SETUP_TASKRUN_FLAG)
		atomic_or(IORING_SQ_TASKRUN, &ctx->rings->sq_flags);

	if (likely(!task_work_add(req->task, &tctx->task_work, ctx->notify_method)))
		return;

	spin_lock_irqsave(&tctx->task_lock, flags);
	tctx->task_running = false;
	node = wq_list_merge(&tctx->prio_task_list, &tctx->task_list);
	spin_unlock_irqrestore(&tctx->task_lock, flags);

	while (node) {
		req = container_of(node, struct io_kiocb, io_task_work.node);
		node = node->next;
		if (llist_add(&req->io_task_work.fallback_node,
			      &req->ctx->fallback_llist))
			schedule_delayed_work(&req->ctx->fallback_work, 1);
	}
}

static void io_req_task_work_add(struct io_kiocb *req)
{
	struct io_uring_task *tctx = req->task->io_uring;

	__io_req_task_work_add(req, tctx, &tctx->task_list);
}

static void io_req_task_prio_work_add(struct io_kiocb *req)
{
	struct io_uring_task *tctx = req->task->io_uring;

	if (req->ctx->flags & IORING_SETUP_SQPOLL)
		__io_req_task_work_add(req, tctx, &tctx->prio_task_list);
	else
		__io_req_task_work_add(req, tctx, &tctx->task_list);
}

static void io_req_tw_post(struct io_kiocb *req, bool *locked)
{
	io_req_complete_post(req, req->cqe.res, req->cqe.flags);
}

static void io_req_tw_post_queue(struct io_kiocb *req, s32 res, u32 cflags)
{
	req->cqe.res = res;
	req->cqe.flags = cflags;
	req->io_task_work.func = io_req_tw_post;
	io_req_task_work_add(req);
}

static void io_req_task_cancel(struct io_kiocb *req, bool *locked)
{
	/* not needed for normal modes, but SQPOLL depends on it */
	io_tw_lock(req->ctx, locked);
	io_req_complete_failed(req, req->cqe.res);
}

static void io_req_task_submit(struct io_kiocb *req, bool *locked)
{
	io_tw_lock(req->ctx, locked);
	/* req->task == current here, checking PF_EXITING is safe */
	if (likely(!(req->task->flags & PF_EXITING)))
		io_queue_sqe(req);
	else
		io_req_complete_failed(req, -EFAULT);
}

static void io_req_task_queue_fail(struct io_kiocb *req, int ret)
{
	req->cqe.res = ret;
	req->io_task_work.func = io_req_task_cancel;
	io_req_task_work_add(req);
}

static void io_req_task_queue(struct io_kiocb *req)
{
	req->io_task_work.func = io_req_task_submit;
	io_req_task_work_add(req);
}

static void io_req_task_queue_reissue(struct io_kiocb *req)
{
	req->io_task_work.func = io_queue_iowq;
	io_req_task_work_add(req);
}

static void io_queue_next(struct io_kiocb *req)
{
	struct io_kiocb *nxt = io_req_find_next(req);

	if (nxt)
		io_req_task_queue(nxt);
}

static void io_free_batch_list(struct io_ring_ctx *ctx,
				struct io_wq_work_node *node)
	__must_hold(&ctx->uring_lock)
{
	struct task_struct *task = NULL;
	int task_refs = 0;

	do {
		struct io_kiocb *req = container_of(node, struct io_kiocb,
						    comp_list);

		if (unlikely(req->flags & IO_REQ_CLEAN_SLOW_FLAGS)) {
			if (req->flags & REQ_F_REFCOUNT) {
				node = req->comp_list.next;
				if (!req_ref_put_and_test(req))
					continue;
			}
			if ((req->flags & REQ_F_POLLED) && req->apoll) {
				struct async_poll *apoll = req->apoll;

				if (apoll->double_poll)
					kfree(apoll->double_poll);
				list_add(&apoll->poll.wait.entry,
						&ctx->apoll_cache);
				req->flags &= ~REQ_F_POLLED;
			}
			if (req->flags & IO_REQ_LINK_FLAGS)
				io_queue_next(req);
			if (unlikely(req->flags & IO_REQ_CLEAN_FLAGS))
				io_clean_op(req);
		}
		if (!(req->flags & REQ_F_FIXED_FILE))
			io_put_file(req->file);

		io_req_put_rsrc_locked(req, ctx);

		if (req->task != task) {
			if (task)
				io_put_task(task, task_refs);
			task = req->task;
			task_refs = 0;
		}
		task_refs++;
		node = req->comp_list.next;
		io_req_add_to_cache(req, ctx);
	} while (node);

	if (task)
		io_put_task(task, task_refs);
}

static void __io_submit_flush_completions(struct io_ring_ctx *ctx)
	__must_hold(&ctx->uring_lock)
{
	struct io_wq_work_node *node, *prev;
	struct io_submit_state *state = &ctx->submit_state;

	if (state->flush_cqes) {
		spin_lock(&ctx->completion_lock);
		wq_list_for_each(node, prev, &state->compl_reqs) {
			struct io_kiocb *req = container_of(node, struct io_kiocb,
						    comp_list);

			if (!(req->flags & REQ_F_CQE_SKIP))
				__io_fill_cqe_req(ctx, req);
		}

		io_commit_cqring(ctx);
		spin_unlock(&ctx->completion_lock);
		io_cqring_ev_posted(ctx);
		state->flush_cqes = false;
	}

	io_free_batch_list(ctx, state->compl_reqs.first);
	INIT_WQ_LIST(&state->compl_reqs);
}

/*
 * Drop reference to request, return next in chain (if there is one) if this
 * was the last reference to this request.
 */
static inline struct io_kiocb *io_put_req_find_next(struct io_kiocb *req)
{
	struct io_kiocb *nxt = NULL;

	if (req_ref_put_and_test(req)) {
		if (unlikely(req->flags & IO_REQ_LINK_FLAGS))
			nxt = io_req_find_next(req);
		io_free_req(req);
	}
	return nxt;
}

static inline void io_put_req(struct io_kiocb *req)
{
	if (req_ref_put_and_test(req)) {
		io_queue_next(req);
		io_free_req(req);
	}
}

static unsigned io_cqring_events(struct io_ring_ctx *ctx)
{
	/* See comment at the top of this file */
	smp_rmb();
	return __io_cqring_events(ctx);
}

static inline unsigned int io_sqring_entries(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;

	/* make sure SQ entry isn't read before tail */
	return smp_load_acquire(&rings->sq.tail) - ctx->cached_sq_head;
}

static inline bool io_run_task_work(void)
{
	if (test_thread_flag(TIF_NOTIFY_SIGNAL) || task_work_pending(current)) {
		__set_current_state(TASK_RUNNING);
		clear_notify_signal();
		if (task_work_pending(current))
			task_work_run();
		return true;
	}

	return false;
}

static int io_do_iopoll(struct io_ring_ctx *ctx, bool force_nonspin)
{
	struct io_wq_work_node *pos, *start, *prev;
	unsigned int poll_flags = BLK_POLL_NOSLEEP;
	DEFINE_IO_COMP_BATCH(iob);
	int nr_events = 0;

	/*
	 * Only spin for completions if we don't have multiple devices hanging
	 * off our complete list.
	 */
	if (ctx->poll_multi_queue || force_nonspin)
		poll_flags |= BLK_POLL_ONESHOT;

	wq_list_for_each(pos, start, &ctx->iopoll_list) {
		struct io_kiocb *req = container_of(pos, struct io_kiocb, comp_list);
		struct kiocb *kiocb = &req->rw.kiocb;
		int ret;

		/*
		 * Move completed and retryable entries to our local lists.
		 * If we find a request that requires polling, break out
		 * and complete those lists first, if we have entries there.
		 */
		if (READ_ONCE(req->iopoll_completed))
			break;

		ret = kiocb->ki_filp->f_op->iopoll(kiocb, &iob, poll_flags);
		if (unlikely(ret < 0))
			return ret;
		else if (ret)
			poll_flags |= BLK_POLL_ONESHOT;

		/* iopoll may have completed current req */
		if (!rq_list_empty(iob.req_list) ||
		    READ_ONCE(req->iopoll_completed))
			break;
	}

	if (!rq_list_empty(iob.req_list))
		iob.complete(&iob);
	else if (!pos)
		return 0;

	prev = start;
	wq_list_for_each_resume(pos, prev) {
		struct io_kiocb *req = container_of(pos, struct io_kiocb, comp_list);

		/* order with io_complete_rw_iopoll(), e.g. ->result updates */
		if (!smp_load_acquire(&req->iopoll_completed))
			break;
		nr_events++;
		if (unlikely(req->flags & REQ_F_CQE_SKIP))
			continue;

		req->cqe.flags = io_put_kbuf(req, 0);
		__io_fill_cqe_req(req->ctx, req);
	}

	if (unlikely(!nr_events))
		return 0;

	io_commit_cqring(ctx);
	io_cqring_ev_posted_iopoll(ctx);
	pos = start ? start->next : ctx->iopoll_list.first;
	wq_list_cut(&ctx->iopoll_list, prev, start);
	io_free_batch_list(ctx, pos);
	return nr_events;
}

/*
 * We can't just wait for polled events to come to us, we have to actively
 * find and complete them.
 */
static __cold void io_iopoll_try_reap_events(struct io_ring_ctx *ctx)
{
	if (!(ctx->flags & IORING_SETUP_IOPOLL))
		return;

	mutex_lock(&ctx->uring_lock);
	while (!wq_list_empty(&ctx->iopoll_list)) {
		/* let it sleep and repeat later if can't complete a request */
		if (io_do_iopoll(ctx, true) == 0)
			break;
		/*
		 * Ensure we allow local-to-the-cpu processing to take place,
		 * in this case we need to ensure that we reap all events.
		 * Also let task_work, etc. to progress by releasing the mutex
		 */
		if (need_resched()) {
			mutex_unlock(&ctx->uring_lock);
			cond_resched();
			mutex_lock(&ctx->uring_lock);
		}
	}
	mutex_unlock(&ctx->uring_lock);
}

static int io_iopoll_check(struct io_ring_ctx *ctx, long min)
{
	unsigned int nr_events = 0;
	int ret = 0;
	unsigned long check_cq;

	/*
	 * Don't enter poll loop if we already have events pending.
	 * If we do, we can potentially be spinning for commands that
	 * already triggered a CQE (eg in error).
	 */
	check_cq = READ_ONCE(ctx->check_cq);
	if (check_cq & BIT(IO_CHECK_CQ_OVERFLOW_BIT))
		__io_cqring_overflow_flush(ctx, false);
	if (io_cqring_events(ctx))
		return 0;

	/*
	 * Similarly do not spin if we have not informed the user of any
	 * dropped CQE.
	 */
	if (unlikely(check_cq & BIT(IO_CHECK_CQ_DROPPED_BIT)))
		return -EBADR;

	do {
		/*
		 * If a submit got punted to a workqueue, we can have the
		 * application entering polling for a command before it gets
		 * issued. That app will hold the uring_lock for the duration
		 * of the poll right here, so we need to take a breather every
		 * now and then to ensure that the issue has a chance to add
		 * the poll to the issued list. Otherwise we can spin here
		 * forever, while the workqueue is stuck trying to acquire the
		 * very same mutex.
		 */
		if (wq_list_empty(&ctx->iopoll_list)) {
			u32 tail = ctx->cached_cq_tail;

			mutex_unlock(&ctx->uring_lock);
			io_run_task_work();
			mutex_lock(&ctx->uring_lock);

			/* some requests don't go through iopoll_list */
			if (tail != ctx->cached_cq_tail ||
			    wq_list_empty(&ctx->iopoll_list))
				break;
		}
		ret = io_do_iopoll(ctx, !min);
		if (ret < 0)
			break;
		nr_events += ret;
		ret = 0;
	} while (nr_events < min && !need_resched());

	return ret;
}

static void kiocb_end_write(struct io_kiocb *req)
{
	/*
	 * Tell lockdep we inherited freeze protection from submission
	 * thread.
	 */
	if (req->flags & REQ_F_ISREG) {
		struct super_block *sb = file_inode(req->file)->i_sb;

		__sb_writers_acquired(sb, SB_FREEZE_WRITE);
		sb_end_write(sb);
	}
}

#ifdef CONFIG_BLOCK
static bool io_resubmit_prep(struct io_kiocb *req)
{
	struct io_async_rw *rw = req->async_data;

	if (!req_has_async_data(req))
		return !io_req_prep_async(req);
	iov_iter_restore(&rw->s.iter, &rw->s.iter_state);
	return true;
}

static bool io_rw_should_reissue(struct io_kiocb *req)
{
	umode_t mode = file_inode(req->file)->i_mode;
	struct io_ring_ctx *ctx = req->ctx;

	if (!S_ISBLK(mode) && !S_ISREG(mode))
		return false;
	if ((req->flags & REQ_F_NOWAIT) || (io_wq_current_is_worker() &&
	    !(ctx->flags & IORING_SETUP_IOPOLL)))
		return false;
	/*
	 * If ref is dying, we might be running poll reap from the exit work.
	 * Don't attempt to reissue from that path, just let it fail with
	 * -EAGAIN.
	 */
	if (percpu_ref_is_dying(&ctx->refs))
		return false;
	/*
	 * Play it safe and assume not safe to re-import and reissue if we're
	 * not in the original thread group (or in task context).
	 */
	if (!same_thread_group(req->task, current) || !in_task())
		return false;
	return true;
}
#else
static bool io_resubmit_prep(struct io_kiocb *req)
{
	return false;
}
static bool io_rw_should_reissue(struct io_kiocb *req)
{
	return false;
}
#endif

static bool __io_complete_rw_common(struct io_kiocb *req, long res)
{
	if (req->rw.kiocb.ki_flags & IOCB_WRITE) {
		kiocb_end_write(req);
		fsnotify_modify(req->file);
	} else {
		fsnotify_access(req->file);
	}
	if (unlikely(res != req->cqe.res)) {
		if ((res == -EAGAIN || res == -EOPNOTSUPP) &&
		    io_rw_should_reissue(req)) {
			req->flags |= REQ_F_REISSUE;
			return true;
		}
		req_set_fail(req);
		req->cqe.res = res;
	}
	return false;
}

static inline void io_req_task_complete(struct io_kiocb *req, bool *locked)
{
	int res = req->cqe.res;

	if (*locked) {
		io_req_complete_state(req, res, io_put_kbuf(req, 0));
		io_req_add_compl_list(req);
	} else {
		io_req_complete_post(req, res,
					io_put_kbuf(req, IO_URING_F_UNLOCKED));
	}
}

static void __io_complete_rw(struct io_kiocb *req, long res,
			     unsigned int issue_flags)
{
	if (__io_complete_rw_common(req, res))
		return;
	__io_req_complete(req, issue_flags, req->cqe.res,
				io_put_kbuf(req, issue_flags));
}

static void io_complete_rw(struct kiocb *kiocb, long res)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw.kiocb);

	if (__io_complete_rw_common(req, res))
		return;
	req->cqe.res = res;
	req->io_task_work.func = io_req_task_complete;
	io_req_task_prio_work_add(req);
}

static void io_complete_rw_iopoll(struct kiocb *kiocb, long res)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw.kiocb);

	if (kiocb->ki_flags & IOCB_WRITE)
		kiocb_end_write(req);
	if (unlikely(res != req->cqe.res)) {
		if (res == -EAGAIN && io_rw_should_reissue(req)) {
			req->flags |= REQ_F_REISSUE;
			return;
		}
		req->cqe.res = res;
	}

	/* order with io_iopoll_complete() checking ->iopoll_completed */
	smp_store_release(&req->iopoll_completed, 1);
}

/*
 * After the iocb has been issued, it's safe to be found on the poll list.
 * Adding the kiocb to the list AFTER submission ensures that we don't
 * find it from a io_do_iopoll() thread before the issuer is done
 * accessing the kiocb cookie.
 */
static void io_iopoll_req_issued(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	const bool needs_lock = issue_flags & IO_URING_F_UNLOCKED;

	/* workqueue context doesn't hold uring_lock, grab it now */
	if (unlikely(needs_lock))
		mutex_lock(&ctx->uring_lock);

	/*
	 * Track whether we have multiple files in our lists. This will impact
	 * how we do polling eventually, not spinning if we're on potentially
	 * different devices.
	 */
	if (wq_list_empty(&ctx->iopoll_list)) {
		ctx->poll_multi_queue = false;
	} else if (!ctx->poll_multi_queue) {
		struct io_kiocb *list_req;

		list_req = container_of(ctx->iopoll_list.first, struct io_kiocb,
					comp_list);
		if (list_req->file != req->file)
			ctx->poll_multi_queue = true;
	}

	/*
	 * For fast devices, IO may have already completed. If it has, add
	 * it to the front so we find it first.
	 */
	if (READ_ONCE(req->iopoll_completed))
		wq_list_add_head(&req->comp_list, &ctx->iopoll_list);
	else
		wq_list_add_tail(&req->comp_list, &ctx->iopoll_list);

	if (unlikely(needs_lock)) {
		/*
		 * If IORING_SETUP_SQPOLL is enabled, sqes are either handle
		 * in sq thread task context or in io worker task context. If
		 * current task context is sq thread, we don't need to check
		 * whether should wake up sq thread.
		 */
		if ((ctx->flags & IORING_SETUP_SQPOLL) &&
		    wq_has_sleeper(&ctx->sq_data->wait))
			wake_up(&ctx->sq_data->wait);

		mutex_unlock(&ctx->uring_lock);
	}
}

static bool io_bdev_nowait(struct block_device *bdev)
{
	return !bdev || blk_queue_nowait(bdev_get_queue(bdev));
}

/*
 * If we tracked the file through the SCM inflight mechanism, we could support
 * any file. For now, just ensure that anything potentially problematic is done
 * inline.
 */
static bool __io_file_supports_nowait(struct file *file, umode_t mode)
{
	if (S_ISBLK(mode)) {
		if (IS_ENABLED(CONFIG_BLOCK) &&
		    io_bdev_nowait(I_BDEV(file->f_mapping->host)))
			return true;
		return false;
	}
	if (S_ISSOCK(mode))
		return true;
	if (S_ISREG(mode)) {
		if (IS_ENABLED(CONFIG_BLOCK) &&
		    io_bdev_nowait(file->f_inode->i_sb->s_bdev) &&
		    file->f_op != &io_uring_fops)
			return true;
		return false;
	}

	/* any ->read/write should understand O_NONBLOCK */
	if (file->f_flags & O_NONBLOCK)
		return true;
	return file->f_mode & FMODE_NOWAIT;
}

/*
 * If we tracked the file through the SCM inflight mechanism, we could support
 * any file. For now, just ensure that anything potentially problematic is done
 * inline.
 */
static unsigned int io_file_get_flags(struct file *file)
{
	umode_t mode = file_inode(file)->i_mode;
	unsigned int res = 0;

	if (S_ISREG(mode))
		res |= FFS_ISREG;
	if (__io_file_supports_nowait(file, mode))
		res |= FFS_NOWAIT;
	if (io_file_need_scm(file))
		res |= FFS_SCM;
	return res;
}

static inline bool io_file_supports_nowait(struct io_kiocb *req)
{
	return req->flags & REQ_F_SUPPORT_NOWAIT;
}

static int io_prep_rw(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct kiocb *kiocb = &req->rw.kiocb;
	unsigned ioprio;
	int ret;

	kiocb->ki_pos = READ_ONCE(sqe->off);
	/* used for fixed read/write too - just read unconditionally */
	req->buf_index = READ_ONCE(sqe->buf_index);

	if (req->opcode == IORING_OP_READ_FIXED ||
	    req->opcode == IORING_OP_WRITE_FIXED) {
		struct io_ring_ctx *ctx = req->ctx;
		u16 index;

		if (unlikely(req->buf_index >= ctx->nr_user_bufs))
			return -EFAULT;
		index = array_index_nospec(req->buf_index, ctx->nr_user_bufs);
		req->imu = ctx->user_bufs[index];
		io_req_set_rsrc_node(req, ctx, 0);
	}

	ioprio = READ_ONCE(sqe->ioprio);
	if (ioprio) {
		ret = ioprio_check_cap(ioprio);
		if (ret)
			return ret;

		kiocb->ki_ioprio = ioprio;
	} else {
		kiocb->ki_ioprio = get_current_ioprio();
	}

	req->rw.addr = READ_ONCE(sqe->addr);
	req->rw.len = READ_ONCE(sqe->len);
	req->rw.flags = READ_ONCE(sqe->rw_flags);
	return 0;
}

static inline void io_rw_done(struct kiocb *kiocb, ssize_t ret)
{
	switch (ret) {
	case -EIOCBQUEUED:
		break;
	case -ERESTARTSYS:
	case -ERESTARTNOINTR:
	case -ERESTARTNOHAND:
	case -ERESTART_RESTARTBLOCK:
		/*
		 * We can't just restart the syscall, since previously
		 * submitted sqes may already be in progress. Just fail this
		 * IO with EINTR.
		 */
		ret = -EINTR;
		fallthrough;
	default:
		kiocb->ki_complete(kiocb, ret);
	}
}

static inline loff_t *io_kiocb_update_pos(struct io_kiocb *req)
{
	struct kiocb *kiocb = &req->rw.kiocb;

	if (kiocb->ki_pos != -1)
		return &kiocb->ki_pos;

	if (!(req->file->f_mode & FMODE_STREAM)) {
		req->flags |= REQ_F_CUR_POS;
		kiocb->ki_pos = req->file->f_pos;
		return &kiocb->ki_pos;
	}

	kiocb->ki_pos = 0;
	return NULL;
}

static void kiocb_done(struct io_kiocb *req, ssize_t ret,
		       unsigned int issue_flags)
{
	struct io_async_rw *io = req->async_data;

	/* add previously done IO, if any */
	if (req_has_async_data(req) && io->bytes_done > 0) {
		if (ret < 0)
			ret = io->bytes_done;
		else
			ret += io->bytes_done;
	}

	if (req->flags & REQ_F_CUR_POS)
		req->file->f_pos = req->rw.kiocb.ki_pos;
	if (ret >= 0 && (req->rw.kiocb.ki_complete == io_complete_rw))
		__io_complete_rw(req, ret, issue_flags);
	else
		io_rw_done(&req->rw.kiocb, ret);

	if (req->flags & REQ_F_REISSUE) {
		req->flags &= ~REQ_F_REISSUE;
		if (io_resubmit_prep(req))
			io_req_task_queue_reissue(req);
		else
			io_req_task_queue_fail(req, ret);
	}
}

static int __io_import_fixed(struct io_kiocb *req, int rw, struct iov_iter *iter,
			     struct io_mapped_ubuf *imu)
{
	size_t len = req->rw.len;
	u64 buf_end, buf_addr = req->rw.addr;
	size_t offset;

	if (unlikely(check_add_overflow(buf_addr, (u64)len, &buf_end)))
		return -EFAULT;
	/* not inside the mapped region */
	if (unlikely(buf_addr < imu->ubuf || buf_end > imu->ubuf_end))
		return -EFAULT;

	/*
	 * May not be a start of buffer, set size appropriately
	 * and advance us to the beginning.
	 */
	offset = buf_addr - imu->ubuf;
	iov_iter_bvec(iter, rw, imu->bvec, imu->nr_bvecs, offset + len);

	if (offset) {
		/*
		 * Don't use iov_iter_advance() here, as it's really slow for
		 * using the latter parts of a big fixed buffer - it iterates
		 * over each segment manually. We can cheat a bit here, because
		 * we know that:
		 *
		 * 1) it's a BVEC iter, we set it up
		 * 2) all bvecs are PAGE_SIZE in size, except potentially the
		 *    first and last bvec
		 *
		 * So just find our index, and adjust the iterator afterwards.
		 * If the offset is within the first bvec (or the whole first
		 * bvec, just use iov_iter_advance(). This makes it easier
		 * since we can just skip the first segment, which may not
		 * be PAGE_SIZE aligned.
		 */
		const struct bio_vec *bvec = imu->bvec;

		if (offset <= bvec->bv_len) {
			iov_iter_advance(iter, offset);
		} else {
			unsigned long seg_skip;

			/* skip first vec */
			offset -= bvec->bv_len;
			seg_skip = 1 + (offset >> PAGE_SHIFT);

			iter->bvec = bvec + seg_skip;
			iter->nr_segs -= seg_skip;
			iter->count -= bvec->bv_len + offset;
			iter->iov_offset = offset & ~PAGE_MASK;
		}
	}

	return 0;
}

static int io_import_fixed(struct io_kiocb *req, int rw, struct iov_iter *iter,
			   unsigned int issue_flags)
{
	if (WARN_ON_ONCE(!req->imu))
		return -EFAULT;
	return __io_import_fixed(req, rw, iter, req->imu);
}

static int io_buffer_add_list(struct io_ring_ctx *ctx,
			      struct io_buffer_list *bl, unsigned int bgid)
{
	bl->bgid = bgid;
	if (bgid < BGID_ARRAY)
		return 0;

	return xa_err(xa_store(&ctx->io_bl_xa, bgid, bl, GFP_KERNEL));
}

static void __user *io_provided_buffer_select(struct io_kiocb *req, size_t *len,
					      struct io_buffer_list *bl)
{
	if (!list_empty(&bl->buf_list)) {
		struct io_buffer *kbuf;

		kbuf = list_first_entry(&bl->buf_list, struct io_buffer, list);
		list_del(&kbuf->list);
		if (*len > kbuf->len)
			*len = kbuf->len;
		req->flags |= REQ_F_BUFFER_SELECTED;
		req->kbuf = kbuf;
		req->buf_index = kbuf->bid;
		return u64_to_user_ptr(kbuf->addr);
	}
	return NULL;
}

static void __user *io_ring_buffer_select(struct io_kiocb *req, size_t *len,
					  struct io_buffer_list *bl,
					  unsigned int issue_flags)
{
	struct io_uring_buf_ring *br = bl->buf_ring;
	struct io_uring_buf *buf;
	__u16 head = bl->head;

	if (unlikely(smp_load_acquire(&br->tail) == head))
		return NULL;

	head &= bl->mask;
	if (head < IO_BUFFER_LIST_BUF_PER_PAGE) {
		buf = &br->bufs[head];
	} else {
		int off = head & (IO_BUFFER_LIST_BUF_PER_PAGE - 1);
		int index = head / IO_BUFFER_LIST_BUF_PER_PAGE;
		buf = page_address(bl->buf_pages[index]);
		buf += off;
	}
	if (*len > buf->len)
		*len = buf->len;
	req->flags |= REQ_F_BUFFER_RING;
	req->buf_list = bl;
	req->buf_index = buf->bid;

	if (issue_flags & IO_URING_F_UNLOCKED || !file_can_poll(req->file)) {
		/*
		 * If we came in unlocked, we have no choice but to consume the
		 * buffer here. This does mean it'll be pinned until the IO
		 * completes. But coming in unlocked means we're in io-wq
		 * context, hence there should be no further retry. For the
		 * locked case, the caller must ensure to call the commit when
		 * the transfer completes (or if we get -EAGAIN and must poll
		 * or retry).
		 */
		req->buf_list = NULL;
		bl->head++;
	}
	return u64_to_user_ptr(buf->addr);
}

static void __user *io_buffer_select(struct io_kiocb *req, size_t *len,
				     unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	void __user *ret = NULL;

	io_ring_submit_lock(req->ctx, issue_flags);

	bl = io_buffer_get_list(ctx, req->buf_index);
	if (likely(bl)) {
		if (bl->buf_nr_pages)
			ret = io_ring_buffer_select(req, len, bl, issue_flags);
		else
			ret = io_provided_buffer_select(req, len, bl);
	}
	io_ring_submit_unlock(req->ctx, issue_flags);
	return ret;
}

#ifdef CONFIG_COMPAT
static ssize_t io_compat_import(struct io_kiocb *req, struct iovec *iov,
				unsigned int issue_flags)
{
	struct compat_iovec __user *uiov;
	compat_ssize_t clen;
	void __user *buf;
	size_t len;

	uiov = u64_to_user_ptr(req->rw.addr);
	if (!access_ok(uiov, sizeof(*uiov)))
		return -EFAULT;
	if (__get_user(clen, &uiov->iov_len))
		return -EFAULT;
	if (clen < 0)
		return -EINVAL;

	len = clen;
	buf = io_buffer_select(req, &len, issue_flags);
	if (!buf)
		return -ENOBUFS;
	req->rw.addr = (unsigned long) buf;
	iov[0].iov_base = buf;
	req->rw.len = iov[0].iov_len = (compat_size_t) len;
	return 0;
}
#endif

static ssize_t __io_iov_buffer_select(struct io_kiocb *req, struct iovec *iov,
				      unsigned int issue_flags)
{
	struct iovec __user *uiov = u64_to_user_ptr(req->rw.addr);
	void __user *buf;
	ssize_t len;

	if (copy_from_user(iov, uiov, sizeof(*uiov)))
		return -EFAULT;

	len = iov[0].iov_len;
	if (len < 0)
		return -EINVAL;
	buf = io_buffer_select(req, &len, issue_flags);
	if (!buf)
		return -ENOBUFS;
	req->rw.addr = (unsigned long) buf;
	iov[0].iov_base = buf;
	req->rw.len = iov[0].iov_len = len;
	return 0;
}

static ssize_t io_iov_buffer_select(struct io_kiocb *req, struct iovec *iov,
				    unsigned int issue_flags)
{
	if (req->flags & (REQ_F_BUFFER_SELECTED|REQ_F_BUFFER_RING)) {
		iov[0].iov_base = u64_to_user_ptr(req->rw.addr);
		iov[0].iov_len = req->rw.len;
		return 0;
	}
	if (req->rw.len != 1)
		return -EINVAL;

#ifdef CONFIG_COMPAT
	if (req->ctx->compat)
		return io_compat_import(req, iov, issue_flags);
#endif

	return __io_iov_buffer_select(req, iov, issue_flags);
}

static inline bool io_do_buffer_select(struct io_kiocb *req)
{
	if (!(req->flags & REQ_F_BUFFER_SELECT))
		return false;
	return !(req->flags & (REQ_F_BUFFER_SELECTED|REQ_F_BUFFER_RING));
}

static struct iovec *__io_import_iovec(int rw, struct io_kiocb *req,
				       struct io_rw_state *s,
				       unsigned int issue_flags)
{
	struct iov_iter *iter = &s->iter;
	u8 opcode = req->opcode;
	struct iovec *iovec;
	void __user *buf;
	size_t sqe_len;
	ssize_t ret;

	if (opcode == IORING_OP_READ_FIXED || opcode == IORING_OP_WRITE_FIXED) {
		ret = io_import_fixed(req, rw, iter, issue_flags);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	buf = u64_to_user_ptr(req->rw.addr);
	sqe_len = req->rw.len;

	if (opcode == IORING_OP_READ || opcode == IORING_OP_WRITE) {
		if (io_do_buffer_select(req)) {
			buf = io_buffer_select(req, &sqe_len, issue_flags);
			if (!buf)
				return ERR_PTR(-ENOBUFS);
			req->rw.addr = (unsigned long) buf;
			req->rw.len = sqe_len;
		}

		ret = import_single_range(rw, buf, sqe_len, s->fast_iov, iter);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	iovec = s->fast_iov;
	if (req->flags & REQ_F_BUFFER_SELECT) {
		ret = io_iov_buffer_select(req, iovec, issue_flags);
		if (ret)
			return ERR_PTR(ret);
		iov_iter_init(iter, rw, iovec, 1, iovec->iov_len);
		return NULL;
	}

	ret = __import_iovec(rw, buf, sqe_len, UIO_FASTIOV, &iovec, iter,
			      req->ctx->compat);
	if (unlikely(ret < 0))
		return ERR_PTR(ret);
	return iovec;
}

static inline int io_import_iovec(int rw, struct io_kiocb *req,
				  struct iovec **iovec, struct io_rw_state *s,
				  unsigned int issue_flags)
{
	*iovec = __io_import_iovec(rw, req, s, issue_flags);
	if (unlikely(IS_ERR(*iovec)))
		return PTR_ERR(*iovec);

	iov_iter_save_state(&s->iter, &s->iter_state);
	return 0;
}

static inline loff_t *io_kiocb_ppos(struct kiocb *kiocb)
{
	return (kiocb->ki_filp->f_mode & FMODE_STREAM) ? NULL : &kiocb->ki_pos;
}

/*
 * For files that don't have ->read_iter() and ->write_iter(), handle them
 * by looping over ->read() or ->write() manually.
 */
static ssize_t loop_rw_iter(int rw, struct io_kiocb *req, struct iov_iter *iter)
{
	struct kiocb *kiocb = &req->rw.kiocb;
	struct file *file = req->file;
	ssize_t ret = 0;
	loff_t *ppos;

	/*
	 * Don't support polled IO through this interface, and we can't
	 * support non-blocking either. For the latter, this just causes
	 * the kiocb to be handled from an async context.
	 */
	if (kiocb->ki_flags & IOCB_HIPRI)
		return -EOPNOTSUPP;
	if ((kiocb->ki_flags & IOCB_NOWAIT) &&
	    !(kiocb->ki_filp->f_flags & O_NONBLOCK))
		return -EAGAIN;

	ppos = io_kiocb_ppos(kiocb);

	while (iov_iter_count(iter)) {
		struct iovec iovec;
		ssize_t nr;

		if (!iov_iter_is_bvec(iter)) {
			iovec = iov_iter_iovec(iter);
		} else {
			iovec.iov_base = u64_to_user_ptr(req->rw.addr);
			iovec.iov_len = req->rw.len;
		}

		if (rw == READ) {
			nr = file->f_op->read(file, iovec.iov_base,
					      iovec.iov_len, ppos);
		} else {
			nr = file->f_op->write(file, iovec.iov_base,
					       iovec.iov_len, ppos);
		}

		if (nr < 0) {
			if (!ret)
				ret = nr;
			break;
		}
		ret += nr;
		if (!iov_iter_is_bvec(iter)) {
			iov_iter_advance(iter, nr);
		} else {
			req->rw.addr += nr;
			req->rw.len -= nr;
			if (!req->rw.len)
				break;
		}
		if (nr != iovec.iov_len)
			break;
	}

	return ret;
}

static void io_req_map_rw(struct io_kiocb *req, const struct iovec *iovec,
			  const struct iovec *fast_iov, struct iov_iter *iter)
{
	struct io_async_rw *rw = req->async_data;

	memcpy(&rw->s.iter, iter, sizeof(*iter));
	rw->free_iovec = iovec;
	rw->bytes_done = 0;
	/* can only be fixed buffers, no need to do anything */
	if (iov_iter_is_bvec(iter))
		return;
	if (!iovec) {
		unsigned iov_off = 0;

		rw->s.iter.iov = rw->s.fast_iov;
		if (iter->iov != fast_iov) {
			iov_off = iter->iov - fast_iov;
			rw->s.iter.iov += iov_off;
		}
		if (rw->s.fast_iov != fast_iov)
			memcpy(rw->s.fast_iov + iov_off, fast_iov + iov_off,
			       sizeof(struct iovec) * iter->nr_segs);
	} else {
		req->flags |= REQ_F_NEED_CLEANUP;
	}
}

static inline bool io_alloc_async_data(struct io_kiocb *req)
{
	WARN_ON_ONCE(!io_op_defs[req->opcode].async_size);
	req->async_data = kmalloc(io_op_defs[req->opcode].async_size, GFP_KERNEL);
	if (req->async_data) {
		req->flags |= REQ_F_ASYNC_DATA;
		return false;
	}
	return true;
}

static int io_setup_async_rw(struct io_kiocb *req, const struct iovec *iovec,
			     struct io_rw_state *s, bool force)
{
	if (!force && !io_op_defs[req->opcode].needs_async_setup)
		return 0;
	if (!req_has_async_data(req)) {
		struct io_async_rw *iorw;

		if (io_alloc_async_data(req)) {
			kfree(iovec);
			return -ENOMEM;
		}

		io_req_map_rw(req, iovec, s->fast_iov, &s->iter);
		iorw = req->async_data;
		/* we've copied and mapped the iter, ensure state is saved */
		iov_iter_save_state(&iorw->s.iter, &iorw->s.iter_state);
	}
	return 0;
}

static inline int io_rw_prep_async(struct io_kiocb *req, int rw)
{
	struct io_async_rw *iorw = req->async_data;
	struct iovec *iov;
	int ret;

	/* submission path, ->uring_lock should already be taken */
	ret = io_import_iovec(rw, req, &iov, &iorw->s, 0);
	if (unlikely(ret < 0))
		return ret;

	iorw->bytes_done = 0;
	iorw->free_iovec = iov;
	if (iov)
		req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_readv_prep_async(struct io_kiocb *req)
{
	return io_rw_prep_async(req, READ);
}

static int io_writev_prep_async(struct io_kiocb *req)
{
	return io_rw_prep_async(req, WRITE);
}

/*
 * This is our waitqueue callback handler, registered through __folio_lock_async()
 * when we initially tried to do the IO with the iocb armed our waitqueue.
 * This gets called when the page is unlocked, and we generally expect that to
 * happen when the page IO is completed and the page is now uptodate. This will
 * queue a task_work based retry of the operation, attempting to copy the data
 * again. If the latter fails because the page was NOT uptodate, then we will
 * do a thread based blocking retry of the operation. That's the unexpected
 * slow path.
 */
static int io_async_buf_func(struct wait_queue_entry *wait, unsigned mode,
			     int sync, void *arg)
{
	struct wait_page_queue *wpq;
	struct io_kiocb *req = wait->private;
	struct wait_page_key *key = arg;

	wpq = container_of(wait, struct wait_page_queue, wait);

	if (!wake_page_match(wpq, key))
		return 0;

	req->rw.kiocb.ki_flags &= ~IOCB_WAITQ;
	list_del_init(&wait->entry);
	io_req_task_queue(req);
	return 1;
}

/*
 * This controls whether a given IO request should be armed for async page
 * based retry. If we return false here, the request is handed to the async
 * worker threads for retry. If we're doing buffered reads on a regular file,
 * we prepare a private wait_page_queue entry and retry the operation. This
 * will either succeed because the page is now uptodate and unlocked, or it
 * will register a callback when the page is unlocked at IO completion. Through
 * that callback, io_uring uses task_work to setup a retry of the operation.
 * That retry will attempt the buffered read again. The retry will generally
 * succeed, or in rare cases where it fails, we then fall back to using the
 * async worker threads for a blocking retry.
 */
static bool io_rw_should_retry(struct io_kiocb *req)
{
	struct io_async_rw *rw = req->async_data;
	struct wait_page_queue *wait = &rw->wpq;
	struct kiocb *kiocb = &req->rw.kiocb;

	/* never retry for NOWAIT, we just complete with -EAGAIN */
	if (req->flags & REQ_F_NOWAIT)
		return false;

	/* Only for buffered IO */
	if (kiocb->ki_flags & (IOCB_DIRECT | IOCB_HIPRI))
		return false;

	/*
	 * just use poll if we can, and don't attempt if the fs doesn't
	 * support callback based unlocks
	 */
	if (file_can_poll(req->file) || !(req->file->f_mode & FMODE_BUF_RASYNC))
		return false;

	wait->wait.func = io_async_buf_func;
	wait->wait.private = req;
	wait->wait.flags = 0;
	INIT_LIST_HEAD(&wait->wait.entry);
	kiocb->ki_flags |= IOCB_WAITQ;
	kiocb->ki_flags &= ~IOCB_NOWAIT;
	kiocb->ki_waitq = wait;
	return true;
}

static inline int io_iter_do_read(struct io_kiocb *req, struct iov_iter *iter)
{
	if (likely(req->file->f_op->read_iter))
		return call_read_iter(req->file, &req->rw.kiocb, iter);
	else if (req->file->f_op->read)
		return loop_rw_iter(READ, req, iter);
	else
		return -EINVAL;
}

static bool need_read_all(struct io_kiocb *req)
{
	return req->flags & REQ_F_ISREG ||
		S_ISBLK(file_inode(req->file)->i_mode);
}

static int io_rw_init_file(struct io_kiocb *req, fmode_t mode)
{
	struct kiocb *kiocb = &req->rw.kiocb;
	struct io_ring_ctx *ctx = req->ctx;
	struct file *file = req->file;
	int ret;

	if (unlikely(!file || !(file->f_mode & mode)))
		return -EBADF;

	if (!io_req_ffs_set(req))
		req->flags |= io_file_get_flags(file) << REQ_F_SUPPORT_NOWAIT_BIT;

	kiocb->ki_flags = iocb_flags(file);
	ret = kiocb_set_rw_flags(kiocb, req->rw.flags);
	if (unlikely(ret))
		return ret;

	/*
	 * If the file is marked O_NONBLOCK, still allow retry for it if it
	 * supports async. Otherwise it's impossible to use O_NONBLOCK files
	 * reliably. If not, or it IOCB_NOWAIT is set, don't retry.
	 */
	if ((kiocb->ki_flags & IOCB_NOWAIT) ||
	    ((file->f_flags & O_NONBLOCK) && !io_file_supports_nowait(req)))
		req->flags |= REQ_F_NOWAIT;

	if (ctx->flags & IORING_SETUP_IOPOLL) {
		if (!(kiocb->ki_flags & IOCB_DIRECT) || !file->f_op->iopoll)
			return -EOPNOTSUPP;

		kiocb->private = NULL;
		kiocb->ki_flags |= IOCB_HIPRI | IOCB_ALLOC_CACHE;
		kiocb->ki_complete = io_complete_rw_iopoll;
		req->iopoll_completed = 0;
	} else {
		if (kiocb->ki_flags & IOCB_HIPRI)
			return -EINVAL;
		kiocb->ki_complete = io_complete_rw;
	}

	return 0;
}

static int io_read(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_rw_state __s, *s = &__s;
	struct iovec *iovec;
	struct kiocb *kiocb = &req->rw.kiocb;
	bool force_nonblock = issue_flags & IO_URING_F_NONBLOCK;
	struct io_async_rw *rw;
	ssize_t ret, ret2;
	loff_t *ppos;

	if (!req_has_async_data(req)) {
		ret = io_import_iovec(READ, req, &iovec, s, issue_flags);
		if (unlikely(ret < 0))
			return ret;
	} else {
		/*
		 * Safe and required to re-import if we're using provided
		 * buffers, as we dropped the selected one before retry.
		 */
		if (req->flags & REQ_F_BUFFER_SELECT) {
			ret = io_import_iovec(READ, req, &iovec, s, issue_flags);
			if (unlikely(ret < 0))
				return ret;
		}

		rw = req->async_data;
		s = &rw->s;
		/*
		 * We come here from an earlier attempt, restore our state to
		 * match in case it doesn't. It's cheap enough that we don't
		 * need to make this conditional.
		 */
		iov_iter_restore(&s->iter, &s->iter_state);
		iovec = NULL;
	}
	ret = io_rw_init_file(req, FMODE_READ);
	if (unlikely(ret)) {
		kfree(iovec);
		return ret;
	}
	req->cqe.res = iov_iter_count(&s->iter);

	if (force_nonblock) {
		/* If the file doesn't support async, just async punt */
		if (unlikely(!io_file_supports_nowait(req))) {
			ret = io_setup_async_rw(req, iovec, s, true);
			return ret ?: -EAGAIN;
		}
		kiocb->ki_flags |= IOCB_NOWAIT;
	} else {
		/* Ensure we clear previously set non-block flag */
		kiocb->ki_flags &= ~IOCB_NOWAIT;
	}

	ppos = io_kiocb_update_pos(req);

	ret = rw_verify_area(READ, req->file, ppos, req->cqe.res);
	if (unlikely(ret)) {
		kfree(iovec);
		return ret;
	}

	ret = io_iter_do_read(req, &s->iter);

	if (ret == -EAGAIN || (req->flags & REQ_F_REISSUE)) {
		req->flags &= ~REQ_F_REISSUE;
		/* if we can poll, just do that */
		if (req->opcode == IORING_OP_READ && file_can_poll(req->file))
			return -EAGAIN;
		/* IOPOLL retry should happen for io-wq threads */
		if (!force_nonblock && !(req->ctx->flags & IORING_SETUP_IOPOLL))
			goto done;
		/* no retry on NONBLOCK nor RWF_NOWAIT */
		if (req->flags & REQ_F_NOWAIT)
			goto done;
		ret = 0;
	} else if (ret == -EIOCBQUEUED) {
		goto out_free;
	} else if (ret == req->cqe.res || ret <= 0 || !force_nonblock ||
		   (req->flags & REQ_F_NOWAIT) || !need_read_all(req)) {
		/* read all, failed, already did sync or don't want to retry */
		goto done;
	}

	/*
	 * Don't depend on the iter state matching what was consumed, or being
	 * untouched in case of error. Restore it and we'll advance it
	 * manually if we need to.
	 */
	iov_iter_restore(&s->iter, &s->iter_state);

	ret2 = io_setup_async_rw(req, iovec, s, true);
	if (ret2)
		return ret2;

	iovec = NULL;
	rw = req->async_data;
	s = &rw->s;
	/*
	 * Now use our persistent iterator and state, if we aren't already.
	 * We've restored and mapped the iter to match.
	 */

	do {
		/*
		 * We end up here because of a partial read, either from
		 * above or inside this loop. Advance the iter by the bytes
		 * that were consumed.
		 */
		iov_iter_advance(&s->iter, ret);
		if (!iov_iter_count(&s->iter))
			break;
		rw->bytes_done += ret;
		iov_iter_save_state(&s->iter, &s->iter_state);

		/* if we can retry, do so with the callbacks armed */
		if (!io_rw_should_retry(req)) {
			kiocb->ki_flags &= ~IOCB_WAITQ;
			return -EAGAIN;
		}

		/*
		 * Now retry read with the IOCB_WAITQ parts set in the iocb. If
		 * we get -EIOCBQUEUED, then we'll get a notification when the
		 * desired page gets unlocked. We can also get a partial read
		 * here, and if we do, then just retry at the new offset.
		 */
		ret = io_iter_do_read(req, &s->iter);
		if (ret == -EIOCBQUEUED)
			return 0;
		/* we got some bytes, but not all. retry. */
		kiocb->ki_flags &= ~IOCB_WAITQ;
		iov_iter_restore(&s->iter, &s->iter_state);
	} while (ret > 0);
done:
	kiocb_done(req, ret, issue_flags);
out_free:
	/* it's faster to check here then delegate to kfree */
	if (iovec)
		kfree(iovec);
	return 0;
}

static int io_write(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_rw_state __s, *s = &__s;
	struct iovec *iovec;
	struct kiocb *kiocb = &req->rw.kiocb;
	bool force_nonblock = issue_flags & IO_URING_F_NONBLOCK;
	ssize_t ret, ret2;
	loff_t *ppos;

	if (!req_has_async_data(req)) {
		ret = io_import_iovec(WRITE, req, &iovec, s, issue_flags);
		if (unlikely(ret < 0))
			return ret;
	} else {
		struct io_async_rw *rw = req->async_data;

		s = &rw->s;
		iov_iter_restore(&s->iter, &s->iter_state);
		iovec = NULL;
	}
	ret = io_rw_init_file(req, FMODE_WRITE);
	if (unlikely(ret)) {
		kfree(iovec);
		return ret;
	}
	req->cqe.res = iov_iter_count(&s->iter);

	if (force_nonblock) {
		/* If the file doesn't support async, just async punt */
		if (unlikely(!io_file_supports_nowait(req)))
			goto copy_iov;

		/* file path doesn't support NOWAIT for non-direct_IO */
		if (force_nonblock && !(kiocb->ki_flags & IOCB_DIRECT) &&
		    (req->flags & REQ_F_ISREG))
			goto copy_iov;

		kiocb->ki_flags |= IOCB_NOWAIT;
	} else {
		/* Ensure we clear previously set non-block flag */
		kiocb->ki_flags &= ~IOCB_NOWAIT;
	}

	ppos = io_kiocb_update_pos(req);

	ret = rw_verify_area(WRITE, req->file, ppos, req->cqe.res);
	if (unlikely(ret))
		goto out_free;

	/*
	 * Open-code file_start_write here to grab freeze protection,
	 * which will be released by another thread in
	 * io_complete_rw().  Fool lockdep by telling it the lock got
	 * released so that it doesn't complain about the held lock when
	 * we return to userspace.
	 */
	if (req->flags & REQ_F_ISREG) {
		sb_start_write(file_inode(req->file)->i_sb);
		__sb_writers_release(file_inode(req->file)->i_sb,
					SB_FREEZE_WRITE);
	}
	kiocb->ki_flags |= IOCB_WRITE;

	if (likely(req->file->f_op->write_iter))
		ret2 = call_write_iter(req->file, kiocb, &s->iter);
	else if (req->file->f_op->write)
		ret2 = loop_rw_iter(WRITE, req, &s->iter);
	else
		ret2 = -EINVAL;

	if (req->flags & REQ_F_REISSUE) {
		req->flags &= ~REQ_F_REISSUE;
		ret2 = -EAGAIN;
	}

	/*
	 * Raw bdev writes will return -EOPNOTSUPP for IOCB_NOWAIT. Just
	 * retry them without IOCB_NOWAIT.
	 */
	if (ret2 == -EOPNOTSUPP && (kiocb->ki_flags & IOCB_NOWAIT))
		ret2 = -EAGAIN;
	/* no retry on NONBLOCK nor RWF_NOWAIT */
	if (ret2 == -EAGAIN && (req->flags & REQ_F_NOWAIT))
		goto done;
	if (!force_nonblock || ret2 != -EAGAIN) {
		/* IOPOLL retry should happen for io-wq threads */
		if (ret2 == -EAGAIN && (req->ctx->flags & IORING_SETUP_IOPOLL))
			goto copy_iov;
done:
		kiocb_done(req, ret2, issue_flags);
	} else {
copy_iov:
		iov_iter_restore(&s->iter, &s->iter_state);
		ret = io_setup_async_rw(req, iovec, s, false);
		return ret ?: -EAGAIN;
	}
out_free:
	/* it's reportedly faster than delegating the null check to kfree() */
	if (iovec)
		kfree(iovec);
	return ret;
}

static int io_renameat_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	struct io_rename *ren = &req->rename;
	const char __user *oldf, *newf;

	if (sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;
	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	ren->old_dfd = READ_ONCE(sqe->fd);
	oldf = u64_to_user_ptr(READ_ONCE(sqe->addr));
	newf = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	ren->new_dfd = READ_ONCE(sqe->len);
	ren->flags = READ_ONCE(sqe->rename_flags);

	ren->oldpath = getname(oldf);
	if (IS_ERR(ren->oldpath))
		return PTR_ERR(ren->oldpath);

	ren->newpath = getname(newf);
	if (IS_ERR(ren->newpath)) {
		putname(ren->oldpath);
		return PTR_ERR(ren->newpath);
	}

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_renameat(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_rename *ren = &req->rename;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = do_renameat2(ren->old_dfd, ren->oldpath, ren->new_dfd,
				ren->newpath, ren->flags);

	req->flags &= ~REQ_F_NEED_CLEANUP;
	io_req_complete(req, ret);
	return 0;
}

static inline void __io_xattr_finish(struct io_kiocb *req)
{
	struct io_xattr *ix = &req->xattr;

	if (ix->filename)
		putname(ix->filename);

	kfree(ix->ctx.kname);
	kvfree(ix->ctx.kvalue);
}

static void io_xattr_finish(struct io_kiocb *req, int ret)
{
	req->flags &= ~REQ_F_NEED_CLEANUP;

	__io_xattr_finish(req);
	io_req_complete(req, ret);
}

static int __io_getxattr_prep(struct io_kiocb *req,
			      const struct io_uring_sqe *sqe)
{
	struct io_xattr *ix = &req->xattr;
	const char __user *name;
	int ret;

	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	ix->filename = NULL;
	ix->ctx.kvalue = NULL;
	name = u64_to_user_ptr(READ_ONCE(sqe->addr));
	ix->ctx.cvalue = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	ix->ctx.size = READ_ONCE(sqe->len);
	ix->ctx.flags = READ_ONCE(sqe->xattr_flags);

	if (ix->ctx.flags)
		return -EINVAL;

	ix->ctx.kname = kmalloc(sizeof(*ix->ctx.kname), GFP_KERNEL);
	if (!ix->ctx.kname)
		return -ENOMEM;

	ret = strncpy_from_user(ix->ctx.kname->name, name,
				sizeof(ix->ctx.kname->name));
	if (!ret || ret == sizeof(ix->ctx.kname->name))
		ret = -ERANGE;
	if (ret < 0) {
		kfree(ix->ctx.kname);
		return ret;
	}

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_fgetxattr_prep(struct io_kiocb *req,
			     const struct io_uring_sqe *sqe)
{
	return __io_getxattr_prep(req, sqe);
}

static int io_getxattr_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	struct io_xattr *ix = &req->xattr;
	const char __user *path;
	int ret;

	ret = __io_getxattr_prep(req, sqe);
	if (ret)
		return ret;

	path = u64_to_user_ptr(READ_ONCE(sqe->addr3));

	ix->filename = getname_flags(path, LOOKUP_FOLLOW, NULL);
	if (IS_ERR(ix->filename)) {
		ret = PTR_ERR(ix->filename);
		ix->filename = NULL;
	}

	return ret;
}

static int io_fgetxattr(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_xattr *ix = &req->xattr;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = do_getxattr(mnt_user_ns(req->file->f_path.mnt),
			req->file->f_path.dentry,
			&ix->ctx);

	io_xattr_finish(req, ret);
	return 0;
}

static int io_getxattr(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_xattr *ix = &req->xattr;
	unsigned int lookup_flags = LOOKUP_FOLLOW;
	struct path path;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

retry:
	ret = filename_lookup(AT_FDCWD, ix->filename, lookup_flags, &path, NULL);
	if (!ret) {
		ret = do_getxattr(mnt_user_ns(path.mnt),
				path.dentry,
				&ix->ctx);

		path_put(&path);
		if (retry_estale(ret, lookup_flags)) {
			lookup_flags |= LOOKUP_REVAL;
			goto retry;
		}
	}

	io_xattr_finish(req, ret);
	return 0;
}

static int __io_setxattr_prep(struct io_kiocb *req,
			const struct io_uring_sqe *sqe)
{
	struct io_xattr *ix = &req->xattr;
	const char __user *name;
	int ret;

	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	ix->filename = NULL;
	name = u64_to_user_ptr(READ_ONCE(sqe->addr));
	ix->ctx.cvalue = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	ix->ctx.kvalue = NULL;
	ix->ctx.size = READ_ONCE(sqe->len);
	ix->ctx.flags = READ_ONCE(sqe->xattr_flags);

	ix->ctx.kname = kmalloc(sizeof(*ix->ctx.kname), GFP_KERNEL);
	if (!ix->ctx.kname)
		return -ENOMEM;

	ret = setxattr_copy(name, &ix->ctx);
	if (ret) {
		kfree(ix->ctx.kname);
		return ret;
	}

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_setxattr_prep(struct io_kiocb *req,
			const struct io_uring_sqe *sqe)
{
	struct io_xattr *ix = &req->xattr;
	const char __user *path;
	int ret;

	ret = __io_setxattr_prep(req, sqe);
	if (ret)
		return ret;

	path = u64_to_user_ptr(READ_ONCE(sqe->addr3));

	ix->filename = getname_flags(path, LOOKUP_FOLLOW, NULL);
	if (IS_ERR(ix->filename)) {
		ret = PTR_ERR(ix->filename);
		ix->filename = NULL;
	}

	return ret;
}

static int io_fsetxattr_prep(struct io_kiocb *req,
			const struct io_uring_sqe *sqe)
{
	return __io_setxattr_prep(req, sqe);
}

static int __io_setxattr(struct io_kiocb *req, unsigned int issue_flags,
			struct path *path)
{
	struct io_xattr *ix = &req->xattr;
	int ret;

	ret = mnt_want_write(path->mnt);
	if (!ret) {
		ret = do_setxattr(mnt_user_ns(path->mnt), path->dentry, &ix->ctx);
		mnt_drop_write(path->mnt);
	}

	return ret;
}

static int io_fsetxattr(struct io_kiocb *req, unsigned int issue_flags)
{
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = __io_setxattr(req, issue_flags, &req->file->f_path);
	io_xattr_finish(req, ret);

	return 0;
}

static int io_setxattr(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_xattr *ix = &req->xattr;
	unsigned int lookup_flags = LOOKUP_FOLLOW;
	struct path path;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

retry:
	ret = filename_lookup(AT_FDCWD, ix->filename, lookup_flags, &path, NULL);
	if (!ret) {
		ret = __io_setxattr(req, issue_flags, &path);
		path_put(&path);
		if (retry_estale(ret, lookup_flags)) {
			lookup_flags |= LOOKUP_REVAL;
			goto retry;
		}
	}

	io_xattr_finish(req, ret);
	return 0;
}

static int io_unlinkat_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	struct io_unlink *un = &req->unlink;
	const char __user *fname;

	if (sqe->off || sqe->len || sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;
	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	un->dfd = READ_ONCE(sqe->fd);

	un->flags = READ_ONCE(sqe->unlink_flags);
	if (un->flags & ~AT_REMOVEDIR)
		return -EINVAL;

	fname = u64_to_user_ptr(READ_ONCE(sqe->addr));
	un->filename = getname(fname);
	if (IS_ERR(un->filename))
		return PTR_ERR(un->filename);

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_unlinkat(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_unlink *un = &req->unlink;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	if (un->flags & AT_REMOVEDIR)
		ret = do_rmdir(un->dfd, un->filename);
	else
		ret = do_unlinkat(un->dfd, un->filename);

	req->flags &= ~REQ_F_NEED_CLEANUP;
	io_req_complete(req, ret);
	return 0;
}

static int io_mkdirat_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	struct io_mkdir *mkd = &req->mkdir;
	const char __user *fname;

	if (sqe->off || sqe->rw_flags || sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;
	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	mkd->dfd = READ_ONCE(sqe->fd);
	mkd->mode = READ_ONCE(sqe->len);

	fname = u64_to_user_ptr(READ_ONCE(sqe->addr));
	mkd->filename = getname(fname);
	if (IS_ERR(mkd->filename))
		return PTR_ERR(mkd->filename);

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_mkdirat(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_mkdir *mkd = &req->mkdir;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = do_mkdirat(mkd->dfd, mkd->filename, mkd->mode);

	req->flags &= ~REQ_F_NEED_CLEANUP;
	io_req_complete(req, ret);
	return 0;
}

static int io_symlinkat_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	struct io_symlink *sl = &req->symlink;
	const char __user *oldpath, *newpath;

	if (sqe->len || sqe->rw_flags || sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;
	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	sl->new_dfd = READ_ONCE(sqe->fd);
	oldpath = u64_to_user_ptr(READ_ONCE(sqe->addr));
	newpath = u64_to_user_ptr(READ_ONCE(sqe->addr2));

	sl->oldpath = getname(oldpath);
	if (IS_ERR(sl->oldpath))
		return PTR_ERR(sl->oldpath);

	sl->newpath = getname(newpath);
	if (IS_ERR(sl->newpath)) {
		putname(sl->oldpath);
		return PTR_ERR(sl->newpath);
	}

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_symlinkat(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_symlink *sl = &req->symlink;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = do_symlinkat(sl->oldpath, sl->new_dfd, sl->newpath);

	req->flags &= ~REQ_F_NEED_CLEANUP;
	io_req_complete(req, ret);
	return 0;
}

static int io_linkat_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	struct io_hardlink *lnk = &req->hardlink;
	const char __user *oldf, *newf;

	if (sqe->rw_flags || sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;
	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	lnk->old_dfd = READ_ONCE(sqe->fd);
	lnk->new_dfd = READ_ONCE(sqe->len);
	oldf = u64_to_user_ptr(READ_ONCE(sqe->addr));
	newf = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	lnk->flags = READ_ONCE(sqe->hardlink_flags);

	lnk->oldpath = getname(oldf);
	if (IS_ERR(lnk->oldpath))
		return PTR_ERR(lnk->oldpath);

	lnk->newpath = getname(newf);
	if (IS_ERR(lnk->newpath)) {
		putname(lnk->oldpath);
		return PTR_ERR(lnk->newpath);
	}

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_linkat(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_hardlink *lnk = &req->hardlink;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = do_linkat(lnk->old_dfd, lnk->oldpath, lnk->new_dfd,
				lnk->newpath, lnk->flags);

	req->flags &= ~REQ_F_NEED_CLEANUP;
	io_req_complete(req, ret);
	return 0;
}

static void io_uring_cmd_work(struct io_kiocb *req, bool *locked)
{
	req->uring_cmd.task_work_cb(&req->uring_cmd);
}

void io_uring_cmd_complete_in_task(struct io_uring_cmd *ioucmd,
			void (*task_work_cb)(struct io_uring_cmd *))
{
	struct io_kiocb *req = container_of(ioucmd, struct io_kiocb, uring_cmd);

	req->uring_cmd.task_work_cb = task_work_cb;
	req->io_task_work.func = io_uring_cmd_work;
	io_req_task_work_add(req);
}
EXPORT_SYMBOL_GPL(io_uring_cmd_complete_in_task);

static inline void io_req_set_cqe32_extra(struct io_kiocb *req,
					  u64 extra1, u64 extra2)
{
	req->extra1 = extra1;
	req->extra2 = extra2;
	req->flags |= REQ_F_CQE32_INIT;
}

/*
 * Called by consumers of io_uring_cmd, if they originally returned
 * -EIOCBQUEUED upon receiving the command.
 */
void io_uring_cmd_done(struct io_uring_cmd *ioucmd, ssize_t ret, ssize_t res2)
{
	struct io_kiocb *req = container_of(ioucmd, struct io_kiocb, uring_cmd);

	if (ret < 0)
		req_set_fail(req);

	if (req->ctx->flags & IORING_SETUP_CQE32)
		io_req_set_cqe32_extra(req, res2, 0);
	io_req_complete(req, ret);
}
EXPORT_SYMBOL_GPL(io_uring_cmd_done);

static int io_uring_cmd_prep_async(struct io_kiocb *req)
{
	size_t cmd_size;

	cmd_size = uring_cmd_pdu_size(req->ctx->flags & IORING_SETUP_SQE128);

	memcpy(req->async_data, req->uring_cmd.cmd, cmd_size);
	return 0;
}

static int io_uring_cmd_prep(struct io_kiocb *req,
			     const struct io_uring_sqe *sqe)
{
	struct io_uring_cmd *ioucmd = &req->uring_cmd;

	if (sqe->rw_flags)
		return -EINVAL;
	ioucmd->cmd = sqe->cmd;
	ioucmd->cmd_op = READ_ONCE(sqe->cmd_op);
	return 0;
}

static int io_uring_cmd(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_uring_cmd *ioucmd = &req->uring_cmd;
	struct io_ring_ctx *ctx = req->ctx;
	struct file *file = req->file;
	int ret;

	if (!req->file->f_op->uring_cmd)
		return -EOPNOTSUPP;

	if (ctx->flags & IORING_SETUP_SQE128)
		issue_flags |= IO_URING_F_SQE128;
	if (ctx->flags & IORING_SETUP_CQE32)
		issue_flags |= IO_URING_F_CQE32;
	if (ctx->flags & IORING_SETUP_IOPOLL)
		issue_flags |= IO_URING_F_IOPOLL;

	if (req_has_async_data(req))
		ioucmd->cmd = req->async_data;

	ret = file->f_op->uring_cmd(ioucmd, issue_flags);
	if (ret == -EAGAIN) {
		if (!req_has_async_data(req)) {
			if (io_alloc_async_data(req))
				return -ENOMEM;
			io_uring_cmd_prep_async(req);
		}
		return -EAGAIN;
	}

	if (ret != -EIOCBQUEUED)
		io_uring_cmd_done(ioucmd, ret, 0);
	return 0;
}

static int __io_splice_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	struct io_splice *sp = &req->splice;
	unsigned int valid_flags = SPLICE_F_FD_IN_FIXED | SPLICE_F_ALL;

	sp->len = READ_ONCE(sqe->len);
	sp->flags = READ_ONCE(sqe->splice_flags);
	if (unlikely(sp->flags & ~valid_flags))
		return -EINVAL;
	sp->splice_fd_in = READ_ONCE(sqe->splice_fd_in);
	return 0;
}

static int io_tee_prep(struct io_kiocb *req,
		       const struct io_uring_sqe *sqe)
{
	if (READ_ONCE(sqe->splice_off_in) || READ_ONCE(sqe->off))
		return -EINVAL;
	return __io_splice_prep(req, sqe);
}

static int io_tee(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_splice *sp = &req->splice;
	struct file *out = sp->file_out;
	unsigned int flags = sp->flags & ~SPLICE_F_FD_IN_FIXED;
	struct file *in;
	long ret = 0;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	if (sp->flags & SPLICE_F_FD_IN_FIXED)
		in = io_file_get_fixed(req, sp->splice_fd_in, issue_flags);
	else
		in = io_file_get_normal(req, sp->splice_fd_in);
	if (!in) {
		ret = -EBADF;
		goto done;
	}

	if (sp->len)
		ret = do_tee(in, out, sp->len, flags);

	if (!(sp->flags & SPLICE_F_FD_IN_FIXED))
		io_put_file(in);
done:
	if (ret != sp->len)
		req_set_fail(req);
	__io_req_complete(req, 0, ret, 0);
	return 0;
}

static int io_splice_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_splice *sp = &req->splice;

	sp->off_in = READ_ONCE(sqe->splice_off_in);
	sp->off_out = READ_ONCE(sqe->off);
	return __io_splice_prep(req, sqe);
}

static int io_splice(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_splice *sp = &req->splice;
	struct file *out = sp->file_out;
	unsigned int flags = sp->flags & ~SPLICE_F_FD_IN_FIXED;
	loff_t *poff_in, *poff_out;
	struct file *in;
	long ret = 0;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	if (sp->flags & SPLICE_F_FD_IN_FIXED)
		in = io_file_get_fixed(req, sp->splice_fd_in, issue_flags);
	else
		in = io_file_get_normal(req, sp->splice_fd_in);
	if (!in) {
		ret = -EBADF;
		goto done;
	}

	poff_in = (sp->off_in == -1) ? NULL : &sp->off_in;
	poff_out = (sp->off_out == -1) ? NULL : &sp->off_out;

	if (sp->len)
		ret = do_splice(in, poff_in, out, poff_out, sp->len, flags);

	if (!(sp->flags & SPLICE_F_FD_IN_FIXED))
		io_put_file(in);
done:
	if (ret != sp->len)
		req_set_fail(req);
	__io_req_complete(req, 0, ret, 0);
	return 0;
}

static int io_nop_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	return 0;
}

/*
 * IORING_OP_NOP just posts a completion event, nothing else.
 */
static int io_nop(struct io_kiocb *req, unsigned int issue_flags)
{
	__io_req_complete(req, issue_flags, 0, 0);
	return 0;
}

static int io_msg_ring_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	if (unlikely(sqe->addr || sqe->rw_flags || sqe->splice_fd_in ||
		     sqe->buf_index || sqe->personality))
		return -EINVAL;

	req->msg.user_data = READ_ONCE(sqe->off);
	req->msg.len = READ_ONCE(sqe->len);
	return 0;
}

static int io_msg_ring(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *target_ctx;
	struct io_msg *msg = &req->msg;
	bool filled;
	int ret;

	ret = -EBADFD;
	if (req->file->f_op != &io_uring_fops)
		goto done;

	ret = -EOVERFLOW;
	target_ctx = req->file->private_data;

	spin_lock(&target_ctx->completion_lock);
	filled = io_fill_cqe_aux(target_ctx, msg->user_data, msg->len, 0);
	io_commit_cqring(target_ctx);
	spin_unlock(&target_ctx->completion_lock);

	if (filled) {
		io_cqring_ev_posted(target_ctx);
		ret = 0;
	}

done:
	if (ret < 0)
		req_set_fail(req);
	__io_req_complete(req, issue_flags, ret, 0);
	/* put file to avoid an attempt to IOPOLL the req */
	io_put_file(req->file);
	req->file = NULL;
	return 0;
}

static int io_fsync_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	if (unlikely(sqe->addr || sqe->buf_index || sqe->splice_fd_in))
		return -EINVAL;

	req->sync.flags = READ_ONCE(sqe->fsync_flags);
	if (unlikely(req->sync.flags & ~IORING_FSYNC_DATASYNC))
		return -EINVAL;

	req->sync.off = READ_ONCE(sqe->off);
	req->sync.len = READ_ONCE(sqe->len);
	return 0;
}

static int io_fsync(struct io_kiocb *req, unsigned int issue_flags)
{
	loff_t end = req->sync.off + req->sync.len;
	int ret;

	/* fsync always requires a blocking context */
	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = vfs_fsync_range(req->file, req->sync.off,
				end > 0 ? end : LLONG_MAX,
				req->sync.flags & IORING_FSYNC_DATASYNC);
	io_req_complete(req, ret);
	return 0;
}

static int io_fallocate_prep(struct io_kiocb *req,
			     const struct io_uring_sqe *sqe)
{
	if (sqe->buf_index || sqe->rw_flags || sqe->splice_fd_in)
		return -EINVAL;

	req->sync.off = READ_ONCE(sqe->off);
	req->sync.len = READ_ONCE(sqe->addr);
	req->sync.mode = READ_ONCE(sqe->len);
	return 0;
}

static int io_fallocate(struct io_kiocb *req, unsigned int issue_flags)
{
	int ret;

	/* fallocate always requiring blocking context */
	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;
	ret = vfs_fallocate(req->file, req->sync.mode, req->sync.off,
				req->sync.len);
	if (ret >= 0)
		fsnotify_modify(req->file);
	io_req_complete(req, ret);
	return 0;
}

static int __io_openat_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	const char __user *fname;
	int ret;

	if (unlikely(sqe->buf_index))
		return -EINVAL;
	if (unlikely(req->flags & REQ_F_FIXED_FILE))
		return -EBADF;

	/* open.how should be already initialised */
	if (!(req->open.how.flags & O_PATH) && force_o_largefile())
		req->open.how.flags |= O_LARGEFILE;

	req->open.dfd = READ_ONCE(sqe->fd);
	fname = u64_to_user_ptr(READ_ONCE(sqe->addr));
	req->open.filename = getname(fname);
	if (IS_ERR(req->open.filename)) {
		ret = PTR_ERR(req->open.filename);
		req->open.filename = NULL;
		return ret;
	}

	req->open.file_slot = READ_ONCE(sqe->file_index);
	if (req->open.file_slot && (req->open.how.flags & O_CLOEXEC))
		return -EINVAL;

	req->open.nofile = rlimit(RLIMIT_NOFILE);
	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_openat_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	u64 mode = READ_ONCE(sqe->len);
	u64 flags = READ_ONCE(sqe->open_flags);

	req->open.how = build_open_how(flags, mode);
	return __io_openat_prep(req, sqe);
}

static int io_openat2_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct open_how __user *how;
	size_t len;
	int ret;

	how = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	len = READ_ONCE(sqe->len);
	if (len < OPEN_HOW_SIZE_VER0)
		return -EINVAL;

	ret = copy_struct_from_user(&req->open.how, sizeof(req->open.how), how,
					len);
	if (ret)
		return ret;

	return __io_openat_prep(req, sqe);
}

static int io_file_bitmap_get(struct io_ring_ctx *ctx)
{
	struct io_file_table *table = &ctx->file_table;
	unsigned long nr = ctx->nr_user_files;
	int ret;

	do {
		ret = find_next_zero_bit(table->bitmap, nr, table->alloc_hint);
		if (ret != nr)
			return ret;

		if (!table->alloc_hint)
			break;

		nr = table->alloc_hint;
		table->alloc_hint = 0;
	} while (1);

	return -ENFILE;
}

/*
 * Note when io_fixed_fd_install() returns error value, it will ensure
 * fput() is called correspondingly.
 */
static int io_fixed_fd_install(struct io_kiocb *req, unsigned int issue_flags,
			       struct file *file, unsigned int file_slot)
{
	bool alloc_slot = file_slot == IORING_FILE_INDEX_ALLOC;
	struct io_ring_ctx *ctx = req->ctx;
	int ret;

	io_ring_submit_lock(ctx, issue_flags);

	if (alloc_slot) {
		ret = io_file_bitmap_get(ctx);
		if (unlikely(ret < 0))
			goto err;
		file_slot = ret;
	} else {
		file_slot--;
	}

	ret = io_install_fixed_file(req, file, issue_flags, file_slot);
	if (!ret && alloc_slot)
		ret = file_slot;
err:
	io_ring_submit_unlock(ctx, issue_flags);
	if (unlikely(ret < 0))
		fput(file);
	return ret;
}

static int io_openat2(struct io_kiocb *req, unsigned int issue_flags)
{
	struct open_flags op;
	struct file *file;
	bool resolve_nonblock, nonblock_set;
	bool fixed = !!req->open.file_slot;
	int ret;

	ret = build_open_flags(&req->open.how, &op);
	if (ret)
		goto err;
	nonblock_set = op.open_flag & O_NONBLOCK;
	resolve_nonblock = req->open.how.resolve & RESOLVE_CACHED;
	if (issue_flags & IO_URING_F_NONBLOCK) {
		/*
		 * Don't bother trying for O_TRUNC, O_CREAT, or O_TMPFILE open,
		 * it'll always -EAGAIN
		 */
		if (req->open.how.flags & (O_TRUNC | O_CREAT | O_TMPFILE))
			return -EAGAIN;
		op.lookup_flags |= LOOKUP_CACHED;
		op.open_flag |= O_NONBLOCK;
	}

	if (!fixed) {
		ret = __get_unused_fd_flags(req->open.how.flags, req->open.nofile);
		if (ret < 0)
			goto err;
	}

	file = do_filp_open(req->open.dfd, req->open.filename, &op);
	if (IS_ERR(file)) {
		/*
		 * We could hang on to this 'fd' on retrying, but seems like
		 * marginal gain for something that is now known to be a slower
		 * path. So just put it, and we'll get a new one when we retry.
		 */
		if (!fixed)
			put_unused_fd(ret);

		ret = PTR_ERR(file);
		/* only retry if RESOLVE_CACHED wasn't already set by application */
		if (ret == -EAGAIN &&
		    (!resolve_nonblock && (issue_flags & IO_URING_F_NONBLOCK)))
			return -EAGAIN;
		goto err;
	}

	if ((issue_flags & IO_URING_F_NONBLOCK) && !nonblock_set)
		file->f_flags &= ~O_NONBLOCK;
	fsnotify_open(file);

	if (!fixed)
		fd_install(ret, file);
	else
		ret = io_fixed_fd_install(req, issue_flags, file,
						req->open.file_slot);
err:
	putname(req->open.filename);
	req->flags &= ~REQ_F_NEED_CLEANUP;
	if (ret < 0)
		req_set_fail(req);
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int io_openat(struct io_kiocb *req, unsigned int issue_flags)
{
	return io_openat2(req, issue_flags);
}

static int io_remove_buffers_prep(struct io_kiocb *req,
				  const struct io_uring_sqe *sqe)
{
	struct io_provide_buf *p = &req->pbuf;
	u64 tmp;

	if (sqe->rw_flags || sqe->addr || sqe->len || sqe->off ||
	    sqe->splice_fd_in)
		return -EINVAL;

	tmp = READ_ONCE(sqe->fd);
	if (!tmp || tmp > USHRT_MAX)
		return -EINVAL;

	memset(p, 0, sizeof(*p));
	p->nbufs = tmp;
	p->bgid = READ_ONCE(sqe->buf_group);
	return 0;
}

static int __io_remove_buffers(struct io_ring_ctx *ctx,
			       struct io_buffer_list *bl, unsigned nbufs)
{
	unsigned i = 0;

	/* shouldn't happen */
	if (!nbufs)
		return 0;

	if (bl->buf_nr_pages) {
		int j;

		i = bl->buf_ring->tail - bl->head;
		for (j = 0; j < bl->buf_nr_pages; j++)
			unpin_user_page(bl->buf_pages[j]);
		kvfree(bl->buf_pages);
		bl->buf_pages = NULL;
		bl->buf_nr_pages = 0;
		/* make sure it's seen as empty */
		INIT_LIST_HEAD(&bl->buf_list);
		return i;
	}

	/* the head kbuf is the list itself */
	while (!list_empty(&bl->buf_list)) {
		struct io_buffer *nxt;

		nxt = list_first_entry(&bl->buf_list, struct io_buffer, list);
		list_del(&nxt->list);
		if (++i == nbufs)
			return i;
		cond_resched();
	}
	i++;

	return i;
}

static int io_remove_buffers(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_provide_buf *p = &req->pbuf;
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	int ret = 0;

	io_ring_submit_lock(ctx, issue_flags);

	ret = -ENOENT;
	bl = io_buffer_get_list(ctx, p->bgid);
	if (bl) {
		ret = -EINVAL;
		/* can't use provide/remove buffers command on mapped buffers */
		if (!bl->buf_nr_pages)
			ret = __io_remove_buffers(ctx, bl, p->nbufs);
	}
	if (ret < 0)
		req_set_fail(req);

	/* complete before unlock, IOPOLL may need the lock */
	__io_req_complete(req, issue_flags, ret, 0);
	io_ring_submit_unlock(ctx, issue_flags);
	return 0;
}

static int io_provide_buffers_prep(struct io_kiocb *req,
				   const struct io_uring_sqe *sqe)
{
	unsigned long size, tmp_check;
	struct io_provide_buf *p = &req->pbuf;
	u64 tmp;

	if (sqe->rw_flags || sqe->splice_fd_in)
		return -EINVAL;

	tmp = READ_ONCE(sqe->fd);
	if (!tmp || tmp > USHRT_MAX)
		return -E2BIG;
	p->nbufs = tmp;
	p->addr = READ_ONCE(sqe->addr);
	p->len = READ_ONCE(sqe->len);

	if (check_mul_overflow((unsigned long)p->len, (unsigned long)p->nbufs,
				&size))
		return -EOVERFLOW;
	if (check_add_overflow((unsigned long)p->addr, size, &tmp_check))
		return -EOVERFLOW;

	size = (unsigned long)p->len * p->nbufs;
	if (!access_ok(u64_to_user_ptr(p->addr), size))
		return -EFAULT;

	p->bgid = READ_ONCE(sqe->buf_group);
	tmp = READ_ONCE(sqe->off);
	if (tmp > USHRT_MAX)
		return -E2BIG;
	p->bid = tmp;
	return 0;
}

static int io_refill_buffer_cache(struct io_ring_ctx *ctx)
{
	struct io_buffer *buf;
	struct page *page;
	int bufs_in_page;

	/*
	 * Completions that don't happen inline (eg not under uring_lock) will
	 * add to ->io_buffers_comp. If we don't have any free buffers, check
	 * the completion list and splice those entries first.
	 */
	if (!list_empty_careful(&ctx->io_buffers_comp)) {
		spin_lock(&ctx->completion_lock);
		if (!list_empty(&ctx->io_buffers_comp)) {
			list_splice_init(&ctx->io_buffers_comp,
						&ctx->io_buffers_cache);
			spin_unlock(&ctx->completion_lock);
			return 0;
		}
		spin_unlock(&ctx->completion_lock);
	}

	/*
	 * No free buffers and no completion entries either. Allocate a new
	 * page worth of buffer entries and add those to our freelist.
	 */
	page = alloc_page(GFP_KERNEL_ACCOUNT);
	if (!page)
		return -ENOMEM;

	list_add(&page->lru, &ctx->io_buffers_pages);

	buf = page_address(page);
	bufs_in_page = PAGE_SIZE / sizeof(*buf);
	while (bufs_in_page) {
		list_add_tail(&buf->list, &ctx->io_buffers_cache);
		buf++;
		bufs_in_page--;
	}

	return 0;
}

static int io_add_buffers(struct io_ring_ctx *ctx, struct io_provide_buf *pbuf,
			  struct io_buffer_list *bl)
{
	struct io_buffer *buf;
	u64 addr = pbuf->addr;
	int i, bid = pbuf->bid;

	for (i = 0; i < pbuf->nbufs; i++) {
		if (list_empty(&ctx->io_buffers_cache) &&
		    io_refill_buffer_cache(ctx))
			break;
		buf = list_first_entry(&ctx->io_buffers_cache, struct io_buffer,
					list);
		list_move_tail(&buf->list, &bl->buf_list);
		buf->addr = addr;
		buf->len = min_t(__u32, pbuf->len, MAX_RW_COUNT);
		buf->bid = bid;
		buf->bgid = pbuf->bgid;
		addr += pbuf->len;
		bid++;
		cond_resched();
	}

	return i ? 0 : -ENOMEM;
}

static __cold int io_init_bl_list(struct io_ring_ctx *ctx)
{
	int i;

	ctx->io_bl = kcalloc(BGID_ARRAY, sizeof(struct io_buffer_list),
				GFP_KERNEL);
	if (!ctx->io_bl)
		return -ENOMEM;

	for (i = 0; i < BGID_ARRAY; i++) {
		INIT_LIST_HEAD(&ctx->io_bl[i].buf_list);
		ctx->io_bl[i].bgid = i;
	}

	return 0;
}

static int io_provide_buffers(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_provide_buf *p = &req->pbuf;
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	int ret = 0;

	io_ring_submit_lock(ctx, issue_flags);

	if (unlikely(p->bgid < BGID_ARRAY && !ctx->io_bl)) {
		ret = io_init_bl_list(ctx);
		if (ret)
			goto err;
	}

	bl = io_buffer_get_list(ctx, p->bgid);
	if (unlikely(!bl)) {
		bl = kzalloc(sizeof(*bl), GFP_KERNEL);
		if (!bl) {
			ret = -ENOMEM;
			goto err;
		}
		INIT_LIST_HEAD(&bl->buf_list);
		ret = io_buffer_add_list(ctx, bl, p->bgid);
		if (ret) {
			kfree(bl);
			goto err;
		}
	}
	/* can't add buffers via this command for a mapped buffer ring */
	if (bl->buf_nr_pages) {
		ret = -EINVAL;
		goto err;
	}

	ret = io_add_buffers(ctx, p, bl);
err:
	if (ret < 0)
		req_set_fail(req);
	/* complete before unlock, IOPOLL may need the lock */
	__io_req_complete(req, issue_flags, ret, 0);
	io_ring_submit_unlock(ctx, issue_flags);
	return 0;
}

static int io_epoll_ctl_prep(struct io_kiocb *req,
			     const struct io_uring_sqe *sqe)
{
#if defined(CONFIG_EPOLL)
	if (sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;

	req->epoll.epfd = READ_ONCE(sqe->fd);
	req->epoll.op = READ_ONCE(sqe->len);
	req->epoll.fd = READ_ONCE(sqe->off);

	if (ep_op_has_event(req->epoll.op)) {
		struct epoll_event __user *ev;

		ev = u64_to_user_ptr(READ_ONCE(sqe->addr));
		if (copy_from_user(&req->epoll.event, ev, sizeof(*ev)))
			return -EFAULT;
	}

	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static int io_epoll_ctl(struct io_kiocb *req, unsigned int issue_flags)
{
#if defined(CONFIG_EPOLL)
	struct io_epoll *ie = &req->epoll;
	int ret;
	bool force_nonblock = issue_flags & IO_URING_F_NONBLOCK;

	ret = do_epoll_ctl(ie->epfd, ie->op, ie->fd, &ie->event, force_nonblock);
	if (force_nonblock && ret == -EAGAIN)
		return -EAGAIN;

	if (ret < 0)
		req_set_fail(req);
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static int io_madvise_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
#if defined(CONFIG_ADVISE_SYSCALLS) && defined(CONFIG_MMU)
	if (sqe->buf_index || sqe->off || sqe->splice_fd_in)
		return -EINVAL;

	req->madvise.addr = READ_ONCE(sqe->addr);
	req->madvise.len = READ_ONCE(sqe->len);
	req->madvise.advice = READ_ONCE(sqe->fadvise_advice);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static int io_madvise(struct io_kiocb *req, unsigned int issue_flags)
{
#if defined(CONFIG_ADVISE_SYSCALLS) && defined(CONFIG_MMU)
	struct io_madvise *ma = &req->madvise;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = do_madvise(current->mm, ma->addr, ma->len, ma->advice);
	io_req_complete(req, ret);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static int io_fadvise_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	if (sqe->buf_index || sqe->addr || sqe->splice_fd_in)
		return -EINVAL;

	req->fadvise.offset = READ_ONCE(sqe->off);
	req->fadvise.len = READ_ONCE(sqe->len);
	req->fadvise.advice = READ_ONCE(sqe->fadvise_advice);
	return 0;
}

static int io_fadvise(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_fadvise *fa = &req->fadvise;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK) {
		switch (fa->advice) {
		case POSIX_FADV_NORMAL:
		case POSIX_FADV_RANDOM:
		case POSIX_FADV_SEQUENTIAL:
			break;
		default:
			return -EAGAIN;
		}
	}

	ret = vfs_fadvise(req->file, fa->offset, fa->len, fa->advice);
	if (ret < 0)
		req_set_fail(req);
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int io_statx_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	const char __user *path;

	if (sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;
	if (req->flags & REQ_F_FIXED_FILE)
		return -EBADF;

	req->statx.dfd = READ_ONCE(sqe->fd);
	req->statx.mask = READ_ONCE(sqe->len);
	path = u64_to_user_ptr(READ_ONCE(sqe->addr));
	req->statx.buffer = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	req->statx.flags = READ_ONCE(sqe->statx_flags);

	req->statx.filename = getname_flags(path,
					getname_statx_lookup_flags(req->statx.flags),
					NULL);

	if (IS_ERR(req->statx.filename)) {
		int ret = PTR_ERR(req->statx.filename);

		req->statx.filename = NULL;
		return ret;
	}

	req->flags |= REQ_F_NEED_CLEANUP;
	return 0;
}

static int io_statx(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_statx *ctx = &req->statx;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = do_statx(ctx->dfd, ctx->filename, ctx->flags, ctx->mask,
		       ctx->buffer);
	io_req_complete(req, ret);
	return 0;
}

static int io_close_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	if (sqe->off || sqe->addr || sqe->len || sqe->rw_flags || sqe->buf_index)
		return -EINVAL;
	if (req->flags & REQ_F_FIXED_FILE)
		return -EBADF;

	req->close.fd = READ_ONCE(sqe->fd);
	req->close.file_slot = READ_ONCE(sqe->file_index);
	if (req->close.file_slot && req->close.fd)
		return -EINVAL;

	return 0;
}

static int io_close(struct io_kiocb *req, unsigned int issue_flags)
{
	struct files_struct *files = current->files;
	struct io_close *close = &req->close;
	struct fdtable *fdt;
	struct file *file;
	int ret = -EBADF;

	if (req->close.file_slot) {
		ret = io_close_fixed(req, issue_flags);
		goto err;
	}

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	if (close->fd >= fdt->max_fds) {
		spin_unlock(&files->file_lock);
		goto err;
	}
	file = rcu_dereference_protected(fdt->fd[close->fd],
			lockdep_is_held(&files->file_lock));
	if (!file || file->f_op == &io_uring_fops) {
		spin_unlock(&files->file_lock);
		goto err;
	}

	/* if the file has a flush method, be safe and punt to async */
	if (file->f_op->flush && (issue_flags & IO_URING_F_NONBLOCK)) {
		spin_unlock(&files->file_lock);
		return -EAGAIN;
	}

	file = __close_fd_get_file(close->fd);
	spin_unlock(&files->file_lock);
	if (!file)
		goto err;

	/* No ->flush() or already async, safely close from here */
	ret = filp_close(file, current->files);
err:
	if (ret < 0)
		req_set_fail(req);
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int io_sfr_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	if (unlikely(sqe->addr || sqe->buf_index || sqe->splice_fd_in))
		return -EINVAL;

	req->sync.off = READ_ONCE(sqe->off);
	req->sync.len = READ_ONCE(sqe->len);
	req->sync.flags = READ_ONCE(sqe->sync_range_flags);
	return 0;
}

static int io_sync_file_range(struct io_kiocb *req, unsigned int issue_flags)
{
	int ret;

	/* sync_file_range always requires a blocking context */
	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	ret = sync_file_range(req->file, req->sync.off, req->sync.len,
				req->sync.flags);
	io_req_complete(req, ret);
	return 0;
}

#if defined(CONFIG_NET)
static int io_shutdown_prep(struct io_kiocb *req,
			    const struct io_uring_sqe *sqe)
{
	if (unlikely(sqe->off || sqe->addr || sqe->rw_flags ||
		     sqe->buf_index || sqe->splice_fd_in))
		return -EINVAL;

	req->shutdown.how = READ_ONCE(sqe->len);
	return 0;
}

static int io_shutdown(struct io_kiocb *req, unsigned int issue_flags)
{
	struct socket *sock;
	int ret;

	if (issue_flags & IO_URING_F_NONBLOCK)
		return -EAGAIN;

	sock = sock_from_file(req->file);
	if (unlikely(!sock))
		return -ENOTSOCK;

	ret = __sys_shutdown_sock(sock, req->shutdown.how);
	io_req_complete(req, ret);
	return 0;
}

static bool io_net_retry(struct socket *sock, int flags)
{
	if (!(flags & MSG_WAITALL))
		return false;
	return sock->type == SOCK_STREAM || sock->type == SOCK_SEQPACKET;
}

static int io_setup_async_msg(struct io_kiocb *req,
			      struct io_async_msghdr *kmsg)
{
	struct io_async_msghdr *async_msg = req->async_data;

	if (async_msg)
		return -EAGAIN;
	if (io_alloc_async_data(req)) {
		kfree(kmsg->free_iov);
		return -ENOMEM;
	}
	async_msg = req->async_data;
	req->flags |= REQ_F_NEED_CLEANUP;
	memcpy(async_msg, kmsg, sizeof(*kmsg));
	async_msg->msg.msg_name = &async_msg->addr;
	/* if were using fast_iov, set it to the new one */
	if (!async_msg->free_iov)
		async_msg->msg.msg_iter.iov = async_msg->fast_iov;

	return -EAGAIN;
}

static int io_sendmsg_copy_hdr(struct io_kiocb *req,
			       struct io_async_msghdr *iomsg)
{
	iomsg->msg.msg_name = &iomsg->addr;
	iomsg->free_iov = iomsg->fast_iov;
	return sendmsg_copy_msghdr(&iomsg->msg, req->sr_msg.umsg,
				   req->sr_msg.msg_flags, &iomsg->free_iov);
}

static int io_sendmsg_prep_async(struct io_kiocb *req)
{
	int ret;

	ret = io_sendmsg_copy_hdr(req, req->async_data);
	if (!ret)
		req->flags |= REQ_F_NEED_CLEANUP;
	return ret;
}

static int io_sendmsg_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_sr_msg *sr = &req->sr_msg;

	if (unlikely(sqe->file_index))
		return -EINVAL;
	if (unlikely(sqe->addr2 || sqe->file_index))
		return -EINVAL;

	sr->umsg = u64_to_user_ptr(READ_ONCE(sqe->addr));
	sr->len = READ_ONCE(sqe->len);
	sr->flags = READ_ONCE(sqe->addr2);
	if (sr->flags & ~IORING_RECVSEND_POLL_FIRST)
		return -EINVAL;
	sr->msg_flags = READ_ONCE(sqe->msg_flags) | MSG_NOSIGNAL;
	if (sr->msg_flags & MSG_DONTWAIT)
		req->flags |= REQ_F_NOWAIT;

#ifdef CONFIG_COMPAT
	if (req->ctx->compat)
		sr->msg_flags |= MSG_CMSG_COMPAT;
#endif
	sr->done_io = 0;
	return 0;
}

static int io_sendmsg(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_async_msghdr iomsg, *kmsg;
	struct io_sr_msg *sr = &req->sr_msg;
	struct socket *sock;
	unsigned flags;
	int min_ret = 0;
	int ret;

	sock = sock_from_file(req->file);
	if (unlikely(!sock))
		return -ENOTSOCK;

	if (req_has_async_data(req)) {
		kmsg = req->async_data;
	} else {
		ret = io_sendmsg_copy_hdr(req, &iomsg);
		if (ret)
			return ret;
		kmsg = &iomsg;
	}

	if (!(req->flags & REQ_F_POLLED) &&
	    (sr->flags & IORING_RECVSEND_POLL_FIRST))
		return io_setup_async_msg(req, kmsg);

	flags = sr->msg_flags;
	if (issue_flags & IO_URING_F_NONBLOCK)
		flags |= MSG_DONTWAIT;
	if (flags & MSG_WAITALL)
		min_ret = iov_iter_count(&kmsg->msg.msg_iter);

	ret = __sys_sendmsg_sock(sock, &kmsg->msg, flags);

	if (ret < min_ret) {
		if (ret == -EAGAIN && (issue_flags & IO_URING_F_NONBLOCK))
			return io_setup_async_msg(req, kmsg);
		if (ret == -ERESTARTSYS)
			ret = -EINTR;
		if (ret > 0 && io_net_retry(sock, flags)) {
			sr->done_io += ret;
			req->flags |= REQ_F_PARTIAL_IO;
			return io_setup_async_msg(req, kmsg);
		}
		req_set_fail(req);
	}
	/* fast path, check for non-NULL to avoid function call */
	if (kmsg->free_iov)
		kfree(kmsg->free_iov);
	req->flags &= ~REQ_F_NEED_CLEANUP;
	if (ret >= 0)
		ret += sr->done_io;
	else if (sr->done_io)
		ret = sr->done_io;
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int io_send(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_sr_msg *sr = &req->sr_msg;
	struct msghdr msg;
	struct iovec iov;
	struct socket *sock;
	unsigned flags;
	int min_ret = 0;
	int ret;

	if (!(req->flags & REQ_F_POLLED) &&
	    (sr->flags & IORING_RECVSEND_POLL_FIRST))
		return -EAGAIN;

	sock = sock_from_file(req->file);
	if (unlikely(!sock))
		return -ENOTSOCK;

	ret = import_single_range(WRITE, sr->buf, sr->len, &iov, &msg.msg_iter);
	if (unlikely(ret))
		return ret;

	msg.msg_name = NULL;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_namelen = 0;

	flags = sr->msg_flags;
	if (issue_flags & IO_URING_F_NONBLOCK)
		flags |= MSG_DONTWAIT;
	if (flags & MSG_WAITALL)
		min_ret = iov_iter_count(&msg.msg_iter);

	msg.msg_flags = flags;
	ret = sock_sendmsg(sock, &msg);
	if (ret < min_ret) {
		if (ret == -EAGAIN && (issue_flags & IO_URING_F_NONBLOCK))
			return -EAGAIN;
		if (ret == -ERESTARTSYS)
			ret = -EINTR;
		if (ret > 0 && io_net_retry(sock, flags)) {
			sr->len -= ret;
			sr->buf += ret;
			sr->done_io += ret;
			req->flags |= REQ_F_PARTIAL_IO;
			return -EAGAIN;
		}
		req_set_fail(req);
	}
	if (ret >= 0)
		ret += sr->done_io;
	else if (sr->done_io)
		ret = sr->done_io;
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int __io_recvmsg_copy_hdr(struct io_kiocb *req,
				 struct io_async_msghdr *iomsg)
{
	struct io_sr_msg *sr = &req->sr_msg;
	struct iovec __user *uiov;
	size_t iov_len;
	int ret;

	ret = __copy_msghdr_from_user(&iomsg->msg, sr->umsg,
					&iomsg->uaddr, &uiov, &iov_len);
	if (ret)
		return ret;

	if (req->flags & REQ_F_BUFFER_SELECT) {
		if (iov_len > 1)
			return -EINVAL;
		if (copy_from_user(iomsg->fast_iov, uiov, sizeof(*uiov)))
			return -EFAULT;
		sr->len = iomsg->fast_iov[0].iov_len;
		iomsg->free_iov = NULL;
	} else {
		iomsg->free_iov = iomsg->fast_iov;
		ret = __import_iovec(READ, uiov, iov_len, UIO_FASTIOV,
				     &iomsg->free_iov, &iomsg->msg.msg_iter,
				     false);
		if (ret > 0)
			ret = 0;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static int __io_compat_recvmsg_copy_hdr(struct io_kiocb *req,
					struct io_async_msghdr *iomsg)
{
	struct io_sr_msg *sr = &req->sr_msg;
	struct compat_iovec __user *uiov;
	compat_uptr_t ptr;
	compat_size_t len;
	int ret;

	ret = __get_compat_msghdr(&iomsg->msg, sr->umsg_compat, &iomsg->uaddr,
				  &ptr, &len);
	if (ret)
		return ret;

	uiov = compat_ptr(ptr);
	if (req->flags & REQ_F_BUFFER_SELECT) {
		compat_ssize_t clen;

		if (len > 1)
			return -EINVAL;
		if (!access_ok(uiov, sizeof(*uiov)))
			return -EFAULT;
		if (__get_user(clen, &uiov->iov_len))
			return -EFAULT;
		if (clen < 0)
			return -EINVAL;
		sr->len = clen;
		iomsg->free_iov = NULL;
	} else {
		iomsg->free_iov = iomsg->fast_iov;
		ret = __import_iovec(READ, (struct iovec __user *)uiov, len,
				   UIO_FASTIOV, &iomsg->free_iov,
				   &iomsg->msg.msg_iter, true);
		if (ret < 0)
			return ret;
	}

	return 0;
}
#endif

static int io_recvmsg_copy_hdr(struct io_kiocb *req,
			       struct io_async_msghdr *iomsg)
{
	iomsg->msg.msg_name = &iomsg->addr;

#ifdef CONFIG_COMPAT
	if (req->ctx->compat)
		return __io_compat_recvmsg_copy_hdr(req, iomsg);
#endif

	return __io_recvmsg_copy_hdr(req, iomsg);
}

static int io_recvmsg_prep_async(struct io_kiocb *req)
{
	int ret;

	ret = io_recvmsg_copy_hdr(req, req->async_data);
	if (!ret)
		req->flags |= REQ_F_NEED_CLEANUP;
	return ret;
}

static int io_recvmsg_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_sr_msg *sr = &req->sr_msg;

	if (unlikely(sqe->file_index))
		return -EINVAL;
	if (unlikely(sqe->addr2 || sqe->file_index))
		return -EINVAL;

	sr->umsg = u64_to_user_ptr(READ_ONCE(sqe->addr));
	sr->len = READ_ONCE(sqe->len);
	sr->flags = READ_ONCE(sqe->addr2);
	if (sr->flags & ~IORING_RECVSEND_POLL_FIRST)
		return -EINVAL;
	sr->msg_flags = READ_ONCE(sqe->msg_flags) | MSG_NOSIGNAL;
	if (sr->msg_flags & MSG_DONTWAIT)
		req->flags |= REQ_F_NOWAIT;

#ifdef CONFIG_COMPAT
	if (req->ctx->compat)
		sr->msg_flags |= MSG_CMSG_COMPAT;
#endif
	sr->done_io = 0;
	return 0;
}

static int io_recvmsg(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_async_msghdr iomsg, *kmsg;
	struct io_sr_msg *sr = &req->sr_msg;
	struct socket *sock;
	unsigned int cflags;
	unsigned flags;
	int ret, min_ret = 0;
	bool force_nonblock = issue_flags & IO_URING_F_NONBLOCK;

	sock = sock_from_file(req->file);
	if (unlikely(!sock))
		return -ENOTSOCK;

	if (req_has_async_data(req)) {
		kmsg = req->async_data;
	} else {
		ret = io_recvmsg_copy_hdr(req, &iomsg);
		if (ret)
			return ret;
		kmsg = &iomsg;
	}

	if (!(req->flags & REQ_F_POLLED) &&
	    (sr->flags & IORING_RECVSEND_POLL_FIRST))
		return io_setup_async_msg(req, kmsg);

	if (io_do_buffer_select(req)) {
		void __user *buf;

		buf = io_buffer_select(req, &sr->len, issue_flags);
		if (!buf)
			return -ENOBUFS;
		kmsg->fast_iov[0].iov_base = buf;
		kmsg->fast_iov[0].iov_len = sr->len;
		iov_iter_init(&kmsg->msg.msg_iter, READ, kmsg->fast_iov, 1,
				sr->len);
	}

	flags = sr->msg_flags;
	if (force_nonblock)
		flags |= MSG_DONTWAIT;
	if (flags & MSG_WAITALL)
		min_ret = iov_iter_count(&kmsg->msg.msg_iter);

	kmsg->msg.msg_get_inq = 1;
	ret = __sys_recvmsg_sock(sock, &kmsg->msg, sr->umsg, kmsg->uaddr, flags);
	if (ret < min_ret) {
		if (ret == -EAGAIN && force_nonblock)
			return io_setup_async_msg(req, kmsg);
		if (ret == -ERESTARTSYS)
			ret = -EINTR;
		if (ret > 0 && io_net_retry(sock, flags)) {
			sr->done_io += ret;
			req->flags |= REQ_F_PARTIAL_IO;
			return io_setup_async_msg(req, kmsg);
		}
		req_set_fail(req);
	} else if ((flags & MSG_WAITALL) && (kmsg->msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
		req_set_fail(req);
	}

	/* fast path, check for non-NULL to avoid function call */
	if (kmsg->free_iov)
		kfree(kmsg->free_iov);
	req->flags &= ~REQ_F_NEED_CLEANUP;
	if (ret >= 0)
		ret += sr->done_io;
	else if (sr->done_io)
		ret = sr->done_io;
	cflags = io_put_kbuf(req, issue_flags);
	if (kmsg->msg.msg_inq)
		cflags |= IORING_CQE_F_SOCK_NONEMPTY;
	__io_req_complete(req, issue_flags, ret, cflags);
	return 0;
}

static int io_recv(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_sr_msg *sr = &req->sr_msg;
	struct msghdr msg;
	struct socket *sock;
	struct iovec iov;
	unsigned int cflags;
	unsigned flags;
	int ret, min_ret = 0;
	bool force_nonblock = issue_flags & IO_URING_F_NONBLOCK;

	if (!(req->flags & REQ_F_POLLED) &&
	    (sr->flags & IORING_RECVSEND_POLL_FIRST))
		return -EAGAIN;

	sock = sock_from_file(req->file);
	if (unlikely(!sock))
		return -ENOTSOCK;

	if (io_do_buffer_select(req)) {
		void __user *buf;

		buf = io_buffer_select(req, &sr->len, issue_flags);
		if (!buf)
			return -ENOBUFS;
		sr->buf = buf;
	}

	ret = import_single_range(READ, sr->buf, sr->len, &iov, &msg.msg_iter);
	if (unlikely(ret))
		goto out_free;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_control = NULL;
	msg.msg_get_inq = 1;
	msg.msg_flags = 0;
	msg.msg_controllen = 0;
	msg.msg_iocb = NULL;

	flags = sr->msg_flags;
	if (force_nonblock)
		flags |= MSG_DONTWAIT;
	if (flags & MSG_WAITALL)
		min_ret = iov_iter_count(&msg.msg_iter);

	ret = sock_recvmsg(sock, &msg, flags);
	if (ret < min_ret) {
		if (ret == -EAGAIN && force_nonblock)
			return -EAGAIN;
		if (ret == -ERESTARTSYS)
			ret = -EINTR;
		if (ret > 0 && io_net_retry(sock, flags)) {
			sr->len -= ret;
			sr->buf += ret;
			sr->done_io += ret;
			req->flags |= REQ_F_PARTIAL_IO;
			return -EAGAIN;
		}
		req_set_fail(req);
	} else if ((flags & MSG_WAITALL) && (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
out_free:
		req_set_fail(req);
	}

	if (ret >= 0)
		ret += sr->done_io;
	else if (sr->done_io)
		ret = sr->done_io;
	cflags = io_put_kbuf(req, issue_flags);
	if (msg.msg_inq)
		cflags |= IORING_CQE_F_SOCK_NONEMPTY;
	__io_req_complete(req, issue_flags, ret, cflags);
	return 0;
}

static int io_accept_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_accept *accept = &req->accept;
	unsigned flags;

	if (sqe->len || sqe->buf_index)
		return -EINVAL;

	accept->addr = u64_to_user_ptr(READ_ONCE(sqe->addr));
	accept->addr_len = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	accept->flags = READ_ONCE(sqe->accept_flags);
	accept->nofile = rlimit(RLIMIT_NOFILE);
	flags = READ_ONCE(sqe->ioprio);
	if (flags & ~IORING_ACCEPT_MULTISHOT)
		return -EINVAL;

	accept->file_slot = READ_ONCE(sqe->file_index);
	if (accept->file_slot) {
		if (accept->flags & SOCK_CLOEXEC)
			return -EINVAL;
		if (flags & IORING_ACCEPT_MULTISHOT &&
		    accept->file_slot != IORING_FILE_INDEX_ALLOC)
			return -EINVAL;
	}
	if (accept->flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
		return -EINVAL;
	if (SOCK_NONBLOCK != O_NONBLOCK && (accept->flags & SOCK_NONBLOCK))
		accept->flags = (accept->flags & ~SOCK_NONBLOCK) | O_NONBLOCK;
	if (flags & IORING_ACCEPT_MULTISHOT)
		req->flags |= REQ_F_APOLL_MULTISHOT;
	return 0;
}

static int io_accept(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_accept *accept = &req->accept;
	bool force_nonblock = issue_flags & IO_URING_F_NONBLOCK;
	unsigned int file_flags = force_nonblock ? O_NONBLOCK : 0;
	bool fixed = !!accept->file_slot;
	struct file *file;
	int ret, fd;

retry:
	if (!fixed) {
		fd = __get_unused_fd_flags(accept->flags, accept->nofile);
		if (unlikely(fd < 0))
			return fd;
	}
	file = do_accept(req->file, file_flags, accept->addr, accept->addr_len,
			 accept->flags);
	if (IS_ERR(file)) {
		if (!fixed)
			put_unused_fd(fd);
		ret = PTR_ERR(file);
		if (ret == -EAGAIN && force_nonblock) {
			/*
			 * if it's multishot and polled, we don't need to
			 * return EAGAIN to arm the poll infra since it
			 * has already been done
			 */
			if ((req->flags & IO_APOLL_MULTI_POLLED) ==
			    IO_APOLL_MULTI_POLLED)
				ret = 0;
			return ret;
		}
		if (ret == -ERESTARTSYS)
			ret = -EINTR;
		req_set_fail(req);
	} else if (!fixed) {
		fd_install(fd, file);
		ret = fd;
	} else {
		ret = io_fixed_fd_install(req, issue_flags, file,
						accept->file_slot);
	}

	if (!(req->flags & REQ_F_APOLL_MULTISHOT)) {
		__io_req_complete(req, issue_flags, ret, 0);
		return 0;
	}
	if (ret >= 0) {
		bool filled;

		spin_lock(&ctx->completion_lock);
		filled = io_fill_cqe_aux(ctx, req->cqe.user_data, ret,
					 IORING_CQE_F_MORE);
		io_commit_cqring(ctx);
		spin_unlock(&ctx->completion_lock);
		if (filled) {
			io_cqring_ev_posted(ctx);
			goto retry;
		}
		ret = -ECANCELED;
	}

	return ret;
}

static int io_socket_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_socket *sock = &req->sock;

	if (sqe->addr || sqe->rw_flags || sqe->buf_index)
		return -EINVAL;

	sock->domain = READ_ONCE(sqe->fd);
	sock->type = READ_ONCE(sqe->off);
	sock->protocol = READ_ONCE(sqe->len);
	sock->file_slot = READ_ONCE(sqe->file_index);
	sock->nofile = rlimit(RLIMIT_NOFILE);

	sock->flags = sock->type & ~SOCK_TYPE_MASK;
	if (sock->file_slot && (sock->flags & SOCK_CLOEXEC))
		return -EINVAL;
	if (sock->flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
		return -EINVAL;
	return 0;
}

static int io_socket(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_socket *sock = &req->sock;
	bool fixed = !!sock->file_slot;
	struct file *file;
	int ret, fd;

	if (!fixed) {
		fd = __get_unused_fd_flags(sock->flags, sock->nofile);
		if (unlikely(fd < 0))
			return fd;
	}
	file = __sys_socket_file(sock->domain, sock->type, sock->protocol);
	if (IS_ERR(file)) {
		if (!fixed)
			put_unused_fd(fd);
		ret = PTR_ERR(file);
		if (ret == -EAGAIN && (issue_flags & IO_URING_F_NONBLOCK))
			return -EAGAIN;
		if (ret == -ERESTARTSYS)
			ret = -EINTR;
		req_set_fail(req);
	} else if (!fixed) {
		fd_install(fd, file);
		ret = fd;
	} else {
		ret = io_fixed_fd_install(req, issue_flags, file,
					    sock->file_slot);
	}
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int io_connect_prep_async(struct io_kiocb *req)
{
	struct io_async_connect *io = req->async_data;
	struct io_connect *conn = &req->connect;

	return move_addr_to_kernel(conn->addr, conn->addr_len, &io->address);
}

static int io_connect_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_connect *conn = &req->connect;

	if (sqe->len || sqe->buf_index || sqe->rw_flags || sqe->splice_fd_in)
		return -EINVAL;

	conn->addr = u64_to_user_ptr(READ_ONCE(sqe->addr));
	conn->addr_len =  READ_ONCE(sqe->addr2);
	return 0;
}

static int io_connect(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_async_connect __io, *io;
	unsigned file_flags;
	int ret;
	bool force_nonblock = issue_flags & IO_URING_F_NONBLOCK;

	if (req_has_async_data(req)) {
		io = req->async_data;
	} else {
		ret = move_addr_to_kernel(req->connect.addr,
						req->connect.addr_len,
						&__io.address);
		if (ret)
			goto out;
		io = &__io;
	}

	file_flags = force_nonblock ? O_NONBLOCK : 0;

	ret = __sys_connect_file(req->file, &io->address,
					req->connect.addr_len, file_flags);
	if ((ret == -EAGAIN || ret == -EINPROGRESS) && force_nonblock) {
		if (req_has_async_data(req))
			return -EAGAIN;
		if (io_alloc_async_data(req)) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(req->async_data, &__io, sizeof(__io));
		return -EAGAIN;
	}
	if (ret == -ERESTARTSYS)
		ret = -EINTR;
out:
	if (ret < 0)
		req_set_fail(req);
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}
#else /* !CONFIG_NET */
#define IO_NETOP_FN(op)							\
static int io_##op(struct io_kiocb *req, unsigned int issue_flags)	\
{									\
	return -EOPNOTSUPP;						\
}

#define IO_NETOP_PREP(op)						\
IO_NETOP_FN(op)								\
static int io_##op##_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe) \
{									\
	return -EOPNOTSUPP;						\
}									\

#define IO_NETOP_PREP_ASYNC(op)						\
IO_NETOP_PREP(op)							\
static int io_##op##_prep_async(struct io_kiocb *req)			\
{									\
	return -EOPNOTSUPP;						\
}

IO_NETOP_PREP_ASYNC(sendmsg);
IO_NETOP_PREP_ASYNC(recvmsg);
IO_NETOP_PREP_ASYNC(connect);
IO_NETOP_PREP(accept);
IO_NETOP_PREP(socket);
IO_NETOP_PREP(shutdown);
IO_NETOP_FN(send);
IO_NETOP_FN(recv);
#endif /* CONFIG_NET */

struct io_poll_table {
	struct poll_table_struct pt;
	struct io_kiocb *req;
	int nr_entries;
	int error;
};

#define IO_POLL_CANCEL_FLAG	BIT(31)
#define IO_POLL_REF_MASK	GENMASK(30, 0)

/*
 * If refs part of ->poll_refs (see IO_POLL_REF_MASK) is 0, it's free. We can
 * bump it and acquire ownership. It's disallowed to modify requests while not
 * owning it, that prevents from races for enqueueing task_work's and b/w
 * arming poll and wakeups.
 */
static inline bool io_poll_get_ownership(struct io_kiocb *req)
{
	return !(atomic_fetch_inc(&req->poll_refs) & IO_POLL_REF_MASK);
}

static void io_poll_mark_cancelled(struct io_kiocb *req)
{
	atomic_or(IO_POLL_CANCEL_FLAG, &req->poll_refs);
}

static struct io_poll_iocb *io_poll_get_double(struct io_kiocb *req)
{
	/* pure poll stashes this in ->async_data, poll driven retry elsewhere */
	if (req->opcode == IORING_OP_POLL_ADD)
		return req->async_data;
	return req->apoll->double_poll;
}

static struct io_poll_iocb *io_poll_get_single(struct io_kiocb *req)
{
	if (req->opcode == IORING_OP_POLL_ADD)
		return &req->poll;
	return &req->apoll->poll;
}

static void io_poll_req_insert(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct hlist_head *list;

	list = &ctx->cancel_hash[hash_long(req->cqe.user_data, ctx->cancel_hash_bits)];
	hlist_add_head(&req->hash_node, list);
}

static void io_init_poll_iocb(struct io_poll_iocb *poll, __poll_t events,
			      wait_queue_func_t wake_func)
{
	poll->head = NULL;
#define IO_POLL_UNMASK	(EPOLLERR|EPOLLHUP|EPOLLNVAL|EPOLLRDHUP)
	/* mask in events that we always want/need */
	poll->events = events | IO_POLL_UNMASK;
	INIT_LIST_HEAD(&poll->wait.entry);
	init_waitqueue_func_entry(&poll->wait, wake_func);
}

static inline void io_poll_remove_entry(struct io_poll_iocb *poll)
{
	struct wait_queue_head *head = smp_load_acquire(&poll->head);

	if (head) {
		spin_lock_irq(&head->lock);
		list_del_init(&poll->wait.entry);
		poll->head = NULL;
		spin_unlock_irq(&head->lock);
	}
}

static void io_poll_remove_entries(struct io_kiocb *req)
{
	/*
	 * Nothing to do if neither of those flags are set. Avoid dipping
	 * into the poll/apoll/double cachelines if we can.
	 */
	if (!(req->flags & (REQ_F_SINGLE_POLL | REQ_F_DOUBLE_POLL)))
		return;

	/*
	 * While we hold the waitqueue lock and the waitqueue is nonempty,
	 * wake_up_pollfree() will wait for us.  However, taking the waitqueue
	 * lock in the first place can race with the waitqueue being freed.
	 *
	 * We solve this as eventpoll does: by taking advantage of the fact that
	 * all users of wake_up_pollfree() will RCU-delay the actual free.  If
	 * we enter rcu_read_lock() and see that the pointer to the queue is
	 * non-NULL, we can then lock it without the memory being freed out from
	 * under us.
	 *
	 * Keep holding rcu_read_lock() as long as we hold the queue lock, in
	 * case the caller deletes the entry from the queue, leaving it empty.
	 * In that case, only RCU prevents the queue memory from being freed.
	 */
	rcu_read_lock();
	if (req->flags & REQ_F_SINGLE_POLL)
		io_poll_remove_entry(io_poll_get_single(req));
	if (req->flags & REQ_F_DOUBLE_POLL)
		io_poll_remove_entry(io_poll_get_double(req));
	rcu_read_unlock();
}

static int io_issue_sqe(struct io_kiocb *req, unsigned int issue_flags);
/*
 * All poll tw should go through this. Checks for poll events, manages
 * references, does rewait, etc.
 *
 * Returns a negative error on failure. >0 when no action require, which is
 * either spurious wakeup or multishot CQE is served. 0 when it's done with
 * the request, then the mask is stored in req->cqe.res.
 */
static int io_poll_check_events(struct io_kiocb *req, bool *locked)
{
	struct io_ring_ctx *ctx = req->ctx;
	int v, ret;

	/* req->task == current here, checking PF_EXITING is safe */
	if (unlikely(req->task->flags & PF_EXITING))
		return -ECANCELED;

	do {
		v = atomic_read(&req->poll_refs);

		/* tw handler should be the owner, and so have some references */
		if (WARN_ON_ONCE(!(v & IO_POLL_REF_MASK)))
			return 0;
		if (v & IO_POLL_CANCEL_FLAG)
			return -ECANCELED;

		if (!req->cqe.res) {
			struct poll_table_struct pt = { ._key = req->apoll_events };
			req->cqe.res = vfs_poll(req->file, &pt) & req->apoll_events;
		}

		if ((unlikely(!req->cqe.res)))
			continue;
		if (req->apoll_events & EPOLLONESHOT)
			return 0;

		/* multishot, just fill a CQE and proceed */
		if (!(req->flags & REQ_F_APOLL_MULTISHOT)) {
			__poll_t mask = mangle_poll(req->cqe.res &
						    req->apoll_events);
			bool filled;

			spin_lock(&ctx->completion_lock);
			filled = io_fill_cqe_aux(ctx, req->cqe.user_data,
						 mask, IORING_CQE_F_MORE);
			io_commit_cqring(ctx);
			spin_unlock(&ctx->completion_lock);
			if (filled) {
				io_cqring_ev_posted(ctx);
				continue;
			}
			return -ECANCELED;
		}

		io_tw_lock(req->ctx, locked);
		if (unlikely(req->task->flags & PF_EXITING))
			return -EFAULT;
		ret = io_issue_sqe(req,
				   IO_URING_F_NONBLOCK|IO_URING_F_COMPLETE_DEFER);
		if (ret)
			return ret;

		/*
		 * Release all references, retry if someone tried to restart
		 * task_work while we were executing it.
		 */
	} while (atomic_sub_return(v & IO_POLL_REF_MASK, &req->poll_refs));

	return 1;
}

static void io_poll_task_func(struct io_kiocb *req, bool *locked)
{
	struct io_ring_ctx *ctx = req->ctx;
	int ret;

	ret = io_poll_check_events(req, locked);
	if (ret > 0)
		return;

	if (!ret) {
		req->cqe.res = mangle_poll(req->cqe.res & req->poll.events);
	} else {
		req->cqe.res = ret;
		req_set_fail(req);
	}

	io_poll_remove_entries(req);
	spin_lock(&ctx->completion_lock);
	hash_del(&req->hash_node);
	__io_req_complete_post(req, req->cqe.res, 0);
	io_commit_cqring(ctx);
	spin_unlock(&ctx->completion_lock);
	io_cqring_ev_posted(ctx);
}

static void io_apoll_task_func(struct io_kiocb *req, bool *locked)
{
	struct io_ring_ctx *ctx = req->ctx;
	int ret;

	ret = io_poll_check_events(req, locked);
	if (ret > 0)
		return;

	io_poll_remove_entries(req);
	spin_lock(&ctx->completion_lock);
	hash_del(&req->hash_node);
	spin_unlock(&ctx->completion_lock);

	if (!ret)
		io_req_task_submit(req, locked);
	else
		io_req_complete_failed(req, ret);
}

static void __io_poll_execute(struct io_kiocb *req, int mask, __poll_t events)
{
	req->cqe.res = mask;
	/*
	 * This is useful for poll that is armed on behalf of another
	 * request, and where the wakeup path could be on a different
	 * CPU. We want to avoid pulling in req->apoll->events for that
	 * case.
	 */
	req->apoll_events = events;
	if (req->opcode == IORING_OP_POLL_ADD)
		req->io_task_work.func = io_poll_task_func;
	else
		req->io_task_work.func = io_apoll_task_func;

	trace_io_uring_task_add(req->ctx, req, req->cqe.user_data, req->opcode, mask);
	io_req_task_work_add(req);
}

static inline void io_poll_execute(struct io_kiocb *req, int res,
		__poll_t events)
{
	if (io_poll_get_ownership(req))
		__io_poll_execute(req, res, events);
}

static void io_poll_cancel_req(struct io_kiocb *req)
{
	io_poll_mark_cancelled(req);
	/* kick tw, which should complete the request */
	io_poll_execute(req, 0, 0);
}

#define wqe_to_req(wait)	((void *)((unsigned long) (wait)->private & ~1))
#define wqe_is_double(wait)	((unsigned long) (wait)->private & 1)
#define IO_ASYNC_POLL_COMMON	(EPOLLONESHOT | EPOLLPRI)

static int io_poll_wake(struct wait_queue_entry *wait, unsigned mode, int sync,
			void *key)
{
	struct io_kiocb *req = wqe_to_req(wait);
	struct io_poll_iocb *poll = container_of(wait, struct io_poll_iocb,
						 wait);
	__poll_t mask = key_to_poll(key);

	if (unlikely(mask & POLLFREE)) {
		io_poll_mark_cancelled(req);
		/* we have to kick tw in case it's not already */
		io_poll_execute(req, 0, poll->events);

		/*
		 * If the waitqueue is being freed early but someone is already
		 * holds ownership over it, we have to tear down the request as
		 * best we can. That means immediately removing the request from
		 * its waitqueue and preventing all further accesses to the
		 * waitqueue via the request.
		 */
		list_del_init(&poll->wait.entry);

		/*
		 * Careful: this *must* be the last step, since as soon
		 * as req->head is NULL'ed out, the request can be
		 * completed and freed, since aio_poll_complete_work()
		 * will no longer need to take the waitqueue lock.
		 */
		smp_store_release(&poll->head, NULL);
		return 1;
	}

	/* for instances that support it check for an event match first */
	if (mask && !(mask & (poll->events & ~IO_ASYNC_POLL_COMMON)))
		return 0;

	if (io_poll_get_ownership(req)) {
		/* optional, saves extra locking for removal in tw handler */
		if (mask && poll->events & EPOLLONESHOT) {
			list_del_init(&poll->wait.entry);
			poll->head = NULL;
			if (wqe_is_double(wait))
				req->flags &= ~REQ_F_DOUBLE_POLL;
			else
				req->flags &= ~REQ_F_SINGLE_POLL;
		}
		__io_poll_execute(req, mask, poll->events);
	}
	return 1;
}

static void __io_queue_proc(struct io_poll_iocb *poll, struct io_poll_table *pt,
			    struct wait_queue_head *head,
			    struct io_poll_iocb **poll_ptr)
{
	struct io_kiocb *req = pt->req;
	unsigned long wqe_private = (unsigned long) req;

	/*
	 * The file being polled uses multiple waitqueues for poll handling
	 * (e.g. one for read, one for write). Setup a separate io_poll_iocb
	 * if this happens.
	 */
	if (unlikely(pt->nr_entries)) {
		struct io_poll_iocb *first = poll;

		/* double add on the same waitqueue head, ignore */
		if (first->head == head)
			return;
		/* already have a 2nd entry, fail a third attempt */
		if (*poll_ptr) {
			if ((*poll_ptr)->head == head)
				return;
			pt->error = -EINVAL;
			return;
		}

		poll = kmalloc(sizeof(*poll), GFP_ATOMIC);
		if (!poll) {
			pt->error = -ENOMEM;
			return;
		}
		/* mark as double wq entry */
		wqe_private |= 1;
		req->flags |= REQ_F_DOUBLE_POLL;
		io_init_poll_iocb(poll, first->events, first->wait.func);
		*poll_ptr = poll;
		if (req->opcode == IORING_OP_POLL_ADD)
			req->flags |= REQ_F_ASYNC_DATA;
	}

	req->flags |= REQ_F_SINGLE_POLL;
	pt->nr_entries++;
	poll->head = head;
	poll->wait.private = (void *) wqe_private;

	if (poll->events & EPOLLEXCLUSIVE)
		add_wait_queue_exclusive(head, &poll->wait);
	else
		add_wait_queue(head, &poll->wait);
}

static void io_poll_queue_proc(struct file *file, struct wait_queue_head *head,
			       struct poll_table_struct *p)
{
	struct io_poll_table *pt = container_of(p, struct io_poll_table, pt);

	__io_queue_proc(&pt->req->poll, pt, head,
			(struct io_poll_iocb **) &pt->req->async_data);
}

static int __io_arm_poll_handler(struct io_kiocb *req,
				 struct io_poll_iocb *poll,
				 struct io_poll_table *ipt, __poll_t mask)
{
	struct io_ring_ctx *ctx = req->ctx;
	int v;

	INIT_HLIST_NODE(&req->hash_node);
	req->work.cancel_seq = atomic_read(&ctx->cancel_seq);
	io_init_poll_iocb(poll, mask, io_poll_wake);
	poll->file = req->file;

	ipt->pt._key = mask;
	ipt->req = req;
	ipt->error = 0;
	ipt->nr_entries = 0;

	/*
	 * Take the ownership to delay any tw execution up until we're done
	 * with poll arming. see io_poll_get_ownership().
	 */
	atomic_set(&req->poll_refs, 1);
	mask = vfs_poll(req->file, &ipt->pt) & poll->events;

	if (mask && (poll->events & EPOLLONESHOT)) {
		io_poll_remove_entries(req);
		/* no one else has access to the req, forget about the ref */
		return mask;
	}
	if (!mask && unlikely(ipt->error || !ipt->nr_entries)) {
		io_poll_remove_entries(req);
		if (!ipt->error)
			ipt->error = -EINVAL;
		return 0;
	}

	spin_lock(&ctx->completion_lock);
	io_poll_req_insert(req);
	spin_unlock(&ctx->completion_lock);

	if (mask) {
		/* can't multishot if failed, just queue the event we've got */
		if (unlikely(ipt->error || !ipt->nr_entries))
			poll->events |= EPOLLONESHOT;
		__io_poll_execute(req, mask, poll->events);
		return 0;
	}

	/*
	 * Release ownership. If someone tried to queue a tw while it was
	 * locked, kick it off for them.
	 */
	v = atomic_dec_return(&req->poll_refs);
	if (unlikely(v & IO_POLL_REF_MASK))
		__io_poll_execute(req, 0, poll->events);
	return 0;
}

static void io_async_queue_proc(struct file *file, struct wait_queue_head *head,
			       struct poll_table_struct *p)
{
	struct io_poll_table *pt = container_of(p, struct io_poll_table, pt);
	struct async_poll *apoll = pt->req->apoll;

	__io_queue_proc(&apoll->poll, pt, head, &apoll->double_poll);
}

enum {
	IO_APOLL_OK,
	IO_APOLL_ABORTED,
	IO_APOLL_READY
};

static int io_arm_poll_handler(struct io_kiocb *req, unsigned issue_flags)
{
	const struct io_op_def *def = &io_op_defs[req->opcode];
	struct io_ring_ctx *ctx = req->ctx;
	struct async_poll *apoll;
	struct io_poll_table ipt;
	__poll_t mask = POLLPRI | POLLERR;
	int ret;

	if (!def->pollin && !def->pollout)
		return IO_APOLL_ABORTED;
	if (!file_can_poll(req->file))
		return IO_APOLL_ABORTED;
	if ((req->flags & (REQ_F_POLLED|REQ_F_PARTIAL_IO)) == REQ_F_POLLED)
		return IO_APOLL_ABORTED;
	if (!(req->flags & REQ_F_APOLL_MULTISHOT))
		mask |= EPOLLONESHOT;

	if (def->pollin) {
		mask |= EPOLLIN | EPOLLRDNORM;

		/* If reading from MSG_ERRQUEUE using recvmsg, ignore POLLIN */
		if ((req->opcode == IORING_OP_RECVMSG) &&
		    (req->sr_msg.msg_flags & MSG_ERRQUEUE))
			mask &= ~EPOLLIN;
	} else {
		mask |= EPOLLOUT | EPOLLWRNORM;
	}
	if (def->poll_exclusive)
		mask |= EPOLLEXCLUSIVE;
	if (req->flags & REQ_F_POLLED) {
		apoll = req->apoll;
	} else if (!(issue_flags & IO_URING_F_UNLOCKED) &&
		   !list_empty(&ctx->apoll_cache)) {
		apoll = list_first_entry(&ctx->apoll_cache, struct async_poll,
						poll.wait.entry);
		list_del_init(&apoll->poll.wait.entry);
	} else {
		apoll = kmalloc(sizeof(*apoll), GFP_ATOMIC);
		if (unlikely(!apoll))
			return IO_APOLL_ABORTED;
	}
	apoll->double_poll = NULL;
	req->apoll = apoll;
	req->flags |= REQ_F_POLLED;
	ipt.pt._qproc = io_async_queue_proc;

	io_kbuf_recycle(req, issue_flags);

	ret = __io_arm_poll_handler(req, &apoll->poll, &ipt, mask);
	if (ret || ipt.error)
		return ret ? IO_APOLL_READY : IO_APOLL_ABORTED;

	trace_io_uring_poll_arm(ctx, req, req->cqe.user_data, req->opcode,
				mask, apoll->poll.events);
	return IO_APOLL_OK;
}

/*
 * Returns true if we found and killed one or more poll requests
 */
static __cold bool io_poll_remove_all(struct io_ring_ctx *ctx,
				      struct task_struct *tsk, bool cancel_all)
{
	struct hlist_node *tmp;
	struct io_kiocb *req;
	bool found = false;
	int i;

	spin_lock(&ctx->completion_lock);
	for (i = 0; i < (1U << ctx->cancel_hash_bits); i++) {
		struct hlist_head *list;

		list = &ctx->cancel_hash[i];
		hlist_for_each_entry_safe(req, tmp, list, hash_node) {
			if (io_match_task_safe(req, tsk, cancel_all)) {
				hlist_del_init(&req->hash_node);
				io_poll_cancel_req(req);
				found = true;
			}
		}
	}
	spin_unlock(&ctx->completion_lock);
	return found;
}

static struct io_kiocb *io_poll_find(struct io_ring_ctx *ctx, bool poll_only,
				     struct io_cancel_data *cd)
	__must_hold(&ctx->completion_lock)
{
	struct hlist_head *list;
	struct io_kiocb *req;

	list = &ctx->cancel_hash[hash_long(cd->data, ctx->cancel_hash_bits)];
	hlist_for_each_entry(req, list, hash_node) {
		if (cd->data != req->cqe.user_data)
			continue;
		if (poll_only && req->opcode != IORING_OP_POLL_ADD)
			continue;
		if (cd->flags & IORING_ASYNC_CANCEL_ALL) {
			if (cd->seq == req->work.cancel_seq)
				continue;
			req->work.cancel_seq = cd->seq;
		}
		return req;
	}
	return NULL;
}

static struct io_kiocb *io_poll_file_find(struct io_ring_ctx *ctx,
					  struct io_cancel_data *cd)
	__must_hold(&ctx->completion_lock)
{
	struct io_kiocb *req;
	int i;

	for (i = 0; i < (1U << ctx->cancel_hash_bits); i++) {
		struct hlist_head *list;

		list = &ctx->cancel_hash[i];
		hlist_for_each_entry(req, list, hash_node) {
			if (!(cd->flags & IORING_ASYNC_CANCEL_ANY) &&
			    req->file != cd->file)
				continue;
			if (cd->seq == req->work.cancel_seq)
				continue;
			req->work.cancel_seq = cd->seq;
			return req;
		}
	}
	return NULL;
}

static bool io_poll_disarm(struct io_kiocb *req)
	__must_hold(&ctx->completion_lock)
{
	if (!io_poll_get_ownership(req))
		return false;
	io_poll_remove_entries(req);
	hash_del(&req->hash_node);
	return true;
}

static int io_poll_cancel(struct io_ring_ctx *ctx, struct io_cancel_data *cd)
	__must_hold(&ctx->completion_lock)
{
	struct io_kiocb *req;

	if (cd->flags & (IORING_ASYNC_CANCEL_FD|IORING_ASYNC_CANCEL_ANY))
		req = io_poll_file_find(ctx, cd);
	else
		req = io_poll_find(ctx, false, cd);
	if (!req)
		return -ENOENT;
	io_poll_cancel_req(req);
	return 0;
}

static __poll_t io_poll_parse_events(const struct io_uring_sqe *sqe,
				     unsigned int flags)
{
	u32 events;

	events = READ_ONCE(sqe->poll32_events);
#ifdef __BIG_ENDIAN
	events = swahw32(events);
#endif
	if (!(flags & IORING_POLL_ADD_MULTI))
		events |= EPOLLONESHOT;
	return demangle_poll(events) | (events & (EPOLLEXCLUSIVE|EPOLLONESHOT));
}

static int io_poll_remove_prep(struct io_kiocb *req,
			       const struct io_uring_sqe *sqe)
{
	struct io_poll_update *upd = &req->poll_update;
	u32 flags;

	if (sqe->buf_index || sqe->splice_fd_in)
		return -EINVAL;
	flags = READ_ONCE(sqe->len);
	if (flags & ~(IORING_POLL_UPDATE_EVENTS | IORING_POLL_UPDATE_USER_DATA |
		      IORING_POLL_ADD_MULTI))
		return -EINVAL;
	/* meaningless without update */
	if (flags == IORING_POLL_ADD_MULTI)
		return -EINVAL;

	upd->old_user_data = READ_ONCE(sqe->addr);
	upd->update_events = flags & IORING_POLL_UPDATE_EVENTS;
	upd->update_user_data = flags & IORING_POLL_UPDATE_USER_DATA;

	upd->new_user_data = READ_ONCE(sqe->off);
	if (!upd->update_user_data && upd->new_user_data)
		return -EINVAL;
	if (upd->update_events)
		upd->events = io_poll_parse_events(sqe, flags);
	else if (sqe->poll32_events)
		return -EINVAL;

	return 0;
}

static int io_poll_add_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_poll_iocb *poll = &req->poll;
	u32 flags;

	if (sqe->buf_index || sqe->off || sqe->addr)
		return -EINVAL;
	flags = READ_ONCE(sqe->len);
	if (flags & ~IORING_POLL_ADD_MULTI)
		return -EINVAL;
	if ((flags & IORING_POLL_ADD_MULTI) && (req->flags & REQ_F_CQE_SKIP))
		return -EINVAL;

	io_req_set_refcount(req);
	req->apoll_events = poll->events = io_poll_parse_events(sqe, flags);
	return 0;
}

static int io_poll_add(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_poll_iocb *poll = &req->poll;
	struct io_poll_table ipt;
	int ret;

	ipt.pt._qproc = io_poll_queue_proc;

	ret = __io_arm_poll_handler(req, &req->poll, &ipt, poll->events);
	ret = ret ?: ipt.error;
	if (ret)
		__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int io_poll_remove(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_cancel_data cd = { .data = req->poll_update.old_user_data, };
	struct io_ring_ctx *ctx = req->ctx;
	struct io_kiocb *preq;
	int ret2, ret = 0;
	bool locked;

	spin_lock(&ctx->completion_lock);
	preq = io_poll_find(ctx, true, &cd);
	if (!preq || !io_poll_disarm(preq)) {
		spin_unlock(&ctx->completion_lock);
		ret = preq ? -EALREADY : -ENOENT;
		goto out;
	}
	spin_unlock(&ctx->completion_lock);

	if (req->poll_update.update_events || req->poll_update.update_user_data) {
		/* only mask one event flags, keep behavior flags */
		if (req->poll_update.update_events) {
			preq->poll.events &= ~0xffff;
			preq->poll.events |= req->poll_update.events & 0xffff;
			preq->poll.events |= IO_POLL_UNMASK;
		}
		if (req->poll_update.update_user_data)
			preq->cqe.user_data = req->poll_update.new_user_data;

		ret2 = io_poll_add(preq, issue_flags);
		/* successfully updated, don't complete poll request */
		if (!ret2)
			goto out;
	}

	req_set_fail(preq);
	preq->cqe.res = -ECANCELED;
	locked = !(issue_flags & IO_URING_F_UNLOCKED);
	io_req_task_complete(preq, &locked);
out:
	if (ret < 0)
		req_set_fail(req);
	/* complete update request, we're done with it */
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static enum hrtimer_restart io_timeout_fn(struct hrtimer *timer)
{
	struct io_timeout_data *data = container_of(timer,
						struct io_timeout_data, timer);
	struct io_kiocb *req = data->req;
	struct io_ring_ctx *ctx = req->ctx;
	unsigned long flags;

	spin_lock_irqsave(&ctx->timeout_lock, flags);
	list_del_init(&req->timeout.list);
	atomic_set(&req->ctx->cq_timeouts,
		atomic_read(&req->ctx->cq_timeouts) + 1);
	spin_unlock_irqrestore(&ctx->timeout_lock, flags);

	if (!(data->flags & IORING_TIMEOUT_ETIME_SUCCESS))
		req_set_fail(req);

	req->cqe.res = -ETIME;
	req->io_task_work.func = io_req_task_complete;
	io_req_task_work_add(req);
	return HRTIMER_NORESTART;
}

static struct io_kiocb *io_timeout_extract(struct io_ring_ctx *ctx,
					   struct io_cancel_data *cd)
	__must_hold(&ctx->timeout_lock)
{
	struct io_timeout_data *io;
	struct io_kiocb *req;
	bool found = false;

	list_for_each_entry(req, &ctx->timeout_list, timeout.list) {
		if (!(cd->flags & IORING_ASYNC_CANCEL_ANY) &&
		    cd->data != req->cqe.user_data)
			continue;
		if (cd->flags & (IORING_ASYNC_CANCEL_ALL|IORING_ASYNC_CANCEL_ANY)) {
			if (cd->seq == req->work.cancel_seq)
				continue;
			req->work.cancel_seq = cd->seq;
		}
		found = true;
		break;
	}
	if (!found)
		return ERR_PTR(-ENOENT);

	io = req->async_data;
	if (hrtimer_try_to_cancel(&io->timer) == -1)
		return ERR_PTR(-EALREADY);
	list_del_init(&req->timeout.list);
	return req;
}

static int io_timeout_cancel(struct io_ring_ctx *ctx, struct io_cancel_data *cd)
	__must_hold(&ctx->completion_lock)
{
	struct io_kiocb *req;

	spin_lock_irq(&ctx->timeout_lock);
	req = io_timeout_extract(ctx, cd);
	spin_unlock_irq(&ctx->timeout_lock);

	if (IS_ERR(req))
		return PTR_ERR(req);
	io_req_task_queue_fail(req, -ECANCELED);
	return 0;
}

static clockid_t io_timeout_get_clock(struct io_timeout_data *data)
{
	switch (data->flags & IORING_TIMEOUT_CLOCK_MASK) {
	case IORING_TIMEOUT_BOOTTIME:
		return CLOCK_BOOTTIME;
	case IORING_TIMEOUT_REALTIME:
		return CLOCK_REALTIME;
	default:
		/* can't happen, vetted at prep time */
		WARN_ON_ONCE(1);
		fallthrough;
	case 0:
		return CLOCK_MONOTONIC;
	}
}

static int io_linked_timeout_update(struct io_ring_ctx *ctx, __u64 user_data,
				    struct timespec64 *ts, enum hrtimer_mode mode)
	__must_hold(&ctx->timeout_lock)
{
	struct io_timeout_data *io;
	struct io_kiocb *req;
	bool found = false;

	list_for_each_entry(req, &ctx->ltimeout_list, timeout.list) {
		found = user_data == req->cqe.user_data;
		if (found)
			break;
	}
	if (!found)
		return -ENOENT;

	io = req->async_data;
	if (hrtimer_try_to_cancel(&io->timer) == -1)
		return -EALREADY;
	hrtimer_init(&io->timer, io_timeout_get_clock(io), mode);
	io->timer.function = io_link_timeout_fn;
	hrtimer_start(&io->timer, timespec64_to_ktime(*ts), mode);
	return 0;
}

static int io_timeout_update(struct io_ring_ctx *ctx, __u64 user_data,
			     struct timespec64 *ts, enum hrtimer_mode mode)
	__must_hold(&ctx->timeout_lock)
{
	struct io_cancel_data cd = { .data = user_data, };
	struct io_kiocb *req = io_timeout_extract(ctx, &cd);
	struct io_timeout_data *data;

	if (IS_ERR(req))
		return PTR_ERR(req);

	req->timeout.off = 0; /* noseq */
	data = req->async_data;
	list_add_tail(&req->timeout.list, &ctx->timeout_list);
	hrtimer_init(&data->timer, io_timeout_get_clock(data), mode);
	data->timer.function = io_timeout_fn;
	hrtimer_start(&data->timer, timespec64_to_ktime(*ts), mode);
	return 0;
}

static int io_timeout_remove_prep(struct io_kiocb *req,
				  const struct io_uring_sqe *sqe)
{
	struct io_timeout_rem *tr = &req->timeout_rem;

	if (unlikely(req->flags & (REQ_F_FIXED_FILE | REQ_F_BUFFER_SELECT)))
		return -EINVAL;
	if (sqe->buf_index || sqe->len || sqe->splice_fd_in)
		return -EINVAL;

	tr->ltimeout = false;
	tr->addr = READ_ONCE(sqe->addr);
	tr->flags = READ_ONCE(sqe->timeout_flags);
	if (tr->flags & IORING_TIMEOUT_UPDATE_MASK) {
		if (hweight32(tr->flags & IORING_TIMEOUT_CLOCK_MASK) > 1)
			return -EINVAL;
		if (tr->flags & IORING_LINK_TIMEOUT_UPDATE)
			tr->ltimeout = true;
		if (tr->flags & ~(IORING_TIMEOUT_UPDATE_MASK|IORING_TIMEOUT_ABS))
			return -EINVAL;
		if (get_timespec64(&tr->ts, u64_to_user_ptr(sqe->addr2)))
			return -EFAULT;
		if (tr->ts.tv_sec < 0 || tr->ts.tv_nsec < 0)
			return -EINVAL;
	} else if (tr->flags) {
		/* timeout removal doesn't support flags */
		return -EINVAL;
	}

	return 0;
}

static inline enum hrtimer_mode io_translate_timeout_mode(unsigned int flags)
{
	return (flags & IORING_TIMEOUT_ABS) ? HRTIMER_MODE_ABS
					    : HRTIMER_MODE_REL;
}

/*
 * Remove or update an existing timeout command
 */
static int io_timeout_remove(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_timeout_rem *tr = &req->timeout_rem;
	struct io_ring_ctx *ctx = req->ctx;
	int ret;

	if (!(req->timeout_rem.flags & IORING_TIMEOUT_UPDATE)) {
		struct io_cancel_data cd = { .data = tr->addr, };

		spin_lock(&ctx->completion_lock);
		ret = io_timeout_cancel(ctx, &cd);
		spin_unlock(&ctx->completion_lock);
	} else {
		enum hrtimer_mode mode = io_translate_timeout_mode(tr->flags);

		spin_lock_irq(&ctx->timeout_lock);
		if (tr->ltimeout)
			ret = io_linked_timeout_update(ctx, tr->addr, &tr->ts, mode);
		else
			ret = io_timeout_update(ctx, tr->addr, &tr->ts, mode);
		spin_unlock_irq(&ctx->timeout_lock);
	}

	if (ret < 0)
		req_set_fail(req);
	io_req_complete_post(req, ret, 0);
	return 0;
}

static int __io_timeout_prep(struct io_kiocb *req,
			     const struct io_uring_sqe *sqe,
			     bool is_timeout_link)
{
	struct io_timeout_data *data;
	unsigned flags;
	u32 off = READ_ONCE(sqe->off);

	if (sqe->buf_index || sqe->len != 1 || sqe->splice_fd_in)
		return -EINVAL;
	if (off && is_timeout_link)
		return -EINVAL;
	flags = READ_ONCE(sqe->timeout_flags);
	if (flags & ~(IORING_TIMEOUT_ABS | IORING_TIMEOUT_CLOCK_MASK |
		      IORING_TIMEOUT_ETIME_SUCCESS))
		return -EINVAL;
	/* more than one clock specified is invalid, obviously */
	if (hweight32(flags & IORING_TIMEOUT_CLOCK_MASK) > 1)
		return -EINVAL;

	INIT_LIST_HEAD(&req->timeout.list);
	req->timeout.off = off;
	if (unlikely(off && !req->ctx->off_timeout_used))
		req->ctx->off_timeout_used = true;

	if (WARN_ON_ONCE(req_has_async_data(req)))
		return -EFAULT;
	if (io_alloc_async_data(req))
		return -ENOMEM;

	data = req->async_data;
	data->req = req;
	data->flags = flags;

	if (get_timespec64(&data->ts, u64_to_user_ptr(sqe->addr)))
		return -EFAULT;

	if (data->ts.tv_sec < 0 || data->ts.tv_nsec < 0)
		return -EINVAL;

	INIT_LIST_HEAD(&req->timeout.list);
	data->mode = io_translate_timeout_mode(flags);
	hrtimer_init(&data->timer, io_timeout_get_clock(data), data->mode);

	if (is_timeout_link) {
		struct io_submit_link *link = &req->ctx->submit_state.link;

		if (!link->head)
			return -EINVAL;
		if (link->last->opcode == IORING_OP_LINK_TIMEOUT)
			return -EINVAL;
		req->timeout.head = link->last;
		link->last->flags |= REQ_F_ARM_LTIMEOUT;
	}
	return 0;
}

static int io_timeout_prep(struct io_kiocb *req,
			   const struct io_uring_sqe *sqe)
{
	return __io_timeout_prep(req, sqe, false);
}

static int io_link_timeout_prep(struct io_kiocb *req,
				const struct io_uring_sqe *sqe)
{
	return __io_timeout_prep(req, sqe, true);
}

static int io_timeout(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_timeout_data *data = req->async_data;
	struct list_head *entry;
	u32 tail, off = req->timeout.off;

	spin_lock_irq(&ctx->timeout_lock);

	/*
	 * sqe->off holds how many events that need to occur for this
	 * timeout event to be satisfied. If it isn't set, then this is
	 * a pure timeout request, sequence isn't used.
	 */
	if (io_is_timeout_noseq(req)) {
		entry = ctx->timeout_list.prev;
		goto add;
	}

	tail = ctx->cached_cq_tail - atomic_read(&ctx->cq_timeouts);
	req->timeout.target_seq = tail + off;

	/* Update the last seq here in case io_flush_timeouts() hasn't.
	 * This is safe because ->completion_lock is held, and submissions
	 * and completions are never mixed in the same ->completion_lock section.
	 */
	ctx->cq_last_tm_flush = tail;

	/*
	 * Insertion sort, ensuring the first entry in the list is always
	 * the one we need first.
	 */
	list_for_each_prev(entry, &ctx->timeout_list) {
		struct io_kiocb *nxt = list_entry(entry, struct io_kiocb,
						  timeout.list);

		if (io_is_timeout_noseq(nxt))
			continue;
		/* nxt.seq is behind @tail, otherwise would've been completed */
		if (off >= nxt->timeout.target_seq - tail)
			break;
	}
add:
	list_add(&req->timeout.list, entry);
	data->timer.function = io_timeout_fn;
	hrtimer_start(&data->timer, timespec64_to_ktime(data->ts), data->mode);
	spin_unlock_irq(&ctx->timeout_lock);
	return 0;
}

static bool io_cancel_cb(struct io_wq_work *work, void *data)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);
	struct io_cancel_data *cd = data;

	if (req->ctx != cd->ctx)
		return false;
	if (cd->flags & IORING_ASYNC_CANCEL_ANY) {
		;
	} else if (cd->flags & IORING_ASYNC_CANCEL_FD) {
		if (req->file != cd->file)
			return false;
	} else {
		if (req->cqe.user_data != cd->data)
			return false;
	}
	if (cd->flags & (IORING_ASYNC_CANCEL_ALL|IORING_ASYNC_CANCEL_ANY)) {
		if (cd->seq == req->work.cancel_seq)
			return false;
		req->work.cancel_seq = cd->seq;
	}
	return true;
}

static int io_async_cancel_one(struct io_uring_task *tctx,
			       struct io_cancel_data *cd)
{
	enum io_wq_cancel cancel_ret;
	int ret = 0;
	bool all;

	if (!tctx || !tctx->io_wq)
		return -ENOENT;

	all = cd->flags & (IORING_ASYNC_CANCEL_ALL|IORING_ASYNC_CANCEL_ANY);
	cancel_ret = io_wq_cancel_cb(tctx->io_wq, io_cancel_cb, cd, all);
	switch (cancel_ret) {
	case IO_WQ_CANCEL_OK:
		ret = 0;
		break;
	case IO_WQ_CANCEL_RUNNING:
		ret = -EALREADY;
		break;
	case IO_WQ_CANCEL_NOTFOUND:
		ret = -ENOENT;
		break;
	}

	return ret;
}

static int io_try_cancel(struct io_kiocb *req, struct io_cancel_data *cd)
{
	struct io_ring_ctx *ctx = req->ctx;
	int ret;

	WARN_ON_ONCE(!io_wq_current_is_worker() && req->task != current);

	ret = io_async_cancel_one(req->task->io_uring, cd);
	/*
	 * Fall-through even for -EALREADY, as we may have poll armed
	 * that need unarming.
	 */
	if (!ret)
		return 0;

	spin_lock(&ctx->completion_lock);
	ret = io_poll_cancel(ctx, cd);
	if (ret != -ENOENT)
		goto out;
	if (!(cd->flags & IORING_ASYNC_CANCEL_FD))
		ret = io_timeout_cancel(ctx, cd);
out:
	spin_unlock(&ctx->completion_lock);
	return ret;
}

#define CANCEL_FLAGS	(IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD | \
			 IORING_ASYNC_CANCEL_ANY)

static int io_async_cancel_prep(struct io_kiocb *req,
				const struct io_uring_sqe *sqe)
{
	if (unlikely(req->flags & REQ_F_BUFFER_SELECT))
		return -EINVAL;
	if (sqe->off || sqe->len || sqe->splice_fd_in)
		return -EINVAL;

	req->cancel.addr = READ_ONCE(sqe->addr);
	req->cancel.flags = READ_ONCE(sqe->cancel_flags);
	if (req->cancel.flags & ~CANCEL_FLAGS)
		return -EINVAL;
	if (req->cancel.flags & IORING_ASYNC_CANCEL_FD) {
		if (req->cancel.flags & IORING_ASYNC_CANCEL_ANY)
			return -EINVAL;
		req->cancel.fd = READ_ONCE(sqe->fd);
	}

	return 0;
}

static int __io_async_cancel(struct io_cancel_data *cd, struct io_kiocb *req,
			     unsigned int issue_flags)
{
	bool all = cd->flags & (IORING_ASYNC_CANCEL_ALL|IORING_ASYNC_CANCEL_ANY);
	struct io_ring_ctx *ctx = cd->ctx;
	struct io_tctx_node *node;
	int ret, nr = 0;

	do {
		ret = io_try_cancel(req, cd);
		if (ret == -ENOENT)
			break;
		if (!all)
			return ret;
		nr++;
	} while (1);

	/* slow path, try all io-wq's */
	io_ring_submit_lock(ctx, issue_flags);
	ret = -ENOENT;
	list_for_each_entry(node, &ctx->tctx_list, ctx_node) {
		struct io_uring_task *tctx = node->task->io_uring;

		ret = io_async_cancel_one(tctx, cd);
		if (ret != -ENOENT) {
			if (!all)
				break;
			nr++;
		}
	}
	io_ring_submit_unlock(ctx, issue_flags);
	return all ? nr : ret;
}

static int io_async_cancel(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_cancel_data cd = {
		.ctx	= req->ctx,
		.data	= req->cancel.addr,
		.flags	= req->cancel.flags,
		.seq	= atomic_inc_return(&req->ctx->cancel_seq),
	};
	int ret;

	if (cd.flags & IORING_ASYNC_CANCEL_FD) {
		if (req->flags & REQ_F_FIXED_FILE)
			req->file = io_file_get_fixed(req, req->cancel.fd,
							issue_flags);
		else
			req->file = io_file_get_normal(req, req->cancel.fd);
		if (!req->file) {
			ret = -EBADF;
			goto done;
		}
		cd.file = req->file;
	}

	ret = __io_async_cancel(&cd, req, issue_flags);
done:
	if (ret < 0)
		req_set_fail(req);
	io_req_complete_post(req, ret, 0);
	return 0;
}

static int io_files_update_prep(struct io_kiocb *req,
				const struct io_uring_sqe *sqe)
{
	if (unlikely(req->flags & (REQ_F_FIXED_FILE | REQ_F_BUFFER_SELECT)))
		return -EINVAL;
	if (sqe->rw_flags || sqe->splice_fd_in)
		return -EINVAL;

	req->rsrc_update.offset = READ_ONCE(sqe->off);
	req->rsrc_update.nr_args = READ_ONCE(sqe->len);
	if (!req->rsrc_update.nr_args)
		return -EINVAL;
	req->rsrc_update.arg = READ_ONCE(sqe->addr);
	return 0;
}

static int io_files_update_with_index_alloc(struct io_kiocb *req,
					    unsigned int issue_flags)
{
	__s32 __user *fds = u64_to_user_ptr(req->rsrc_update.arg);
	unsigned int done;
	struct file *file;
	int ret, fd;

	for (done = 0; done < req->rsrc_update.nr_args; done++) {
		if (copy_from_user(&fd, &fds[done], sizeof(fd))) {
			ret = -EFAULT;
			break;
		}

		file = fget(fd);
		if (!file) {
			ret = -EBADF;
			break;
		}
		ret = io_fixed_fd_install(req, issue_flags, file,
					  IORING_FILE_INDEX_ALLOC);
		if (ret < 0)
			break;
		if (copy_to_user(&fds[done], &ret, sizeof(ret))) {
			__io_close_fixed(req, issue_flags, ret);
			ret = -EFAULT;
			break;
		}
	}

	if (done)
		return done;
	return ret;
}

static int io_files_update(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_uring_rsrc_update2 up;
	int ret;

	up.offset = req->rsrc_update.offset;
	up.data = req->rsrc_update.arg;
	up.nr = 0;
	up.tags = 0;
	up.resv = 0;
	up.resv2 = 0;

	if (req->rsrc_update.offset == IORING_FILE_INDEX_ALLOC) {
		ret = io_files_update_with_index_alloc(req, issue_flags);
	} else {
		io_ring_submit_lock(ctx, issue_flags);
		ret = __io_register_rsrc_update(ctx, IORING_RSRC_FILE,
				&up, req->rsrc_update.nr_args);
		io_ring_submit_unlock(ctx, issue_flags);
	}

	if (ret < 0)
		req_set_fail(req);
	__io_req_complete(req, issue_flags, ret, 0);
	return 0;
}

static int io_req_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	switch (req->opcode) {
	case IORING_OP_NOP:
		return io_nop_prep(req, sqe);
	case IORING_OP_READV:
	case IORING_OP_READ_FIXED:
	case IORING_OP_READ:
	case IORING_OP_WRITEV:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_WRITE:
		return io_prep_rw(req, sqe);
	case IORING_OP_POLL_ADD:
		return io_poll_add_prep(req, sqe);
	case IORING_OP_POLL_REMOVE:
		return io_poll_remove_prep(req, sqe);
	case IORING_OP_FSYNC:
		return io_fsync_prep(req, sqe);
	case IORING_OP_SYNC_FILE_RANGE:
		return io_sfr_prep(req, sqe);
	case IORING_OP_SENDMSG:
	case IORING_OP_SEND:
		return io_sendmsg_prep(req, sqe);
	case IORING_OP_RECVMSG:
	case IORING_OP_RECV:
		return io_recvmsg_prep(req, sqe);
	case IORING_OP_CONNECT:
		return io_connect_prep(req, sqe);
	case IORING_OP_TIMEOUT:
		return io_timeout_prep(req, sqe);
	case IORING_OP_TIMEOUT_REMOVE:
		return io_timeout_remove_prep(req, sqe);
	case IORING_OP_ASYNC_CANCEL:
		return io_async_cancel_prep(req, sqe);
	case IORING_OP_LINK_TIMEOUT:
		return io_link_timeout_prep(req, sqe);
	case IORING_OP_ACCEPT:
		return io_accept_prep(req, sqe);
	case IORING_OP_FALLOCATE:
		return io_fallocate_prep(req, sqe);
	case IORING_OP_OPENAT:
		return io_openat_prep(req, sqe);
	case IORING_OP_CLOSE:
		return io_close_prep(req, sqe);
	case IORING_OP_FILES_UPDATE:
		return io_files_update_prep(req, sqe);
	case IORING_OP_STATX:
		return io_statx_prep(req, sqe);
	case IORING_OP_FADVISE:
		return io_fadvise_prep(req, sqe);
	case IORING_OP_MADVISE:
		return io_madvise_prep(req, sqe);
	case IORING_OP_OPENAT2:
		return io_openat2_prep(req, sqe);
	case IORING_OP_EPOLL_CTL:
		return io_epoll_ctl_prep(req, sqe);
	case IORING_OP_SPLICE:
		return io_splice_prep(req, sqe);
	case IORING_OP_PROVIDE_BUFFERS:
		return io_provide_buffers_prep(req, sqe);
	case IORING_OP_REMOVE_BUFFERS:
		return io_remove_buffers_prep(req, sqe);
	case IORING_OP_TEE:
		return io_tee_prep(req, sqe);
	case IORING_OP_SHUTDOWN:
		return io_shutdown_prep(req, sqe);
	case IORING_OP_RENAMEAT:
		return io_renameat_prep(req, sqe);
	case IORING_OP_UNLINKAT:
		return io_unlinkat_prep(req, sqe);
	case IORING_OP_MKDIRAT:
		return io_mkdirat_prep(req, sqe);
	case IORING_OP_SYMLINKAT:
		return io_symlinkat_prep(req, sqe);
	case IORING_OP_LINKAT:
		return io_linkat_prep(req, sqe);
	case IORING_OP_MSG_RING:
		return io_msg_ring_prep(req, sqe);
	case IORING_OP_FSETXATTR:
		return io_fsetxattr_prep(req, sqe);
	case IORING_OP_SETXATTR:
		return io_setxattr_prep(req, sqe);
	case IORING_OP_FGETXATTR:
		return io_fgetxattr_prep(req, sqe);
	case IORING_OP_GETXATTR:
		return io_getxattr_prep(req, sqe);
	case IORING_OP_SOCKET:
		return io_socket_prep(req, sqe);
	case IORING_OP_URING_CMD:
		return io_uring_cmd_prep(req, sqe);
	}

	printk_once(KERN_WARNING "io_uring: unhandled opcode %d\n",
			req->opcode);
	return -EINVAL;
}

static int io_req_prep_async(struct io_kiocb *req)
{
	const struct io_op_def *def = &io_op_defs[req->opcode];

	/* assign early for deferred execution for non-fixed file */
	if (def->needs_file && !(req->flags & REQ_F_FIXED_FILE))
		req->file = io_file_get_normal(req, req->cqe.fd);
	if (!def->needs_async_setup)
		return 0;
	if (WARN_ON_ONCE(req_has_async_data(req)))
		return -EFAULT;
	if (io_alloc_async_data(req))
		return -EAGAIN;

	switch (req->opcode) {
	case IORING_OP_READV:
		return io_readv_prep_async(req);
	case IORING_OP_WRITEV:
		return io_writev_prep_async(req);
	case IORING_OP_SENDMSG:
		return io_sendmsg_prep_async(req);
	case IORING_OP_RECVMSG:
		return io_recvmsg_prep_async(req);
	case IORING_OP_CONNECT:
		return io_connect_prep_async(req);
	case IORING_OP_URING_CMD:
		return io_uring_cmd_prep_async(req);
	}
	printk_once(KERN_WARNING "io_uring: prep_async() bad opcode %d\n",
		    req->opcode);
	return -EFAULT;
}

static u32 io_get_sequence(struct io_kiocb *req)
{
	u32 seq = req->ctx->cached_sq_head;
	struct io_kiocb *cur;

	/* need original cached_sq_head, but it was increased for each req */
	io_for_each_link(cur, req)
		seq--;
	return seq;
}

static __cold void io_drain_req(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_defer_entry *de;
	int ret;
	u32 seq = io_get_sequence(req);

	/* Still need defer if there is pending req in defer list. */
	spin_lock(&ctx->completion_lock);
	if (!req_need_defer(req, seq) && list_empty_careful(&ctx->defer_list)) {
		spin_unlock(&ctx->completion_lock);
queue:
		ctx->drain_active = false;
		io_req_task_queue(req);
		return;
	}
	spin_unlock(&ctx->completion_lock);

	ret = io_req_prep_async(req);
	if (ret) {
fail:
		io_req_complete_failed(req, ret);
		return;
	}
	io_prep_async_link(req);
	de = kmalloc(sizeof(*de), GFP_KERNEL);
	if (!de) {
		ret = -ENOMEM;
		goto fail;
	}

	spin_lock(&ctx->completion_lock);
	if (!req_need_defer(req, seq) && list_empty(&ctx->defer_list)) {
		spin_unlock(&ctx->completion_lock);
		kfree(de);
		goto queue;
	}

	trace_io_uring_defer(ctx, req, req->cqe.user_data, req->opcode);
	de->req = req;
	de->seq = seq;
	list_add_tail(&de->list, &ctx->defer_list);
	spin_unlock(&ctx->completion_lock);
}

static void io_clean_op(struct io_kiocb *req)
{
	if (req->flags & REQ_F_BUFFER_SELECTED) {
		spin_lock(&req->ctx->completion_lock);
		io_put_kbuf_comp(req);
		spin_unlock(&req->ctx->completion_lock);
	}

	if (req->flags & REQ_F_NEED_CLEANUP) {
		switch (req->opcode) {
		case IORING_OP_READV:
		case IORING_OP_READ_FIXED:
		case IORING_OP_READ:
		case IORING_OP_WRITEV:
		case IORING_OP_WRITE_FIXED:
		case IORING_OP_WRITE: {
			struct io_async_rw *io = req->async_data;

			kfree(io->free_iovec);
			break;
			}
		case IORING_OP_RECVMSG:
		case IORING_OP_SENDMSG: {
			struct io_async_msghdr *io = req->async_data;

			kfree(io->free_iov);
			break;
			}
		case IORING_OP_OPENAT:
		case IORING_OP_OPENAT2:
			if (req->open.filename)
				putname(req->open.filename);
			break;
		case IORING_OP_RENAMEAT:
			putname(req->rename.oldpath);
			putname(req->rename.newpath);
			break;
		case IORING_OP_UNLINKAT:
			putname(req->unlink.filename);
			break;
		case IORING_OP_MKDIRAT:
			putname(req->mkdir.filename);
			break;
		case IORING_OP_SYMLINKAT:
			putname(req->symlink.oldpath);
			putname(req->symlink.newpath);
			break;
		case IORING_OP_LINKAT:
			putname(req->hardlink.oldpath);
			putname(req->hardlink.newpath);
			break;
		case IORING_OP_STATX:
			if (req->statx.filename)
				putname(req->statx.filename);
			break;
		case IORING_OP_SETXATTR:
		case IORING_OP_FSETXATTR:
		case IORING_OP_GETXATTR:
		case IORING_OP_FGETXATTR:
			__io_xattr_finish(req);
			break;
		}
	}
	if ((req->flags & REQ_F_POLLED) && req->apoll) {
		kfree(req->apoll->double_poll);
		kfree(req->apoll);
		req->apoll = NULL;
	}
	if (req->flags & REQ_F_INFLIGHT) {
		struct io_uring_task *tctx = req->task->io_uring;

		atomic_dec(&tctx->inflight_tracked);
	}
	if (req->flags & REQ_F_CREDS)
		put_cred(req->creds);
	if (req->flags & REQ_F_ASYNC_DATA) {
		kfree(req->async_data);
		req->async_data = NULL;
	}
	req->flags &= ~IO_REQ_CLEAN_FLAGS;
}

static bool io_assign_file(struct io_kiocb *req, unsigned int issue_flags)
{
	if (req->file || !io_op_defs[req->opcode].needs_file)
		return true;

	if (req->flags & REQ_F_FIXED_FILE)
		req->file = io_file_get_fixed(req, req->cqe.fd, issue_flags);
	else
		req->file = io_file_get_normal(req, req->cqe.fd);

	return !!req->file;
}

static int io_issue_sqe(struct io_kiocb *req, unsigned int issue_flags)
{
	const struct io_op_def *def = &io_op_defs[req->opcode];
	const struct cred *creds = NULL;
	int ret;

	if (unlikely(!io_assign_file(req, issue_flags)))
		return -EBADF;

	if (unlikely((req->flags & REQ_F_CREDS) && req->creds != current_cred()))
		creds = override_creds(req->creds);

	if (!def->audit_skip)
		audit_uring_entry(req->opcode);

	switch (req->opcode) {
	case IORING_OP_NOP:
		ret = io_nop(req, issue_flags);
		break;
	case IORING_OP_READV:
	case IORING_OP_READ_FIXED:
	case IORING_OP_READ:
		ret = io_read(req, issue_flags);
		break;
	case IORING_OP_WRITEV:
	case IORING_OP_WRITE_FIXED:
	case IORING_OP_WRITE:
		ret = io_write(req, issue_flags);
		break;
	case IORING_OP_FSYNC:
		ret = io_fsync(req, issue_flags);
		break;
	case IORING_OP_POLL_ADD:
		ret = io_poll_add(req, issue_flags);
		break;
	case IORING_OP_POLL_REMOVE:
		ret = io_poll_remove(req, issue_flags);
		break;
	case IORING_OP_SYNC_FILE_RANGE:
		ret = io_sync_file_range(req, issue_flags);
		break;
	case IORING_OP_SENDMSG:
		ret = io_sendmsg(req, issue_flags);
		break;
	case IORING_OP_SEND:
		ret = io_send(req, issue_flags);
		break;
	case IORING_OP_RECVMSG:
		ret = io_recvmsg(req, issue_flags);
		break;
	case IORING_OP_RECV:
		ret = io_recv(req, issue_flags);
		break;
	case IORING_OP_TIMEOUT:
		ret = io_timeout(req, issue_flags);
		break;
	case IORING_OP_TIMEOUT_REMOVE:
		ret = io_timeout_remove(req, issue_flags);
		break;
	case IORING_OP_ACCEPT:
		ret = io_accept(req, issue_flags);
		break;
	case IORING_OP_CONNECT:
		ret = io_connect(req, issue_flags);
		break;
	case IORING_OP_ASYNC_CANCEL:
		ret = io_async_cancel(req, issue_flags);
		break;
	case IORING_OP_FALLOCATE:
		ret = io_fallocate(req, issue_flags);
		break;
	case IORING_OP_OPENAT:
		ret = io_openat(req, issue_flags);
		break;
	case IORING_OP_CLOSE:
		ret = io_close(req, issue_flags);
		break;
	case IORING_OP_FILES_UPDATE:
		ret = io_files_update(req, issue_flags);
		break;
	case IORING_OP_STATX:
		ret = io_statx(req, issue_flags);
		break;
	case IORING_OP_FADVISE:
		ret = io_fadvise(req, issue_flags);
		break;
	case IORING_OP_MADVISE:
		ret = io_madvise(req, issue_flags);
		break;
	case IORING_OP_OPENAT2:
		ret = io_openat2(req, issue_flags);
		break;
	case IORING_OP_EPOLL_CTL:
		ret = io_epoll_ctl(req, issue_flags);
		break;
	case IORING_OP_SPLICE:
		ret = io_splice(req, issue_flags);
		break;
	case IORING_OP_PROVIDE_BUFFERS:
		ret = io_provide_buffers(req, issue_flags);
		break;
	case IORING_OP_REMOVE_BUFFERS:
		ret = io_remove_buffers(req, issue_flags);
		break;
	case IORING_OP_TEE:
		ret = io_tee(req, issue_flags);
		break;
	case IORING_OP_SHUTDOWN:
		ret = io_shutdown(req, issue_flags);
		break;
	case IORING_OP_RENAMEAT:
		ret = io_renameat(req, issue_flags);
		break;
	case IORING_OP_UNLINKAT:
		ret = io_unlinkat(req, issue_flags);
		break;
	case IORING_OP_MKDIRAT:
		ret = io_mkdirat(req, issue_flags);
		break;
	case IORING_OP_SYMLINKAT:
		ret = io_symlinkat(req, issue_flags);
		break;
	case IORING_OP_LINKAT:
		ret = io_linkat(req, issue_flags);
		break;
	case IORING_OP_MSG_RING:
		ret = io_msg_ring(req, issue_flags);
		break;
	case IORING_OP_FSETXATTR:
		ret = io_fsetxattr(req, issue_flags);
		break;
	case IORING_OP_SETXATTR:
		ret = io_setxattr(req, issue_flags);
		break;
	case IORING_OP_FGETXATTR:
		ret = io_fgetxattr(req, issue_flags);
		break;
	case IORING_OP_GETXATTR:
		ret = io_getxattr(req, issue_flags);
		break;
	case IORING_OP_SOCKET:
		ret = io_socket(req, issue_flags);
		break;
	case IORING_OP_URING_CMD:
		ret = io_uring_cmd(req, issue_flags);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (!def->audit_skip)
		audit_uring_exit(!ret, ret);

	if (creds)
		revert_creds(creds);
	if (ret)
		return ret;
	/* If the op doesn't have a file, we're not polling for it */
	if ((req->ctx->flags & IORING_SETUP_IOPOLL) && req->file)
		io_iopoll_req_issued(req, issue_flags);

	return 0;
}

static struct io_wq_work *io_wq_free_work(struct io_wq_work *work)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);

	req = io_put_req_find_next(req);
	return req ? &req->work : NULL;
}

static void io_wq_submit_work(struct io_wq_work *work)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);
	const struct io_op_def *def = &io_op_defs[req->opcode];
	unsigned int issue_flags = IO_URING_F_UNLOCKED;
	bool needs_poll = false;
	int ret = 0, err = -ECANCELED;

	/* one will be dropped by ->io_free_work() after returning to io-wq */
	if (!(req->flags & REQ_F_REFCOUNT))
		__io_req_set_refcount(req, 2);
	else
		req_ref_get(req);

	io_arm_ltimeout(req);

	/* either cancelled or io-wq is dying, so don't touch tctx->iowq */
	if (work->flags & IO_WQ_WORK_CANCEL) {
fail:
		io_req_task_queue_fail(req, err);
		return;
	}
	if (!io_assign_file(req, issue_flags)) {
		err = -EBADF;
		work->flags |= IO_WQ_WORK_CANCEL;
		goto fail;
	}

	if (req->flags & REQ_F_FORCE_ASYNC) {
		bool opcode_poll = def->pollin || def->pollout;

		if (opcode_poll && file_can_poll(req->file)) {
			needs_poll = true;
			issue_flags |= IO_URING_F_NONBLOCK;
		}
	}

	do {
		ret = io_issue_sqe(req, issue_flags);
		if (ret != -EAGAIN)
			break;
		/*
		 * We can get EAGAIN for iopolled IO even though we're
		 * forcing a sync submission from here, since we can't
		 * wait for request slots on the block side.
		 */
		if (!needs_poll) {
			if (!(req->ctx->flags & IORING_SETUP_IOPOLL))
				break;
			cond_resched();
			continue;
		}

		if (io_arm_poll_handler(req, issue_flags) == IO_APOLL_OK)
			return;
		/* aborted or ready, in either case retry blocking */
		needs_poll = false;
		issue_flags &= ~IO_URING_F_NONBLOCK;
	} while (1);

	/* avoid locking problems by failing it from a clean context */
	if (ret)
		io_req_task_queue_fail(req, ret);
}

static inline struct io_fixed_file *io_fixed_file_slot(struct io_file_table *table,
						       unsigned i)
{
	return &table->files[i];
}

static inline struct file *io_file_from_index(struct io_ring_ctx *ctx,
					      int index)
{
	struct io_fixed_file *slot = io_fixed_file_slot(&ctx->file_table, index);

	return (struct file *) (slot->file_ptr & FFS_MASK);
}

static void io_fixed_file_set(struct io_fixed_file *file_slot, struct file *file)
{
	unsigned long file_ptr = (unsigned long) file;

	file_ptr |= io_file_get_flags(file);
	file_slot->file_ptr = file_ptr;
}

static inline struct file *io_file_get_fixed(struct io_kiocb *req, int fd,
					     unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct file *file = NULL;
	unsigned long file_ptr;

	io_ring_submit_lock(ctx, issue_flags);

	if (unlikely((unsigned int)fd >= ctx->nr_user_files))
		goto out;
	fd = array_index_nospec(fd, ctx->nr_user_files);
	file_ptr = io_fixed_file_slot(&ctx->file_table, fd)->file_ptr;
	file = (struct file *) (file_ptr & FFS_MASK);
	file_ptr &= ~FFS_MASK;
	/* mask in overlapping REQ_F and FFS bits */
	req->flags |= (file_ptr << REQ_F_SUPPORT_NOWAIT_BIT);
	io_req_set_rsrc_node(req, ctx, 0);
	WARN_ON_ONCE(file && !test_bit(fd, ctx->file_table.bitmap));
out:
	io_ring_submit_unlock(ctx, issue_flags);
	return file;
}

static struct file *io_file_get_normal(struct io_kiocb *req, int fd)
{
	struct file *file = fget(fd);

	trace_io_uring_file_get(req->ctx, req, req->cqe.user_data, fd);

	/* we don't allow fixed io_uring files */
	if (file && file->f_op == &io_uring_fops)
		io_req_track_inflight(req);
	return file;
}

static void io_req_task_link_timeout(struct io_kiocb *req, bool *locked)
{
	struct io_kiocb *prev = req->timeout.prev;
	int ret = -ENOENT;

	if (prev) {
		if (!(req->task->flags & PF_EXITING)) {
			struct io_cancel_data cd = {
				.ctx		= req->ctx,
				.data		= prev->cqe.user_data,
			};

			ret = io_try_cancel(req, &cd);
		}
		io_req_complete_post(req, ret ?: -ETIME, 0);
		io_put_req(prev);
	} else {
		io_req_complete_post(req, -ETIME, 0);
	}
}

static enum hrtimer_restart io_link_timeout_fn(struct hrtimer *timer)
{
	struct io_timeout_data *data = container_of(timer,
						struct io_timeout_data, timer);
	struct io_kiocb *prev, *req = data->req;
	struct io_ring_ctx *ctx = req->ctx;
	unsigned long flags;

	spin_lock_irqsave(&ctx->timeout_lock, flags);
	prev = req->timeout.head;
	req->timeout.head = NULL;

	/*
	 * We don't expect the list to be empty, that will only happen if we
	 * race with the completion of the linked work.
	 */
	if (prev) {
		io_remove_next_linked(prev);
		if (!req_ref_inc_not_zero(prev))
			prev = NULL;
	}
	list_del(&req->timeout.list);
	req->timeout.prev = prev;
	spin_unlock_irqrestore(&ctx->timeout_lock, flags);

	req->io_task_work.func = io_req_task_link_timeout;
	io_req_task_work_add(req);
	return HRTIMER_NORESTART;
}

static void io_queue_linked_timeout(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;

	spin_lock_irq(&ctx->timeout_lock);
	/*
	 * If the back reference is NULL, then our linked request finished
	 * before we got a chance to setup the timer
	 */
	if (req->timeout.head) {
		struct io_timeout_data *data = req->async_data;

		data->timer.function = io_link_timeout_fn;
		hrtimer_start(&data->timer, timespec64_to_ktime(data->ts),
				data->mode);
		list_add_tail(&req->timeout.list, &ctx->ltimeout_list);
	}
	spin_unlock_irq(&ctx->timeout_lock);
	/* drop submission reference */
	io_put_req(req);
}

static void io_queue_async(struct io_kiocb *req, int ret)
	__must_hold(&req->ctx->uring_lock)
{
	struct io_kiocb *linked_timeout;

	if (ret != -EAGAIN || (req->flags & REQ_F_NOWAIT)) {
		io_req_complete_failed(req, ret);
		return;
	}

	linked_timeout = io_prep_linked_timeout(req);

	switch (io_arm_poll_handler(req, 0)) {
	case IO_APOLL_READY:
		io_req_task_queue(req);
		break;
	case IO_APOLL_ABORTED:
		/*
		 * Queued up for async execution, worker will release
		 * submit reference when the iocb is actually submitted.
		 */
		io_kbuf_recycle(req, 0);
		io_queue_iowq(req, NULL);
		break;
	case IO_APOLL_OK:
		break;
	}

	if (linked_timeout)
		io_queue_linked_timeout(linked_timeout);
}

static inline void io_queue_sqe(struct io_kiocb *req)
	__must_hold(&req->ctx->uring_lock)
{
	int ret;

	ret = io_issue_sqe(req, IO_URING_F_NONBLOCK|IO_URING_F_COMPLETE_DEFER);

	if (req->flags & REQ_F_COMPLETE_INLINE) {
		io_req_add_compl_list(req);
		return;
	}
	/*
	 * We async punt it if the file wasn't marked NOWAIT, or if the file
	 * doesn't support non-blocking read/write attempts
	 */
	if (likely(!ret))
		io_arm_ltimeout(req);
	else
		io_queue_async(req, ret);
}

static void io_queue_sqe_fallback(struct io_kiocb *req)
	__must_hold(&req->ctx->uring_lock)
{
	if (unlikely(req->flags & REQ_F_FAIL)) {
		/*
		 * We don't submit, fail them all, for that replace hardlinks
		 * with normal links. Extra REQ_F_LINK is tolerated.
		 */
		req->flags &= ~REQ_F_HARDLINK;
		req->flags |= REQ_F_LINK;
		io_req_complete_failed(req, req->cqe.res);
	} else if (unlikely(req->ctx->drain_active)) {
		io_drain_req(req);
	} else {
		int ret = io_req_prep_async(req);

		if (unlikely(ret))
			io_req_complete_failed(req, ret);
		else
			io_queue_iowq(req, NULL);
	}
}

/*
 * Check SQE restrictions (opcode and flags).
 *
 * Returns 'true' if SQE is allowed, 'false' otherwise.
 */
static inline bool io_check_restriction(struct io_ring_ctx *ctx,
					struct io_kiocb *req,
					unsigned int sqe_flags)
{
	if (!test_bit(req->opcode, ctx->restrictions.sqe_op))
		return false;

	if ((sqe_flags & ctx->restrictions.sqe_flags_required) !=
	    ctx->restrictions.sqe_flags_required)
		return false;

	if (sqe_flags & ~(ctx->restrictions.sqe_flags_allowed |
			  ctx->restrictions.sqe_flags_required))
		return false;

	return true;
}

static void io_init_req_drain(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_kiocb *head = ctx->submit_state.link.head;

	ctx->drain_active = true;
	if (head) {
		/*
		 * If we need to drain a request in the middle of a link, drain
		 * the head request and the next request/link after the current
		 * link. Considering sequential execution of links,
		 * REQ_F_IO_DRAIN will be maintained for every request of our
		 * link.
		 */
		head->flags |= REQ_F_IO_DRAIN | REQ_F_FORCE_ASYNC;
		ctx->drain_next = true;
	}
}

static int io_init_req(struct io_ring_ctx *ctx, struct io_kiocb *req,
		       const struct io_uring_sqe *sqe)
	__must_hold(&ctx->uring_lock)
{
	const struct io_op_def *def;
	unsigned int sqe_flags;
	int personality;
	u8 opcode;

	/* req is partially pre-initialised, see io_preinit_req() */
	req->opcode = opcode = READ_ONCE(sqe->opcode);
	/* same numerical values with corresponding REQ_F_*, safe to copy */
	req->flags = sqe_flags = READ_ONCE(sqe->flags);
	req->cqe.user_data = READ_ONCE(sqe->user_data);
	req->file = NULL;
	req->rsrc_node = NULL;
	req->task = current;

	if (unlikely(opcode >= IORING_OP_LAST)) {
		req->opcode = 0;
		return -EINVAL;
	}
	def = &io_op_defs[opcode];
	if (unlikely(sqe_flags & ~SQE_COMMON_FLAGS)) {
		/* enforce forwards compatibility on users */
		if (sqe_flags & ~SQE_VALID_FLAGS)
			return -EINVAL;
		if (sqe_flags & IOSQE_BUFFER_SELECT) {
			if (!def->buffer_select)
				return -EOPNOTSUPP;
			req->buf_index = READ_ONCE(sqe->buf_group);
		}
		if (sqe_flags & IOSQE_CQE_SKIP_SUCCESS)
			ctx->drain_disabled = true;
		if (sqe_flags & IOSQE_IO_DRAIN) {
			if (ctx->drain_disabled)
				return -EOPNOTSUPP;
			io_init_req_drain(req);
		}
	}
	if (unlikely(ctx->restricted || ctx->drain_active || ctx->drain_next)) {
		if (ctx->restricted && !io_check_restriction(ctx, req, sqe_flags))
			return -EACCES;
		/* knock it to the slow queue path, will be drained there */
		if (ctx->drain_active)
			req->flags |= REQ_F_FORCE_ASYNC;
		/* if there is no link, we're at "next" request and need to drain */
		if (unlikely(ctx->drain_next) && !ctx->submit_state.link.head) {
			ctx->drain_next = false;
			ctx->drain_active = true;
			req->flags |= REQ_F_IO_DRAIN | REQ_F_FORCE_ASYNC;
		}
	}

	if (!def->ioprio && sqe->ioprio)
		return -EINVAL;
	if (!def->iopoll && (ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;

	if (def->needs_file) {
		struct io_submit_state *state = &ctx->submit_state;

		req->cqe.fd = READ_ONCE(sqe->fd);

		/*
		 * Plug now if we have more than 2 IO left after this, and the
		 * target is potentially a read/write to block based storage.
		 */
		if (state->need_plug && def->plug) {
			state->plug_started = true;
			state->need_plug = false;
			blk_start_plug_nr_ios(&state->plug, state->submit_nr);
		}
	}

	personality = READ_ONCE(sqe->personality);
	if (personality) {
		int ret;

		req->creds = xa_load(&ctx->personalities, personality);
		if (!req->creds)
			return -EINVAL;
		get_cred(req->creds);
		ret = security_uring_override_creds(req->creds);
		if (ret) {
			put_cred(req->creds);
			return ret;
		}
		req->flags |= REQ_F_CREDS;
	}

	return io_req_prep(req, sqe);
}

static __cold int io_submit_fail_init(const struct io_uring_sqe *sqe,
				      struct io_kiocb *req, int ret)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_submit_link *link = &ctx->submit_state.link;
	struct io_kiocb *head = link->head;

	trace_io_uring_req_failed(sqe, ctx, req, ret);

	/*
	 * Avoid breaking links in the middle as it renders links with SQPOLL
	 * unusable. Instead of failing eagerly, continue assembling the link if
	 * applicable and mark the head with REQ_F_FAIL. The link flushing code
	 * should find the flag and handle the rest.
	 */
	req_fail_link_node(req, ret);
	if (head && !(head->flags & REQ_F_FAIL))
		req_fail_link_node(head, -ECANCELED);

	if (!(req->flags & IO_REQ_LINK_FLAGS)) {
		if (head) {
			link->last->link = req;
			link->head = NULL;
			req = head;
		}
		io_queue_sqe_fallback(req);
		return ret;
	}

	if (head)
		link->last->link = req;
	else
		link->head = req;
	link->last = req;
	return 0;
}

static inline int io_submit_sqe(struct io_ring_ctx *ctx, struct io_kiocb *req,
			 const struct io_uring_sqe *sqe)
	__must_hold(&ctx->uring_lock)
{
	struct io_submit_link *link = &ctx->submit_state.link;
	int ret;

	ret = io_init_req(ctx, req, sqe);
	if (unlikely(ret))
		return io_submit_fail_init(sqe, req, ret);

	/* don't need @sqe from now on */
	trace_io_uring_submit_sqe(ctx, req, req->cqe.user_data, req->opcode,
				  req->flags, true,
				  ctx->flags & IORING_SETUP_SQPOLL);

	/*
	 * If we already have a head request, queue this one for async
	 * submittal once the head completes. If we don't have a head but
	 * IOSQE_IO_LINK is set in the sqe, start a new head. This one will be
	 * submitted sync once the chain is complete. If none of those
	 * conditions are true (normal request), then just queue it.
	 */
	if (unlikely(link->head)) {
		ret = io_req_prep_async(req);
		if (unlikely(ret))
			return io_submit_fail_init(sqe, req, ret);

		trace_io_uring_link(ctx, req, link->head);
		link->last->link = req;
		link->last = req;

		if (req->flags & IO_REQ_LINK_FLAGS)
			return 0;
		/* last request of the link, flush it */
		req = link->head;
		link->head = NULL;
		if (req->flags & (REQ_F_FORCE_ASYNC | REQ_F_FAIL))
			goto fallback;

	} else if (unlikely(req->flags & (IO_REQ_LINK_FLAGS |
					  REQ_F_FORCE_ASYNC | REQ_F_FAIL))) {
		if (req->flags & IO_REQ_LINK_FLAGS) {
			link->head = req;
			link->last = req;
		} else {
fallback:
			io_queue_sqe_fallback(req);
		}
		return 0;
	}

	io_queue_sqe(req);
	return 0;
}

/*
 * Batched submission is done, ensure local IO is flushed out.
 */
static void io_submit_state_end(struct io_ring_ctx *ctx)
{
	struct io_submit_state *state = &ctx->submit_state;

	if (unlikely(state->link.head))
		io_queue_sqe_fallback(state->link.head);
	/* flush only after queuing links as they can generate completions */
	io_submit_flush_completions(ctx);
	if (state->plug_started)
		blk_finish_plug(&state->plug);
}

/*
 * Start submission side cache.
 */
static void io_submit_state_start(struct io_submit_state *state,
				  unsigned int max_ios)
{
	state->plug_started = false;
	state->need_plug = max_ios > 2;
	state->submit_nr = max_ios;
	/* set only head, no need to init link_last in advance */
	state->link.head = NULL;
}

static void io_commit_sqring(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;

	/*
	 * Ensure any loads from the SQEs are done at this point,
	 * since once we write the new head, the application could
	 * write new data to them.
	 */
	smp_store_release(&rings->sq.head, ctx->cached_sq_head);
}

/*
 * Fetch an sqe, if one is available. Note this returns a pointer to memory
 * that is mapped by userspace. This means that care needs to be taken to
 * ensure that reads are stable, as we cannot rely on userspace always
 * being a good citizen. If members of the sqe are validated and then later
 * used, it's important that those reads are done through READ_ONCE() to
 * prevent a re-load down the line.
 */
static const struct io_uring_sqe *io_get_sqe(struct io_ring_ctx *ctx)
{
	unsigned head, mask = ctx->sq_entries - 1;
	unsigned sq_idx = ctx->cached_sq_head++ & mask;

	/*
	 * The cached sq head (or cq tail) serves two purposes:
	 *
	 * 1) allows us to batch the cost of updating the user visible
	 *    head updates.
	 * 2) allows the kernel side to track the head on its own, even
	 *    though the application is the one updating it.
	 */
	head = READ_ONCE(ctx->sq_array[sq_idx]);
	if (likely(head < ctx->sq_entries)) {
		/* double index for 128-byte SQEs, twice as long */
		if (ctx->flags & IORING_SETUP_SQE128)
			head <<= 1;
		return &ctx->sq_sqes[head];
	}

	/* drop invalid entries */
	ctx->cq_extra--;
	WRITE_ONCE(ctx->rings->sq_dropped,
		   READ_ONCE(ctx->rings->sq_dropped) + 1);
	return NULL;
}

static int io_submit_sqes(struct io_ring_ctx *ctx, unsigned int nr)
	__must_hold(&ctx->uring_lock)
{
	unsigned int entries = io_sqring_entries(ctx);
	unsigned int left;
	int ret;

	if (unlikely(!entries))
		return 0;
	/* make sure SQ entry isn't read before tail */
	ret = left = min3(nr, ctx->sq_entries, entries);
	io_get_task_refs(left);
	io_submit_state_start(&ctx->submit_state, left);

	do {
		const struct io_uring_sqe *sqe;
		struct io_kiocb *req;

		if (unlikely(!io_alloc_req_refill(ctx)))
			break;
		req = io_alloc_req(ctx);
		sqe = io_get_sqe(ctx);
		if (unlikely(!sqe)) {
			io_req_add_to_cache(req, ctx);
			break;
		}

		/*
		 * Continue submitting even for sqe failure if the
		 * ring was setup with IORING_SETUP_SUBMIT_ALL
		 */
		if (unlikely(io_submit_sqe(ctx, req, sqe)) &&
		    !(ctx->flags & IORING_SETUP_SUBMIT_ALL)) {
			left--;
			break;
		}
	} while (--left);

	if (unlikely(left)) {
		ret -= left;
		/* try again if it submitted nothing and can't allocate a req */
		if (!ret && io_req_cache_empty(ctx))
			ret = -EAGAIN;
		current->io_uring->cached_refs += left;
	}

	io_submit_state_end(ctx);
	 /* Commit SQ ring head once we've consumed and submitted all SQEs */
	io_commit_sqring(ctx);
	return ret;
}

static inline bool io_sqd_events_pending(struct io_sq_data *sqd)
{
	return READ_ONCE(sqd->state);
}

static int __io_sq_thread(struct io_ring_ctx *ctx, bool cap_entries)
{
	unsigned int to_submit;
	int ret = 0;

	to_submit = io_sqring_entries(ctx);
	/* if we're handling multiple rings, cap submit size for fairness */
	if (cap_entries && to_submit > IORING_SQPOLL_CAP_ENTRIES_VALUE)
		to_submit = IORING_SQPOLL_CAP_ENTRIES_VALUE;

	if (!wq_list_empty(&ctx->iopoll_list) || to_submit) {
		const struct cred *creds = NULL;

		if (ctx->sq_creds != current_cred())
			creds = override_creds(ctx->sq_creds);

		mutex_lock(&ctx->uring_lock);
		if (!wq_list_empty(&ctx->iopoll_list))
			io_do_iopoll(ctx, true);

		/*
		 * Don't submit if refs are dying, good for io_uring_register(),
		 * but also it is relied upon by io_ring_exit_work()
		 */
		if (to_submit && likely(!percpu_ref_is_dying(&ctx->refs)) &&
		    !(ctx->flags & IORING_SETUP_R_DISABLED))
			ret = io_submit_sqes(ctx, to_submit);
		mutex_unlock(&ctx->uring_lock);

		if (to_submit && wq_has_sleeper(&ctx->sqo_sq_wait))
			wake_up(&ctx->sqo_sq_wait);
		if (creds)
			revert_creds(creds);
	}

	return ret;
}

static __cold void io_sqd_update_thread_idle(struct io_sq_data *sqd)
{
	struct io_ring_ctx *ctx;
	unsigned sq_thread_idle = 0;

	list_for_each_entry(ctx, &sqd->ctx_list, sqd_list)
		sq_thread_idle = max(sq_thread_idle, ctx->sq_thread_idle);
	sqd->sq_thread_idle = sq_thread_idle;
}

static bool io_sqd_handle_event(struct io_sq_data *sqd)
{
	bool did_sig = false;
	struct ksignal ksig;

	if (test_bit(IO_SQ_THREAD_SHOULD_PARK, &sqd->state) ||
	    signal_pending(current)) {
		mutex_unlock(&sqd->lock);
		if (signal_pending(current))
			did_sig = get_signal(&ksig);
		cond_resched();
		mutex_lock(&sqd->lock);
	}
	return did_sig || test_bit(IO_SQ_THREAD_SHOULD_STOP, &sqd->state);
}

static int io_sq_thread(void *data)
{
	struct io_sq_data *sqd = data;
	struct io_ring_ctx *ctx;
	unsigned long timeout = 0;
	char buf[TASK_COMM_LEN];
	DEFINE_WAIT(wait);

	snprintf(buf, sizeof(buf), "iou-sqp-%d", sqd->task_pid);
	set_task_comm(current, buf);

	if (sqd->sq_cpu != -1)
		set_cpus_allowed_ptr(current, cpumask_of(sqd->sq_cpu));
	else
		set_cpus_allowed_ptr(current, cpu_online_mask);
	current->flags |= PF_NO_SETAFFINITY;

	audit_alloc_kernel(current);

	mutex_lock(&sqd->lock);
	while (1) {
		bool cap_entries, sqt_spin = false;

		if (io_sqd_events_pending(sqd) || signal_pending(current)) {
			if (io_sqd_handle_event(sqd))
				break;
			timeout = jiffies + sqd->sq_thread_idle;
		}

		cap_entries = !list_is_singular(&sqd->ctx_list);
		list_for_each_entry(ctx, &sqd->ctx_list, sqd_list) {
			int ret = __io_sq_thread(ctx, cap_entries);

			if (!sqt_spin && (ret > 0 || !wq_list_empty(&ctx->iopoll_list)))
				sqt_spin = true;
		}
		if (io_run_task_work())
			sqt_spin = true;

		if (sqt_spin || !time_after(jiffies, timeout)) {
			cond_resched();
			if (sqt_spin)
				timeout = jiffies + sqd->sq_thread_idle;
			continue;
		}

		prepare_to_wait(&sqd->wait, &wait, TASK_INTERRUPTIBLE);
		if (!io_sqd_events_pending(sqd) && !task_work_pending(current)) {
			bool needs_sched = true;

			list_for_each_entry(ctx, &sqd->ctx_list, sqd_list) {
				atomic_or(IORING_SQ_NEED_WAKEUP,
						&ctx->rings->sq_flags);
				if ((ctx->flags & IORING_SETUP_IOPOLL) &&
				    !wq_list_empty(&ctx->iopoll_list)) {
					needs_sched = false;
					break;
				}

				/*
				 * Ensure the store of the wakeup flag is not
				 * reordered with the load of the SQ tail
				 */
				smp_mb__after_atomic();

				if (io_sqring_entries(ctx)) {
					needs_sched = false;
					break;
				}
			}

			if (needs_sched) {
				mutex_unlock(&sqd->lock);
				schedule();
				mutex_lock(&sqd->lock);
			}
			list_for_each_entry(ctx, &sqd->ctx_list, sqd_list)
				atomic_andnot(IORING_SQ_NEED_WAKEUP,
						&ctx->rings->sq_flags);
		}

		finish_wait(&sqd->wait, &wait);
		timeout = jiffies + sqd->sq_thread_idle;
	}

	io_uring_cancel_generic(true, sqd);
	sqd->thread = NULL;
	list_for_each_entry(ctx, &sqd->ctx_list, sqd_list)
		atomic_or(IORING_SQ_NEED_WAKEUP, &ctx->rings->sq_flags);
	io_run_task_work();
	mutex_unlock(&sqd->lock);

	audit_free(current);

	complete(&sqd->exited);
	do_exit(0);
}

struct io_wait_queue {
	struct wait_queue_entry wq;
	struct io_ring_ctx *ctx;
	unsigned cq_tail;
	unsigned nr_timeouts;
};

static inline bool io_should_wake(struct io_wait_queue *iowq)
{
	struct io_ring_ctx *ctx = iowq->ctx;
	int dist = ctx->cached_cq_tail - (int) iowq->cq_tail;

	/*
	 * Wake up if we have enough events, or if a timeout occurred since we
	 * started waiting. For timeouts, we always want to return to userspace,
	 * regardless of event count.
	 */
	return dist >= 0 || atomic_read(&ctx->cq_timeouts) != iowq->nr_timeouts;
}

static int io_wake_function(struct wait_queue_entry *curr, unsigned int mode,
			    int wake_flags, void *key)
{
	struct io_wait_queue *iowq = container_of(curr, struct io_wait_queue,
							wq);

	/*
	 * Cannot safely flush overflowed CQEs from here, ensure we wake up
	 * the task, and the next invocation will do it.
	 */
	if (io_should_wake(iowq) ||
	    test_bit(IO_CHECK_CQ_OVERFLOW_BIT, &iowq->ctx->check_cq))
		return autoremove_wake_function(curr, mode, wake_flags, key);
	return -1;
}

static int io_run_task_work_sig(void)
{
	if (io_run_task_work())
		return 1;
	if (test_thread_flag(TIF_NOTIFY_SIGNAL))
		return -ERESTARTSYS;
	if (task_sigpending(current))
		return -EINTR;
	return 0;
}

/* when returns >0, the caller should retry */
static inline int io_cqring_wait_schedule(struct io_ring_ctx *ctx,
					  struct io_wait_queue *iowq,
					  ktime_t timeout)
{
	int ret;
	unsigned long check_cq;

	/* make sure we run task_work before checking for signals */
	ret = io_run_task_work_sig();
	if (ret || io_should_wake(iowq))
		return ret;
	check_cq = READ_ONCE(ctx->check_cq);
	/* let the caller flush overflows, retry */
	if (check_cq & BIT(IO_CHECK_CQ_OVERFLOW_BIT))
		return 1;
	if (unlikely(check_cq & BIT(IO_CHECK_CQ_DROPPED_BIT)))
		return -EBADR;
	if (!schedule_hrtimeout(&timeout, HRTIMER_MODE_ABS))
		return -ETIME;
	return 1;
}

/*
 * Wait until events become available, if we don't already have some. The
 * application must reap them itself, as they reside on the shared cq ring.
 */
static int io_cqring_wait(struct io_ring_ctx *ctx, int min_events,
			  const sigset_t __user *sig, size_t sigsz,
			  struct __kernel_timespec __user *uts)
{
	struct io_wait_queue iowq;
	struct io_rings *rings = ctx->rings;
	ktime_t timeout = KTIME_MAX;
	int ret;

	do {
		io_cqring_overflow_flush(ctx);
		if (io_cqring_events(ctx) >= min_events)
			return 0;
		if (!io_run_task_work())
			break;
	} while (1);

	if (sig) {
#ifdef CONFIG_COMPAT
		if (in_compat_syscall())
			ret = set_compat_user_sigmask((const compat_sigset_t __user *)sig,
						      sigsz);
		else
#endif
			ret = set_user_sigmask(sig, sigsz);

		if (ret)
			return ret;
	}

	if (uts) {
		struct timespec64 ts;

		if (get_timespec64(&ts, uts))
			return -EFAULT;
		timeout = ktime_add_ns(timespec64_to_ktime(ts), ktime_get_ns());
	}

	init_waitqueue_func_entry(&iowq.wq, io_wake_function);
	iowq.wq.private = current;
	INIT_LIST_HEAD(&iowq.wq.entry);
	iowq.ctx = ctx;
	iowq.nr_timeouts = atomic_read(&ctx->cq_timeouts);
	iowq.cq_tail = READ_ONCE(ctx->rings->cq.head) + min_events;

	trace_io_uring_cqring_wait(ctx, min_events);
	do {
		/* if we can't even flush overflow, don't wait for more */
		if (!io_cqring_overflow_flush(ctx)) {
			ret = -EBUSY;
			break;
		}
		prepare_to_wait_exclusive(&ctx->cq_wait, &iowq.wq,
						TASK_INTERRUPTIBLE);
		ret = io_cqring_wait_schedule(ctx, &iowq, timeout);
		cond_resched();
	} while (ret > 0);

	finish_wait(&ctx->cq_wait, &iowq.wq);
	restore_saved_sigmask_unless(ret == -EINTR);

	return READ_ONCE(rings->cq.head) == READ_ONCE(rings->cq.tail) ? ret : 0;
}

static void io_free_page_table(void **table, size_t size)
{
	unsigned i, nr_tables = DIV_ROUND_UP(size, PAGE_SIZE);

	for (i = 0; i < nr_tables; i++)
		kfree(table[i]);
	kfree(table);
}

static __cold void **io_alloc_page_table(size_t size)
{
	unsigned i, nr_tables = DIV_ROUND_UP(size, PAGE_SIZE);
	size_t init_size = size;
	void **table;

	table = kcalloc(nr_tables, sizeof(*table), GFP_KERNEL_ACCOUNT);
	if (!table)
		return NULL;

	for (i = 0; i < nr_tables; i++) {
		unsigned int this_size = min_t(size_t, size, PAGE_SIZE);

		table[i] = kzalloc(this_size, GFP_KERNEL_ACCOUNT);
		if (!table[i]) {
			io_free_page_table(table, init_size);
			return NULL;
		}
		size -= this_size;
	}
	return table;
}

static void io_rsrc_node_destroy(struct io_rsrc_node *ref_node)
{
	percpu_ref_exit(&ref_node->refs);
	kfree(ref_node);
}

static __cold void io_rsrc_node_ref_zero(struct percpu_ref *ref)
{
	struct io_rsrc_node *node = container_of(ref, struct io_rsrc_node, refs);
	struct io_ring_ctx *ctx = node->rsrc_data->ctx;
	unsigned long flags;
	bool first_add = false;
	unsigned long delay = HZ;

	spin_lock_irqsave(&ctx->rsrc_ref_lock, flags);
	node->done = true;

	/* if we are mid-quiesce then do not delay */
	if (node->rsrc_data->quiesce)
		delay = 0;

	while (!list_empty(&ctx->rsrc_ref_list)) {
		node = list_first_entry(&ctx->rsrc_ref_list,
					    struct io_rsrc_node, node);
		/* recycle ref nodes in order */
		if (!node->done)
			break;
		list_del(&node->node);
		first_add |= llist_add(&node->llist, &ctx->rsrc_put_llist);
	}
	spin_unlock_irqrestore(&ctx->rsrc_ref_lock, flags);

	if (first_add)
		mod_delayed_work(system_wq, &ctx->rsrc_put_work, delay);
}

static struct io_rsrc_node *io_rsrc_node_alloc(void)
{
	struct io_rsrc_node *ref_node;

	ref_node = kzalloc(sizeof(*ref_node), GFP_KERNEL);
	if (!ref_node)
		return NULL;

	if (percpu_ref_init(&ref_node->refs, io_rsrc_node_ref_zero,
			    0, GFP_KERNEL)) {
		kfree(ref_node);
		return NULL;
	}
	INIT_LIST_HEAD(&ref_node->node);
	INIT_LIST_HEAD(&ref_node->rsrc_list);
	ref_node->done = false;
	return ref_node;
}

static void io_rsrc_node_switch(struct io_ring_ctx *ctx,
				struct io_rsrc_data *data_to_kill)
	__must_hold(&ctx->uring_lock)
{
	WARN_ON_ONCE(!ctx->rsrc_backup_node);
	WARN_ON_ONCE(data_to_kill && !ctx->rsrc_node);

	io_rsrc_refs_drop(ctx);

	if (data_to_kill) {
		struct io_rsrc_node *rsrc_node = ctx->rsrc_node;

		rsrc_node->rsrc_data = data_to_kill;
		spin_lock_irq(&ctx->rsrc_ref_lock);
		list_add_tail(&rsrc_node->node, &ctx->rsrc_ref_list);
		spin_unlock_irq(&ctx->rsrc_ref_lock);

		atomic_inc(&data_to_kill->refs);
		percpu_ref_kill(&rsrc_node->refs);
		ctx->rsrc_node = NULL;
	}

	if (!ctx->rsrc_node) {
		ctx->rsrc_node = ctx->rsrc_backup_node;
		ctx->rsrc_backup_node = NULL;
	}
}

static int io_rsrc_node_switch_start(struct io_ring_ctx *ctx)
{
	if (ctx->rsrc_backup_node)
		return 0;
	ctx->rsrc_backup_node = io_rsrc_node_alloc();
	return ctx->rsrc_backup_node ? 0 : -ENOMEM;
}

static __cold int io_rsrc_ref_quiesce(struct io_rsrc_data *data,
				      struct io_ring_ctx *ctx)
{
	int ret;

	/* As we may drop ->uring_lock, other task may have started quiesce */
	if (data->quiesce)
		return -ENXIO;

	data->quiesce = true;
	do {
		ret = io_rsrc_node_switch_start(ctx);
		if (ret)
			break;
		io_rsrc_node_switch(ctx, data);

		/* kill initial ref, already quiesced if zero */
		if (atomic_dec_and_test(&data->refs))
			break;
		mutex_unlock(&ctx->uring_lock);
		flush_delayed_work(&ctx->rsrc_put_work);
		ret = wait_for_completion_interruptible(&data->done);
		if (!ret) {
			mutex_lock(&ctx->uring_lock);
			if (atomic_read(&data->refs) > 0) {
				/*
				 * it has been revived by another thread while
				 * we were unlocked
				 */
				mutex_unlock(&ctx->uring_lock);
			} else {
				break;
			}
		}

		atomic_inc(&data->refs);
		/* wait for all works potentially completing data->done */
		flush_delayed_work(&ctx->rsrc_put_work);
		reinit_completion(&data->done);

		ret = io_run_task_work_sig();
		mutex_lock(&ctx->uring_lock);
	} while (ret >= 0);
	data->quiesce = false;

	return ret;
}

static u64 *io_get_tag_slot(struct io_rsrc_data *data, unsigned int idx)
{
	unsigned int off = idx & IO_RSRC_TAG_TABLE_MASK;
	unsigned int table_idx = idx >> IO_RSRC_TAG_TABLE_SHIFT;

	return &data->tags[table_idx][off];
}

static void io_rsrc_data_free(struct io_rsrc_data *data)
{
	size_t size = data->nr * sizeof(data->tags[0][0]);

	if (data->tags)
		io_free_page_table((void **)data->tags, size);
	kfree(data);
}

static __cold int io_rsrc_data_alloc(struct io_ring_ctx *ctx, rsrc_put_fn *do_put,
				     u64 __user *utags, unsigned nr,
				     struct io_rsrc_data **pdata)
{
	struct io_rsrc_data *data;
	int ret = -ENOMEM;
	unsigned i;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->tags = (u64 **)io_alloc_page_table(nr * sizeof(data->tags[0][0]));
	if (!data->tags) {
		kfree(data);
		return -ENOMEM;
	}

	data->nr = nr;
	data->ctx = ctx;
	data->do_put = do_put;
	if (utags) {
		ret = -EFAULT;
		for (i = 0; i < nr; i++) {
			u64 *tag_slot = io_get_tag_slot(data, i);

			if (copy_from_user(tag_slot, &utags[i],
					   sizeof(*tag_slot)))
				goto fail;
		}
	}

	atomic_set(&data->refs, 1);
	init_completion(&data->done);
	*pdata = data;
	return 0;
fail:
	io_rsrc_data_free(data);
	return ret;
}

static bool io_alloc_file_tables(struct io_file_table *table, unsigned nr_files)
{
	table->files = kvcalloc(nr_files, sizeof(table->files[0]),
				GFP_KERNEL_ACCOUNT);
	if (unlikely(!table->files))
		return false;

	table->bitmap = bitmap_zalloc(nr_files, GFP_KERNEL_ACCOUNT);
	if (unlikely(!table->bitmap)) {
		kvfree(table->files);
		return false;
	}

	return true;
}

static void io_free_file_tables(struct io_file_table *table)
{
	kvfree(table->files);
	bitmap_free(table->bitmap);
	table->files = NULL;
	table->bitmap = NULL;
}

static inline void io_file_bitmap_set(struct io_file_table *table, int bit)
{
	WARN_ON_ONCE(test_bit(bit, table->bitmap));
	__set_bit(bit, table->bitmap);
	table->alloc_hint = bit + 1;
}

static inline void io_file_bitmap_clear(struct io_file_table *table, int bit)
{
	__clear_bit(bit, table->bitmap);
	table->alloc_hint = bit;
}

static void __io_sqe_files_unregister(struct io_ring_ctx *ctx)
{
#if !defined(IO_URING_SCM_ALL)
	int i;

	for (i = 0; i < ctx->nr_user_files; i++) {
		struct file *file = io_file_from_index(ctx, i);

		if (!file)
			continue;
		if (io_fixed_file_slot(&ctx->file_table, i)->file_ptr & FFS_SCM)
			continue;
		io_file_bitmap_clear(&ctx->file_table, i);
		fput(file);
	}
#endif

#if defined(CONFIG_UNIX)
	if (ctx->ring_sock) {
		struct sock *sock = ctx->ring_sock->sk;
		struct sk_buff *skb;

		while ((skb = skb_dequeue(&sock->sk_receive_queue)) != NULL)
			kfree_skb(skb);
	}
#endif
	io_free_file_tables(&ctx->file_table);
	io_rsrc_data_free(ctx->file_data);
	ctx->file_data = NULL;
	ctx->nr_user_files = 0;
}

static int io_sqe_files_unregister(struct io_ring_ctx *ctx)
{
	unsigned nr = ctx->nr_user_files;
	int ret;

	if (!ctx->file_data)
		return -ENXIO;

	/*
	 * Quiesce may unlock ->uring_lock, and while it's not held
	 * prevent new requests using the table.
	 */
	ctx->nr_user_files = 0;
	ret = io_rsrc_ref_quiesce(ctx->file_data, ctx);
	ctx->nr_user_files = nr;
	if (!ret)
		__io_sqe_files_unregister(ctx);
	return ret;
}

static void io_sq_thread_unpark(struct io_sq_data *sqd)
	__releases(&sqd->lock)
{
	WARN_ON_ONCE(sqd->thread == current);

	/*
	 * Do the dance but not conditional clear_bit() because it'd race with
	 * other threads incrementing park_pending and setting the bit.
	 */
	clear_bit(IO_SQ_THREAD_SHOULD_PARK, &sqd->state);
	if (atomic_dec_return(&sqd->park_pending))
		set_bit(IO_SQ_THREAD_SHOULD_PARK, &sqd->state);
	mutex_unlock(&sqd->lock);
}

static void io_sq_thread_park(struct io_sq_data *sqd)
	__acquires(&sqd->lock)
{
	WARN_ON_ONCE(sqd->thread == current);

	atomic_inc(&sqd->park_pending);
	set_bit(IO_SQ_THREAD_SHOULD_PARK, &sqd->state);
	mutex_lock(&sqd->lock);
	if (sqd->thread)
		wake_up_process(sqd->thread);
}

static void io_sq_thread_stop(struct io_sq_data *sqd)
{
	WARN_ON_ONCE(sqd->thread == current);
	WARN_ON_ONCE(test_bit(IO_SQ_THREAD_SHOULD_STOP, &sqd->state));

	set_bit(IO_SQ_THREAD_SHOULD_STOP, &sqd->state);
	mutex_lock(&sqd->lock);
	if (sqd->thread)
		wake_up_process(sqd->thread);
	mutex_unlock(&sqd->lock);
	wait_for_completion(&sqd->exited);
}

static void io_put_sq_data(struct io_sq_data *sqd)
{
	if (refcount_dec_and_test(&sqd->refs)) {
		WARN_ON_ONCE(atomic_read(&sqd->park_pending));

		io_sq_thread_stop(sqd);
		kfree(sqd);
	}
}

static void io_sq_thread_finish(struct io_ring_ctx *ctx)
{
	struct io_sq_data *sqd = ctx->sq_data;

	if (sqd) {
		io_sq_thread_park(sqd);
		list_del_init(&ctx->sqd_list);
		io_sqd_update_thread_idle(sqd);
		io_sq_thread_unpark(sqd);

		io_put_sq_data(sqd);
		ctx->sq_data = NULL;
	}
}

static struct io_sq_data *io_attach_sq_data(struct io_uring_params *p)
{
	struct io_ring_ctx *ctx_attach;
	struct io_sq_data *sqd;
	struct fd f;

	f = fdget(p->wq_fd);
	if (!f.file)
		return ERR_PTR(-ENXIO);
	if (f.file->f_op != &io_uring_fops) {
		fdput(f);
		return ERR_PTR(-EINVAL);
	}

	ctx_attach = f.file->private_data;
	sqd = ctx_attach->sq_data;
	if (!sqd) {
		fdput(f);
		return ERR_PTR(-EINVAL);
	}
	if (sqd->task_tgid != current->tgid) {
		fdput(f);
		return ERR_PTR(-EPERM);
	}

	refcount_inc(&sqd->refs);
	fdput(f);
	return sqd;
}

static struct io_sq_data *io_get_sq_data(struct io_uring_params *p,
					 bool *attached)
{
	struct io_sq_data *sqd;

	*attached = false;
	if (p->flags & IORING_SETUP_ATTACH_WQ) {
		sqd = io_attach_sq_data(p);
		if (!IS_ERR(sqd)) {
			*attached = true;
			return sqd;
		}
		/* fall through for EPERM case, setup new sqd/task */
		if (PTR_ERR(sqd) != -EPERM)
			return sqd;
	}

	sqd = kzalloc(sizeof(*sqd), GFP_KERNEL);
	if (!sqd)
		return ERR_PTR(-ENOMEM);

	atomic_set(&sqd->park_pending, 0);
	refcount_set(&sqd->refs, 1);
	INIT_LIST_HEAD(&sqd->ctx_list);
	mutex_init(&sqd->lock);
	init_waitqueue_head(&sqd->wait);
	init_completion(&sqd->exited);
	return sqd;
}

/*
 * Ensure the UNIX gc is aware of our file set, so we are certain that
 * the io_uring can be safely unregistered on process exit, even if we have
 * loops in the file referencing. We account only files that can hold other
 * files because otherwise they can't form a loop and so are not interesting
 * for GC.
 */
static int io_scm_file_account(struct io_ring_ctx *ctx, struct file *file)
{
#if defined(CONFIG_UNIX)
	struct sock *sk = ctx->ring_sock->sk;
	struct sk_buff_head *head = &sk->sk_receive_queue;
	struct scm_fp_list *fpl;
	struct sk_buff *skb;

	if (likely(!io_file_need_scm(file)))
		return 0;

	/*
	 * See if we can merge this file into an existing skb SCM_RIGHTS
	 * file set. If there's no room, fall back to allocating a new skb
	 * and filling it in.
	 */
	spin_lock_irq(&head->lock);
	skb = skb_peek(head);
	if (skb && UNIXCB(skb).fp->count < SCM_MAX_FD)
		__skb_unlink(skb, head);
	else
		skb = NULL;
	spin_unlock_irq(&head->lock);

	if (!skb) {
		fpl = kzalloc(sizeof(*fpl), GFP_KERNEL);
		if (!fpl)
			return -ENOMEM;

		skb = alloc_skb(0, GFP_KERNEL);
		if (!skb) {
			kfree(fpl);
			return -ENOMEM;
		}

		fpl->user = get_uid(current_user());
		fpl->max = SCM_MAX_FD;
		fpl->count = 0;

		UNIXCB(skb).fp = fpl;
		skb->sk = sk;
		skb->destructor = unix_destruct_scm;
		refcount_add(skb->truesize, &sk->sk_wmem_alloc);
	}

	fpl = UNIXCB(skb).fp;
	fpl->fp[fpl->count++] = get_file(file);
	unix_inflight(fpl->user, file);
	skb_queue_head(head, skb);
	fput(file);
#endif
	return 0;
}

static void io_rsrc_file_put(struct io_ring_ctx *ctx, struct io_rsrc_put *prsrc)
{
	struct file *file = prsrc->file;
#if defined(CONFIG_UNIX)
	struct sock *sock = ctx->ring_sock->sk;
	struct sk_buff_head list, *head = &sock->sk_receive_queue;
	struct sk_buff *skb;
	int i;

	if (!io_file_need_scm(file)) {
		fput(file);
		return;
	}

	__skb_queue_head_init(&list);

	/*
	 * Find the skb that holds this file in its SCM_RIGHTS. When found,
	 * remove this entry and rearrange the file array.
	 */
	skb = skb_dequeue(head);
	while (skb) {
		struct scm_fp_list *fp;

		fp = UNIXCB(skb).fp;
		for (i = 0; i < fp->count; i++) {
			int left;

			if (fp->fp[i] != file)
				continue;

			unix_notinflight(fp->user, fp->fp[i]);
			left = fp->count - 1 - i;
			if (left) {
				memmove(&fp->fp[i], &fp->fp[i + 1],
						left * sizeof(struct file *));
			}
			fp->count--;
			if (!fp->count) {
				kfree_skb(skb);
				skb = NULL;
			} else {
				__skb_queue_tail(&list, skb);
			}
			fput(file);
			file = NULL;
			break;
		}

		if (!file)
			break;

		__skb_queue_tail(&list, skb);

		skb = skb_dequeue(head);
	}

	if (skb_peek(&list)) {
		spin_lock_irq(&head->lock);
		while ((skb = __skb_dequeue(&list)) != NULL)
			__skb_queue_tail(head, skb);
		spin_unlock_irq(&head->lock);
	}
#else
	fput(file);
#endif
}

static void __io_rsrc_put_work(struct io_rsrc_node *ref_node)
{
	struct io_rsrc_data *rsrc_data = ref_node->rsrc_data;
	struct io_ring_ctx *ctx = rsrc_data->ctx;
	struct io_rsrc_put *prsrc, *tmp;

	list_for_each_entry_safe(prsrc, tmp, &ref_node->rsrc_list, list) {
		list_del(&prsrc->list);

		if (prsrc->tag) {
			if (ctx->flags & IORING_SETUP_IOPOLL)
				mutex_lock(&ctx->uring_lock);

			spin_lock(&ctx->completion_lock);
			io_fill_cqe_aux(ctx, prsrc->tag, 0, 0);
			io_commit_cqring(ctx);
			spin_unlock(&ctx->completion_lock);
			io_cqring_ev_posted(ctx);

			if (ctx->flags & IORING_SETUP_IOPOLL)
				mutex_unlock(&ctx->uring_lock);
		}

		rsrc_data->do_put(ctx, prsrc);
		kfree(prsrc);
	}

	io_rsrc_node_destroy(ref_node);
	if (atomic_dec_and_test(&rsrc_data->refs))
		complete(&rsrc_data->done);
}

static void io_rsrc_put_work(struct work_struct *work)
{
	struct io_ring_ctx *ctx;
	struct llist_node *node;

	ctx = container_of(work, struct io_ring_ctx, rsrc_put_work.work);
	node = llist_del_all(&ctx->rsrc_put_llist);

	while (node) {
		struct io_rsrc_node *ref_node;
		struct llist_node *next = node->next;

		ref_node = llist_entry(node, struct io_rsrc_node, llist);
		__io_rsrc_put_work(ref_node);
		node = next;
	}
}

static int io_sqe_files_register(struct io_ring_ctx *ctx, void __user *arg,
				 unsigned nr_args, u64 __user *tags)
{
	__s32 __user *fds = (__s32 __user *) arg;
	struct file *file;
	int fd, ret;
	unsigned i;

	if (ctx->file_data)
		return -EBUSY;
	if (!nr_args)
		return -EINVAL;
	if (nr_args > IORING_MAX_FIXED_FILES)
		return -EMFILE;
	if (nr_args > rlimit(RLIMIT_NOFILE))
		return -EMFILE;
	ret = io_rsrc_node_switch_start(ctx);
	if (ret)
		return ret;
	ret = io_rsrc_data_alloc(ctx, io_rsrc_file_put, tags, nr_args,
				 &ctx->file_data);
	if (ret)
		return ret;

	if (!io_alloc_file_tables(&ctx->file_table, nr_args)) {
		io_rsrc_data_free(ctx->file_data);
		ctx->file_data = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < nr_args; i++, ctx->nr_user_files++) {
		struct io_fixed_file *file_slot;

		if (fds && copy_from_user(&fd, &fds[i], sizeof(fd))) {
			ret = -EFAULT;
			goto fail;
		}
		/* allow sparse sets */
		if (!fds || fd == -1) {
			ret = -EINVAL;
			if (unlikely(*io_get_tag_slot(ctx->file_data, i)))
				goto fail;
			continue;
		}

		file = fget(fd);
		ret = -EBADF;
		if (unlikely(!file))
			goto fail;

		/*
		 * Don't allow io_uring instances to be registered. If UNIX
		 * isn't enabled, then this causes a reference cycle and this
		 * instance can never get freed. If UNIX is enabled we'll
		 * handle it just fine, but there's still no point in allowing
		 * a ring fd as it doesn't support regular read/write anyway.
		 */
		if (file->f_op == &io_uring_fops) {
			fput(file);
			goto fail;
		}
		ret = io_scm_file_account(ctx, file);
		if (ret) {
			fput(file);
			goto fail;
		}
		file_slot = io_fixed_file_slot(&ctx->file_table, i);
		io_fixed_file_set(file_slot, file);
		io_file_bitmap_set(&ctx->file_table, i);
	}

	io_rsrc_node_switch(ctx, NULL);
	return 0;
fail:
	__io_sqe_files_unregister(ctx);
	return ret;
}

static int io_queue_rsrc_removal(struct io_rsrc_data *data, unsigned idx,
				 struct io_rsrc_node *node, void *rsrc)
{
	u64 *tag_slot = io_get_tag_slot(data, idx);
	struct io_rsrc_put *prsrc;

	prsrc = kzalloc(sizeof(*prsrc), GFP_KERNEL);
	if (!prsrc)
		return -ENOMEM;

	prsrc->tag = *tag_slot;
	*tag_slot = 0;
	prsrc->rsrc = rsrc;
	list_add(&prsrc->list, &node->rsrc_list);
	return 0;
}

static int io_install_fixed_file(struct io_kiocb *req, struct file *file,
				 unsigned int issue_flags, u32 slot_index)
	__must_hold(&req->ctx->uring_lock)
{
	struct io_ring_ctx *ctx = req->ctx;
	bool needs_switch = false;
	struct io_fixed_file *file_slot;
	int ret;

	if (file->f_op == &io_uring_fops)
		return -EBADF;
	if (!ctx->file_data)
		return -ENXIO;
	if (slot_index >= ctx->nr_user_files)
		return -EINVAL;

	slot_index = array_index_nospec(slot_index, ctx->nr_user_files);
	file_slot = io_fixed_file_slot(&ctx->file_table, slot_index);

	if (file_slot->file_ptr) {
		struct file *old_file;

		ret = io_rsrc_node_switch_start(ctx);
		if (ret)
			goto err;

		old_file = (struct file *)(file_slot->file_ptr & FFS_MASK);
		ret = io_queue_rsrc_removal(ctx->file_data, slot_index,
					    ctx->rsrc_node, old_file);
		if (ret)
			goto err;
		file_slot->file_ptr = 0;
		io_file_bitmap_clear(&ctx->file_table, slot_index);
		needs_switch = true;
	}

	ret = io_scm_file_account(ctx, file);
	if (!ret) {
		*io_get_tag_slot(ctx->file_data, slot_index) = 0;
		io_fixed_file_set(file_slot, file);
		io_file_bitmap_set(&ctx->file_table, slot_index);
	}
err:
	if (needs_switch)
		io_rsrc_node_switch(ctx, ctx->file_data);
	if (ret)
		fput(file);
	return ret;
}

static int __io_close_fixed(struct io_kiocb *req, unsigned int issue_flags,
			    unsigned int offset)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_fixed_file *file_slot;
	struct file *file;
	int ret;

	io_ring_submit_lock(ctx, issue_flags);
	ret = -ENXIO;
	if (unlikely(!ctx->file_data))
		goto out;
	ret = -EINVAL;
	if (offset >= ctx->nr_user_files)
		goto out;
	ret = io_rsrc_node_switch_start(ctx);
	if (ret)
		goto out;

	offset = array_index_nospec(offset, ctx->nr_user_files);
	file_slot = io_fixed_file_slot(&ctx->file_table, offset);
	ret = -EBADF;
	if (!file_slot->file_ptr)
		goto out;

	file = (struct file *)(file_slot->file_ptr & FFS_MASK);
	ret = io_queue_rsrc_removal(ctx->file_data, offset, ctx->rsrc_node, file);
	if (ret)
		goto out;

	file_slot->file_ptr = 0;
	io_file_bitmap_clear(&ctx->file_table, offset);
	io_rsrc_node_switch(ctx, ctx->file_data);
	ret = 0;
out:
	io_ring_submit_unlock(ctx, issue_flags);
	return ret;
}

static inline int io_close_fixed(struct io_kiocb *req, unsigned int issue_flags)
{
	return __io_close_fixed(req, issue_flags, req->close.file_slot - 1);
}

static int __io_sqe_files_update(struct io_ring_ctx *ctx,
				 struct io_uring_rsrc_update2 *up,
				 unsigned nr_args)
{
	u64 __user *tags = u64_to_user_ptr(up->tags);
	__s32 __user *fds = u64_to_user_ptr(up->data);
	struct io_rsrc_data *data = ctx->file_data;
	struct io_fixed_file *file_slot;
	struct file *file;
	int fd, i, err = 0;
	unsigned int done;
	bool needs_switch = false;

	if (!ctx->file_data)
		return -ENXIO;
	if (up->offset + nr_args > ctx->nr_user_files)
		return -EINVAL;

	for (done = 0; done < nr_args; done++) {
		u64 tag = 0;

		if ((tags && copy_from_user(&tag, &tags[done], sizeof(tag))) ||
		    copy_from_user(&fd, &fds[done], sizeof(fd))) {
			err = -EFAULT;
			break;
		}
		if ((fd == IORING_REGISTER_FILES_SKIP || fd == -1) && tag) {
			err = -EINVAL;
			break;
		}
		if (fd == IORING_REGISTER_FILES_SKIP)
			continue;

		i = array_index_nospec(up->offset + done, ctx->nr_user_files);
		file_slot = io_fixed_file_slot(&ctx->file_table, i);

		if (file_slot->file_ptr) {
			file = (struct file *)(file_slot->file_ptr & FFS_MASK);
			err = io_queue_rsrc_removal(data, i, ctx->rsrc_node, file);
			if (err)
				break;
			file_slot->file_ptr = 0;
			io_file_bitmap_clear(&ctx->file_table, i);
			needs_switch = true;
		}
		if (fd != -1) {
			file = fget(fd);
			if (!file) {
				err = -EBADF;
				break;
			}
			/*
			 * Don't allow io_uring instances to be registered. If
			 * UNIX isn't enabled, then this causes a reference
			 * cycle and this instance can never get freed. If UNIX
			 * is enabled we'll handle it just fine, but there's
			 * still no point in allowing a ring fd as it doesn't
			 * support regular read/write anyway.
			 */
			if (file->f_op == &io_uring_fops) {
				fput(file);
				err = -EBADF;
				break;
			}
			err = io_scm_file_account(ctx, file);
			if (err) {
				fput(file);
				break;
			}
			*io_get_tag_slot(data, i) = tag;
			io_fixed_file_set(file_slot, file);
			io_file_bitmap_set(&ctx->file_table, i);
		}
	}

	if (needs_switch)
		io_rsrc_node_switch(ctx, data);
	return done ? done : err;
}

static struct io_wq *io_init_wq_offload(struct io_ring_ctx *ctx,
					struct task_struct *task)
{
	struct io_wq_hash *hash;
	struct io_wq_data data;
	unsigned int concurrency;

	mutex_lock(&ctx->uring_lock);
	hash = ctx->hash_map;
	if (!hash) {
		hash = kzalloc(sizeof(*hash), GFP_KERNEL);
		if (!hash) {
			mutex_unlock(&ctx->uring_lock);
			return ERR_PTR(-ENOMEM);
		}
		refcount_set(&hash->refs, 1);
		init_waitqueue_head(&hash->wait);
		ctx->hash_map = hash;
	}
	mutex_unlock(&ctx->uring_lock);

	data.hash = hash;
	data.task = task;
	data.free_work = io_wq_free_work;
	data.do_work = io_wq_submit_work;

	/* Do QD, or 4 * CPUS, whatever is smallest */
	concurrency = min(ctx->sq_entries, 4 * num_online_cpus());

	return io_wq_create(concurrency, &data);
}

static __cold int io_uring_alloc_task_context(struct task_struct *task,
					      struct io_ring_ctx *ctx)
{
	struct io_uring_task *tctx;
	int ret;

	tctx = kzalloc(sizeof(*tctx), GFP_KERNEL);
	if (unlikely(!tctx))
		return -ENOMEM;

	tctx->registered_rings = kcalloc(IO_RINGFD_REG_MAX,
					 sizeof(struct file *), GFP_KERNEL);
	if (unlikely(!tctx->registered_rings)) {
		kfree(tctx);
		return -ENOMEM;
	}

	ret = percpu_counter_init(&tctx->inflight, 0, GFP_KERNEL);
	if (unlikely(ret)) {
		kfree(tctx->registered_rings);
		kfree(tctx);
		return ret;
	}

	tctx->io_wq = io_init_wq_offload(ctx, task);
	if (IS_ERR(tctx->io_wq)) {
		ret = PTR_ERR(tctx->io_wq);
		percpu_counter_destroy(&tctx->inflight);
		kfree(tctx->registered_rings);
		kfree(tctx);
		return ret;
	}

	xa_init(&tctx->xa);
	init_waitqueue_head(&tctx->wait);
	atomic_set(&tctx->in_idle, 0);
	atomic_set(&tctx->inflight_tracked, 0);
	task->io_uring = tctx;
	spin_lock_init(&tctx->task_lock);
	INIT_WQ_LIST(&tctx->task_list);
	INIT_WQ_LIST(&tctx->prio_task_list);
	init_task_work(&tctx->task_work, tctx_task_work);
	return 0;
}

void __io_uring_free(struct task_struct *tsk)
{
	struct io_uring_task *tctx = tsk->io_uring;

	WARN_ON_ONCE(!xa_empty(&tctx->xa));
	WARN_ON_ONCE(tctx->io_wq);
	WARN_ON_ONCE(tctx->cached_refs);

	kfree(tctx->registered_rings);
	percpu_counter_destroy(&tctx->inflight);
	kfree(tctx);
	tsk->io_uring = NULL;
}

static __cold int io_sq_offload_create(struct io_ring_ctx *ctx,
				       struct io_uring_params *p)
{
	int ret;

	/* Retain compatibility with failing for an invalid attach attempt */
	if ((ctx->flags & (IORING_SETUP_ATTACH_WQ | IORING_SETUP_SQPOLL)) ==
				IORING_SETUP_ATTACH_WQ) {
		struct fd f;

		f = fdget(p->wq_fd);
		if (!f.file)
			return -ENXIO;
		if (f.file->f_op != &io_uring_fops) {
			fdput(f);
			return -EINVAL;
		}
		fdput(f);
	}
	if (ctx->flags & IORING_SETUP_SQPOLL) {
		struct task_struct *tsk;
		struct io_sq_data *sqd;
		bool attached;

		ret = security_uring_sqpoll();
		if (ret)
			return ret;

		sqd = io_get_sq_data(p, &attached);
		if (IS_ERR(sqd)) {
			ret = PTR_ERR(sqd);
			goto err;
		}

		ctx->sq_creds = get_current_cred();
		ctx->sq_data = sqd;
		ctx->sq_thread_idle = msecs_to_jiffies(p->sq_thread_idle);
		if (!ctx->sq_thread_idle)
			ctx->sq_thread_idle = HZ;

		io_sq_thread_park(sqd);
		list_add(&ctx->sqd_list, &sqd->ctx_list);
		io_sqd_update_thread_idle(sqd);
		/* don't attach to a dying SQPOLL thread, would be racy */
		ret = (attached && !sqd->thread) ? -ENXIO : 0;
		io_sq_thread_unpark(sqd);

		if (ret < 0)
			goto err;
		if (attached)
			return 0;

		if (p->flags & IORING_SETUP_SQ_AFF) {
			int cpu = p->sq_thread_cpu;

			ret = -EINVAL;
			if (cpu >= nr_cpu_ids || !cpu_online(cpu))
				goto err_sqpoll;
			sqd->sq_cpu = cpu;
		} else {
			sqd->sq_cpu = -1;
		}

		sqd->task_pid = current->pid;
		sqd->task_tgid = current->tgid;
		tsk = create_io_thread(io_sq_thread, sqd, NUMA_NO_NODE);
		if (IS_ERR(tsk)) {
			ret = PTR_ERR(tsk);
			goto err_sqpoll;
		}

		sqd->thread = tsk;
		ret = io_uring_alloc_task_context(tsk, ctx);
		wake_up_new_task(tsk);
		if (ret)
			goto err;
	} else if (p->flags & IORING_SETUP_SQ_AFF) {
		/* Can't have SQ_AFF without SQPOLL */
		ret = -EINVAL;
		goto err;
	}

	return 0;
err_sqpoll:
	complete(&ctx->sq_data->exited);
err:
	io_sq_thread_finish(ctx);
	return ret;
}

static inline void __io_unaccount_mem(struct user_struct *user,
				      unsigned long nr_pages)
{
	atomic_long_sub(nr_pages, &user->locked_vm);
}

static inline int __io_account_mem(struct user_struct *user,
				   unsigned long nr_pages)
{
	unsigned long page_limit, cur_pages, new_pages;

	/* Don't allow more pages than we can safely lock */
	page_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	do {
		cur_pages = atomic_long_read(&user->locked_vm);
		new_pages = cur_pages + nr_pages;
		if (new_pages > page_limit)
			return -ENOMEM;
	} while (atomic_long_cmpxchg(&user->locked_vm, cur_pages,
					new_pages) != cur_pages);

	return 0;
}

static void io_unaccount_mem(struct io_ring_ctx *ctx, unsigned long nr_pages)
{
	if (ctx->user)
		__io_unaccount_mem(ctx->user, nr_pages);

	if (ctx->mm_account)
		atomic64_sub(nr_pages, &ctx->mm_account->pinned_vm);
}

static int io_account_mem(struct io_ring_ctx *ctx, unsigned long nr_pages)
{
	int ret;

	if (ctx->user) {
		ret = __io_account_mem(ctx->user, nr_pages);
		if (ret)
			return ret;
	}

	if (ctx->mm_account)
		atomic64_add(nr_pages, &ctx->mm_account->pinned_vm);

	return 0;
}

static void io_mem_free(void *ptr)
{
	struct page *page;

	if (!ptr)
		return;

	page = virt_to_head_page(ptr);
	if (put_page_testzero(page))
		free_compound_page(page);
}

static void *io_mem_alloc(size_t size)
{
	gfp_t gfp = GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NOWARN | __GFP_COMP;

	return (void *) __get_free_pages(gfp, get_order(size));
}

static unsigned long rings_size(struct io_ring_ctx *ctx, unsigned int sq_entries,
				unsigned int cq_entries, size_t *sq_offset)
{
	struct io_rings *rings;
	size_t off, sq_array_size;

	off = struct_size(rings, cqes, cq_entries);
	if (off == SIZE_MAX)
		return SIZE_MAX;
	if (ctx->flags & IORING_SETUP_CQE32) {
		if (check_shl_overflow(off, 1, &off))
			return SIZE_MAX;
	}

#ifdef CONFIG_SMP
	off = ALIGN(off, SMP_CACHE_BYTES);
	if (off == 0)
		return SIZE_MAX;
#endif

	if (sq_offset)
		*sq_offset = off;

	sq_array_size = array_size(sizeof(u32), sq_entries);
	if (sq_array_size == SIZE_MAX)
		return SIZE_MAX;

	if (check_add_overflow(off, sq_array_size, &off))
		return SIZE_MAX;

	return off;
}

static void io_buffer_unmap(struct io_ring_ctx *ctx, struct io_mapped_ubuf **slot)
{
	struct io_mapped_ubuf *imu = *slot;
	unsigned int i;

	if (imu != ctx->dummy_ubuf) {
		for (i = 0; i < imu->nr_bvecs; i++)
			unpin_user_page(imu->bvec[i].bv_page);
		if (imu->acct_pages)
			io_unaccount_mem(ctx, imu->acct_pages);
		kvfree(imu);
	}
	*slot = NULL;
}

static void io_rsrc_buf_put(struct io_ring_ctx *ctx, struct io_rsrc_put *prsrc)
{
	io_buffer_unmap(ctx, &prsrc->buf);
	prsrc->buf = NULL;
}

static void __io_sqe_buffers_unregister(struct io_ring_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->nr_user_bufs; i++)
		io_buffer_unmap(ctx, &ctx->user_bufs[i]);
	kfree(ctx->user_bufs);
	io_rsrc_data_free(ctx->buf_data);
	ctx->user_bufs = NULL;
	ctx->buf_data = NULL;
	ctx->nr_user_bufs = 0;
}

static int io_sqe_buffers_unregister(struct io_ring_ctx *ctx)
{
	unsigned nr = ctx->nr_user_bufs;
	int ret;

	if (!ctx->buf_data)
		return -ENXIO;

	/*
	 * Quiesce may unlock ->uring_lock, and while it's not held
	 * prevent new requests using the table.
	 */
	ctx->nr_user_bufs = 0;
	ret = io_rsrc_ref_quiesce(ctx->buf_data, ctx);
	ctx->nr_user_bufs = nr;
	if (!ret)
		__io_sqe_buffers_unregister(ctx);
	return ret;
}

static int io_copy_iov(struct io_ring_ctx *ctx, struct iovec *dst,
		       void __user *arg, unsigned index)
{
	struct iovec __user *src;

#ifdef CONFIG_COMPAT
	if (ctx->compat) {
		struct compat_iovec __user *ciovs;
		struct compat_iovec ciov;

		ciovs = (struct compat_iovec __user *) arg;
		if (copy_from_user(&ciov, &ciovs[index], sizeof(ciov)))
			return -EFAULT;

		dst->iov_base = u64_to_user_ptr((u64)ciov.iov_base);
		dst->iov_len = ciov.iov_len;
		return 0;
	}
#endif
	src = (struct iovec __user *) arg;
	if (copy_from_user(dst, &src[index], sizeof(*dst)))
		return -EFAULT;
	return 0;
}

/*
 * Not super efficient, but this is just a registration time. And we do cache
 * the last compound head, so generally we'll only do a full search if we don't
 * match that one.
 *
 * We check if the given compound head page has already been accounted, to
 * avoid double accounting it. This allows us to account the full size of the
 * page, not just the constituent pages of a huge page.
 */
static bool headpage_already_acct(struct io_ring_ctx *ctx, struct page **pages,
				  int nr_pages, struct page *hpage)
{
	int i, j;

	/* check current page array */
	for (i = 0; i < nr_pages; i++) {
		if (!PageCompound(pages[i]))
			continue;
		if (compound_head(pages[i]) == hpage)
			return true;
	}

	/* check previously registered pages */
	for (i = 0; i < ctx->nr_user_bufs; i++) {
		struct io_mapped_ubuf *imu = ctx->user_bufs[i];

		for (j = 0; j < imu->nr_bvecs; j++) {
			if (!PageCompound(imu->bvec[j].bv_page))
				continue;
			if (compound_head(imu->bvec[j].bv_page) == hpage)
				return true;
		}
	}

	return false;
}

static int io_buffer_account_pin(struct io_ring_ctx *ctx, struct page **pages,
				 int nr_pages, struct io_mapped_ubuf *imu,
				 struct page **last_hpage)
{
	int i, ret;

	imu->acct_pages = 0;
	for (i = 0; i < nr_pages; i++) {
		if (!PageCompound(pages[i])) {
			imu->acct_pages++;
		} else {
			struct page *hpage;

			hpage = compound_head(pages[i]);
			if (hpage == *last_hpage)
				continue;
			*last_hpage = hpage;
			if (headpage_already_acct(ctx, pages, i, hpage))
				continue;
			imu->acct_pages += page_size(hpage) >> PAGE_SHIFT;
		}
	}

	if (!imu->acct_pages)
		return 0;

	ret = io_account_mem(ctx, imu->acct_pages);
	if (ret)
		imu->acct_pages = 0;
	return ret;
}

static struct page **io_pin_pages(unsigned long ubuf, unsigned long len,
				  int *npages)
{
	unsigned long start, end, nr_pages;
	struct vm_area_struct **vmas = NULL;
	struct page **pages = NULL;
	int i, pret, ret = -ENOMEM;

	end = (ubuf + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	start = ubuf >> PAGE_SHIFT;
	nr_pages = end - start;

	pages = kvmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		goto done;

	vmas = kvmalloc_array(nr_pages, sizeof(struct vm_area_struct *),
			      GFP_KERNEL);
	if (!vmas)
		goto done;

	ret = 0;
	mmap_read_lock(current->mm);
	pret = pin_user_pages(ubuf, nr_pages, FOLL_WRITE | FOLL_LONGTERM,
			      pages, vmas);
	if (pret == nr_pages) {
		/* don't support file backed memory */
		for (i = 0; i < nr_pages; i++) {
			struct vm_area_struct *vma = vmas[i];

			if (vma_is_shmem(vma))
				continue;
			if (vma->vm_file &&
			    !is_file_hugepages(vma->vm_file)) {
				ret = -EOPNOTSUPP;
				break;
			}
		}
		*npages = nr_pages;
	} else {
		ret = pret < 0 ? pret : -EFAULT;
	}
	mmap_read_unlock(current->mm);
	if (ret) {
		/*
		 * if we did partial map, or found file backed vmas,
		 * release any pages we did get
		 */
		if (pret > 0)
			unpin_user_pages(pages, pret);
		goto done;
	}
	ret = 0;
done:
	kvfree(vmas);
	if (ret < 0) {
		kvfree(pages);
		pages = ERR_PTR(ret);
	}
	return pages;
}

static int io_sqe_buffer_register(struct io_ring_ctx *ctx, struct iovec *iov,
				  struct io_mapped_ubuf **pimu,
				  struct page **last_hpage)
{
	struct io_mapped_ubuf *imu = NULL;
	struct page **pages = NULL;
	unsigned long off;
	size_t size;
	int ret, nr_pages, i;

	if (!iov->iov_base) {
		*pimu = ctx->dummy_ubuf;
		return 0;
	}

	*pimu = NULL;
	ret = -ENOMEM;

	pages = io_pin_pages((unsigned long) iov->iov_base, iov->iov_len,
				&nr_pages);
	if (IS_ERR(pages)) {
		ret = PTR_ERR(pages);
		pages = NULL;
		goto done;
	}

	imu = kvmalloc(struct_size(imu, bvec, nr_pages), GFP_KERNEL);
	if (!imu)
		goto done;

	ret = io_buffer_account_pin(ctx, pages, nr_pages, imu, last_hpage);
	if (ret) {
		unpin_user_pages(pages, nr_pages);
		goto done;
	}

	off = (unsigned long) iov->iov_base & ~PAGE_MASK;
	size = iov->iov_len;
	for (i = 0; i < nr_pages; i++) {
		size_t vec_len;

		vec_len = min_t(size_t, size, PAGE_SIZE - off);
		imu->bvec[i].bv_page = pages[i];
		imu->bvec[i].bv_len = vec_len;
		imu->bvec[i].bv_offset = off;
		off = 0;
		size -= vec_len;
	}
	/* store original address for later verification */
	imu->ubuf = (unsigned long) iov->iov_base;
	imu->ubuf_end = imu->ubuf + iov->iov_len;
	imu->nr_bvecs = nr_pages;
	*pimu = imu;
	ret = 0;
done:
	if (ret)
		kvfree(imu);
	kvfree(pages);
	return ret;
}

static int io_buffers_map_alloc(struct io_ring_ctx *ctx, unsigned int nr_args)
{
	ctx->user_bufs = kcalloc(nr_args, sizeof(*ctx->user_bufs), GFP_KERNEL);
	return ctx->user_bufs ? 0 : -ENOMEM;
}

static int io_buffer_validate(struct iovec *iov)
{
	unsigned long tmp, acct_len = iov->iov_len + (PAGE_SIZE - 1);

	/*
	 * Don't impose further limits on the size and buffer
	 * constraints here, we'll -EINVAL later when IO is
	 * submitted if they are wrong.
	 */
	if (!iov->iov_base)
		return iov->iov_len ? -EFAULT : 0;
	if (!iov->iov_len)
		return -EFAULT;

	/* arbitrary limit, but we need something */
	if (iov->iov_len > SZ_1G)
		return -EFAULT;

	if (check_add_overflow((unsigned long)iov->iov_base, acct_len, &tmp))
		return -EOVERFLOW;

	return 0;
}

static int io_sqe_buffers_register(struct io_ring_ctx *ctx, void __user *arg,
				   unsigned int nr_args, u64 __user *tags)
{
	struct page *last_hpage = NULL;
	struct io_rsrc_data *data;
	int i, ret;
	struct iovec iov;

	if (ctx->user_bufs)
		return -EBUSY;
	if (!nr_args || nr_args > IORING_MAX_REG_BUFFERS)
		return -EINVAL;
	ret = io_rsrc_node_switch_start(ctx);
	if (ret)
		return ret;
	ret = io_rsrc_data_alloc(ctx, io_rsrc_buf_put, tags, nr_args, &data);
	if (ret)
		return ret;
	ret = io_buffers_map_alloc(ctx, nr_args);
	if (ret) {
		io_rsrc_data_free(data);
		return ret;
	}

	for (i = 0; i < nr_args; i++, ctx->nr_user_bufs++) {
		if (arg) {
			ret = io_copy_iov(ctx, &iov, arg, i);
			if (ret)
				break;
			ret = io_buffer_validate(&iov);
			if (ret)
				break;
		} else {
			memset(&iov, 0, sizeof(iov));
		}

		if (!iov.iov_base && *io_get_tag_slot(data, i)) {
			ret = -EINVAL;
			break;
		}

		ret = io_sqe_buffer_register(ctx, &iov, &ctx->user_bufs[i],
					     &last_hpage);
		if (ret)
			break;
	}

	WARN_ON_ONCE(ctx->buf_data);

	ctx->buf_data = data;
	if (ret)
		__io_sqe_buffers_unregister(ctx);
	else
		io_rsrc_node_switch(ctx, NULL);
	return ret;
}

static int __io_sqe_buffers_update(struct io_ring_ctx *ctx,
				   struct io_uring_rsrc_update2 *up,
				   unsigned int nr_args)
{
	u64 __user *tags = u64_to_user_ptr(up->tags);
	struct iovec iov, __user *iovs = u64_to_user_ptr(up->data);
	struct page *last_hpage = NULL;
	bool needs_switch = false;
	__u32 done;
	int i, err;

	if (!ctx->buf_data)
		return -ENXIO;
	if (up->offset + nr_args > ctx->nr_user_bufs)
		return -EINVAL;

	for (done = 0; done < nr_args; done++) {
		struct io_mapped_ubuf *imu;
		int offset = up->offset + done;
		u64 tag = 0;

		err = io_copy_iov(ctx, &iov, iovs, done);
		if (err)
			break;
		if (tags && copy_from_user(&tag, &tags[done], sizeof(tag))) {
			err = -EFAULT;
			break;
		}
		err = io_buffer_validate(&iov);
		if (err)
			break;
		if (!iov.iov_base && tag) {
			err = -EINVAL;
			break;
		}
		err = io_sqe_buffer_register(ctx, &iov, &imu, &last_hpage);
		if (err)
			break;

		i = array_index_nospec(offset, ctx->nr_user_bufs);
		if (ctx->user_bufs[i] != ctx->dummy_ubuf) {
			err = io_queue_rsrc_removal(ctx->buf_data, i,
						    ctx->rsrc_node, ctx->user_bufs[i]);
			if (unlikely(err)) {
				io_buffer_unmap(ctx, &imu);
				break;
			}
			ctx->user_bufs[i] = NULL;
			needs_switch = true;
		}

		ctx->user_bufs[i] = imu;
		*io_get_tag_slot(ctx->buf_data, offset) = tag;
	}

	if (needs_switch)
		io_rsrc_node_switch(ctx, ctx->buf_data);
	return done ? done : err;
}

static int io_eventfd_register(struct io_ring_ctx *ctx, void __user *arg,
			       unsigned int eventfd_async)
{
	struct io_ev_fd *ev_fd;
	__s32 __user *fds = arg;
	int fd;

	ev_fd = rcu_dereference_protected(ctx->io_ev_fd,
					lockdep_is_held(&ctx->uring_lock));
	if (ev_fd)
		return -EBUSY;

	if (copy_from_user(&fd, fds, sizeof(*fds)))
		return -EFAULT;

	ev_fd = kmalloc(sizeof(*ev_fd), GFP_KERNEL);
	if (!ev_fd)
		return -ENOMEM;

	ev_fd->cq_ev_fd = eventfd_ctx_fdget(fd);
	if (IS_ERR(ev_fd->cq_ev_fd)) {
		int ret = PTR_ERR(ev_fd->cq_ev_fd);
		kfree(ev_fd);
		return ret;
	}
	ev_fd->eventfd_async = eventfd_async;
	ctx->has_evfd = true;
	rcu_assign_pointer(ctx->io_ev_fd, ev_fd);
	return 0;
}

static void io_eventfd_put(struct rcu_head *rcu)
{
	struct io_ev_fd *ev_fd = container_of(rcu, struct io_ev_fd, rcu);

	eventfd_ctx_put(ev_fd->cq_ev_fd);
	kfree(ev_fd);
}

static int io_eventfd_unregister(struct io_ring_ctx *ctx)
{
	struct io_ev_fd *ev_fd;

	ev_fd = rcu_dereference_protected(ctx->io_ev_fd,
					lockdep_is_held(&ctx->uring_lock));
	if (ev_fd) {
		ctx->has_evfd = false;
		rcu_assign_pointer(ctx->io_ev_fd, NULL);
		call_rcu(&ev_fd->rcu, io_eventfd_put);
		return 0;
	}

	return -ENXIO;
}

static void io_destroy_buffers(struct io_ring_ctx *ctx)
{
	struct io_buffer_list *bl;
	unsigned long index;
	int i;

	for (i = 0; i < BGID_ARRAY; i++) {
		if (!ctx->io_bl)
			break;
		__io_remove_buffers(ctx, &ctx->io_bl[i], -1U);
	}

	xa_for_each(&ctx->io_bl_xa, index, bl) {
		xa_erase(&ctx->io_bl_xa, bl->bgid);
		__io_remove_buffers(ctx, bl, -1U);
		kfree(bl);
	}

	while (!list_empty(&ctx->io_buffers_pages)) {
		struct page *page;

		page = list_first_entry(&ctx->io_buffers_pages, struct page, lru);
		list_del_init(&page->lru);
		__free_page(page);
	}
}

static void io_req_caches_free(struct io_ring_ctx *ctx)
{
	struct io_submit_state *state = &ctx->submit_state;
	int nr = 0;

	mutex_lock(&ctx->uring_lock);
	io_flush_cached_locked_reqs(ctx, state);

	while (!io_req_cache_empty(ctx)) {
		struct io_wq_work_node *node;
		struct io_kiocb *req;

		node = wq_stack_extract(&state->free_list);
		req = container_of(node, struct io_kiocb, comp_list);
		kmem_cache_free(req_cachep, req);
		nr++;
	}
	if (nr)
		percpu_ref_put_many(&ctx->refs, nr);
	mutex_unlock(&ctx->uring_lock);
}

static void io_wait_rsrc_data(struct io_rsrc_data *data)
{
	if (data && !atomic_dec_and_test(&data->refs))
		wait_for_completion(&data->done);
}

static void io_flush_apoll_cache(struct io_ring_ctx *ctx)
{
	struct async_poll *apoll;

	while (!list_empty(&ctx->apoll_cache)) {
		apoll = list_first_entry(&ctx->apoll_cache, struct async_poll,
						poll.wait.entry);
		list_del(&apoll->poll.wait.entry);
		kfree(apoll);
	}
}

static __cold void io_ring_ctx_free(struct io_ring_ctx *ctx)
{
	io_sq_thread_finish(ctx);

	if (ctx->mm_account) {
		mmdrop(ctx->mm_account);
		ctx->mm_account = NULL;
	}

	io_rsrc_refs_drop(ctx);
	/* __io_rsrc_put_work() may need uring_lock to progress, wait w/o it */
	io_wait_rsrc_data(ctx->buf_data);
	io_wait_rsrc_data(ctx->file_data);

	mutex_lock(&ctx->uring_lock);
	if (ctx->buf_data)
		__io_sqe_buffers_unregister(ctx);
	if (ctx->file_data)
		__io_sqe_files_unregister(ctx);
	if (ctx->rings)
		__io_cqring_overflow_flush(ctx, true);
	io_eventfd_unregister(ctx);
	io_flush_apoll_cache(ctx);
	mutex_unlock(&ctx->uring_lock);
	io_destroy_buffers(ctx);
	if (ctx->sq_creds)
		put_cred(ctx->sq_creds);

	/* there are no registered resources left, nobody uses it */
	if (ctx->rsrc_node)
		io_rsrc_node_destroy(ctx->rsrc_node);
	if (ctx->rsrc_backup_node)
		io_rsrc_node_destroy(ctx->rsrc_backup_node);
	flush_delayed_work(&ctx->rsrc_put_work);
	flush_delayed_work(&ctx->fallback_work);

	WARN_ON_ONCE(!list_empty(&ctx->rsrc_ref_list));
	WARN_ON_ONCE(!llist_empty(&ctx->rsrc_put_llist));

#if defined(CONFIG_UNIX)
	if (ctx->ring_sock) {
		ctx->ring_sock->file = NULL; /* so that iput() is called */
		sock_release(ctx->ring_sock);
	}
#endif
	WARN_ON_ONCE(!list_empty(&ctx->ltimeout_list));

	io_mem_free(ctx->rings);
	io_mem_free(ctx->sq_sqes);

	percpu_ref_exit(&ctx->refs);
	free_uid(ctx->user);
	io_req_caches_free(ctx);
	if (ctx->hash_map)
		io_wq_put_hash(ctx->hash_map);
	kfree(ctx->cancel_hash);
	kfree(ctx->dummy_ubuf);
	kfree(ctx->io_bl);
	xa_destroy(&ctx->io_bl_xa);
	kfree(ctx);
}

static __poll_t io_uring_poll(struct file *file, poll_table *wait)
{
	struct io_ring_ctx *ctx = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &ctx->cq_wait, wait);
	/*
	 * synchronizes with barrier from wq_has_sleeper call in
	 * io_commit_cqring
	 */
	smp_rmb();
	if (!io_sqring_full(ctx))
		mask |= EPOLLOUT | EPOLLWRNORM;

	/*
	 * Don't flush cqring overflow list here, just do a simple check.
	 * Otherwise there could possible be ABBA deadlock:
	 *      CPU0                    CPU1
	 *      ----                    ----
	 * lock(&ctx->uring_lock);
	 *                              lock(&ep->mtx);
	 *                              lock(&ctx->uring_lock);
	 * lock(&ep->mtx);
	 *
	 * Users may get EPOLLIN meanwhile seeing nothing in cqring, this
	 * pushs them to do the flush.
	 */
	if (io_cqring_events(ctx) ||
	    test_bit(IO_CHECK_CQ_OVERFLOW_BIT, &ctx->check_cq))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static int io_unregister_personality(struct io_ring_ctx *ctx, unsigned id)
{
	const struct cred *creds;

	creds = xa_erase(&ctx->personalities, id);
	if (creds) {
		put_cred(creds);
		return 0;
	}

	return -EINVAL;
}

struct io_tctx_exit {
	struct callback_head		task_work;
	struct completion		completion;
	struct io_ring_ctx		*ctx;
};

static __cold void io_tctx_exit_cb(struct callback_head *cb)
{
	struct io_uring_task *tctx = current->io_uring;
	struct io_tctx_exit *work;

	work = container_of(cb, struct io_tctx_exit, task_work);
	/*
	 * When @in_idle, we're in cancellation and it's racy to remove the
	 * node. It'll be removed by the end of cancellation, just ignore it.
	 */
	if (!atomic_read(&tctx->in_idle))
		io_uring_del_tctx_node((unsigned long)work->ctx);
	complete(&work->completion);
}

static __cold bool io_cancel_ctx_cb(struct io_wq_work *work, void *data)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);

	return req->ctx == data;
}

static __cold void io_ring_exit_work(struct work_struct *work)
{
	struct io_ring_ctx *ctx = container_of(work, struct io_ring_ctx, exit_work);
	unsigned long timeout = jiffies + HZ * 60 * 5;
	unsigned long interval = HZ / 20;
	struct io_tctx_exit exit;
	struct io_tctx_node *node;
	int ret;

	/*
	 * If we're doing polled IO and end up having requests being
	 * submitted async (out-of-line), then completions can come in while
	 * we're waiting for refs to drop. We need to reap these manually,
	 * as nobody else will be looking for them.
	 */
	do {
		io_uring_try_cancel_requests(ctx, NULL, true);
		if (ctx->sq_data) {
			struct io_sq_data *sqd = ctx->sq_data;
			struct task_struct *tsk;

			io_sq_thread_park(sqd);
			tsk = sqd->thread;
			if (tsk && tsk->io_uring && tsk->io_uring->io_wq)
				io_wq_cancel_cb(tsk->io_uring->io_wq,
						io_cancel_ctx_cb, ctx, true);
			io_sq_thread_unpark(sqd);
		}

		io_req_caches_free(ctx);

		if (WARN_ON_ONCE(time_after(jiffies, timeout))) {
			/* there is little hope left, don't run it too often */
			interval = HZ * 60;
		}
	} while (!wait_for_completion_timeout(&ctx->ref_comp, interval));

	init_completion(&exit.completion);
	init_task_work(&exit.task_work, io_tctx_exit_cb);
	exit.ctx = ctx;
	/*
	 * Some may use context even when all refs and requests have been put,
	 * and they are free to do so while still holding uring_lock or
	 * completion_lock, see io_req_task_submit(). Apart from other work,
	 * this lock/unlock section also waits them to finish.
	 */
	mutex_lock(&ctx->uring_lock);
	while (!list_empty(&ctx->tctx_list)) {
		WARN_ON_ONCE(time_after(jiffies, timeout));

		node = list_first_entry(&ctx->tctx_list, struct io_tctx_node,
					ctx_node);
		/* don't spin on a single task if cancellation failed */
		list_rotate_left(&ctx->tctx_list);
		ret = task_work_add(node->task, &exit.task_work, TWA_SIGNAL);
		if (WARN_ON_ONCE(ret))
			continue;

		mutex_unlock(&ctx->uring_lock);
		wait_for_completion(&exit.completion);
		mutex_lock(&ctx->uring_lock);
	}
	mutex_unlock(&ctx->uring_lock);
	spin_lock(&ctx->completion_lock);
	spin_unlock(&ctx->completion_lock);

	io_ring_ctx_free(ctx);
}

/* Returns true if we found and killed one or more timeouts */
static __cold bool io_kill_timeouts(struct io_ring_ctx *ctx,
				    struct task_struct *tsk, bool cancel_all)
{
	struct io_kiocb *req, *tmp;
	int canceled = 0;

	spin_lock(&ctx->completion_lock);
	spin_lock_irq(&ctx->timeout_lock);
	list_for_each_entry_safe(req, tmp, &ctx->timeout_list, timeout.list) {
		if (io_match_task(req, tsk, cancel_all)) {
			io_kill_timeout(req, -ECANCELED);
			canceled++;
		}
	}
	spin_unlock_irq(&ctx->timeout_lock);
	io_commit_cqring(ctx);
	spin_unlock(&ctx->completion_lock);
	if (canceled != 0)
		io_cqring_ev_posted(ctx);
	return canceled != 0;
}

static __cold void io_ring_ctx_wait_and_kill(struct io_ring_ctx *ctx)
{
	unsigned long index;
	struct creds *creds;

	mutex_lock(&ctx->uring_lock);
	percpu_ref_kill(&ctx->refs);
	if (ctx->rings)
		__io_cqring_overflow_flush(ctx, true);
	xa_for_each(&ctx->personalities, index, creds)
		io_unregister_personality(ctx, index);
	mutex_unlock(&ctx->uring_lock);

	/* failed during ring init, it couldn't have issued any requests */
	if (ctx->rings) {
		io_kill_timeouts(ctx, NULL, true);
		io_poll_remove_all(ctx, NULL, true);
		/* if we failed setting up the ctx, we might not have any rings */
		io_iopoll_try_reap_events(ctx);
	}

	INIT_WORK(&ctx->exit_work, io_ring_exit_work);
	/*
	 * Use system_unbound_wq to avoid spawning tons of event kworkers
	 * if we're exiting a ton of rings at the same time. It just adds
	 * noise and overhead, there's no discernable change in runtime
	 * over using system_wq.
	 */
	queue_work(system_unbound_wq, &ctx->exit_work);
}

static int io_uring_release(struct inode *inode, struct file *file)
{
	struct io_ring_ctx *ctx = file->private_data;

	file->private_data = NULL;
	io_ring_ctx_wait_and_kill(ctx);
	return 0;
}

struct io_task_cancel {
	struct task_struct *task;
	bool all;
};

static bool io_cancel_task_cb(struct io_wq_work *work, void *data)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);
	struct io_task_cancel *cancel = data;

	return io_match_task_safe(req, cancel->task, cancel->all);
}

static __cold bool io_cancel_defer_files(struct io_ring_ctx *ctx,
					 struct task_struct *task,
					 bool cancel_all)
{
	struct io_defer_entry *de;
	LIST_HEAD(list);

	spin_lock(&ctx->completion_lock);
	list_for_each_entry_reverse(de, &ctx->defer_list, list) {
		if (io_match_task_safe(de->req, task, cancel_all)) {
			list_cut_position(&list, &ctx->defer_list, &de->list);
			break;
		}
	}
	spin_unlock(&ctx->completion_lock);
	if (list_empty(&list))
		return false;

	while (!list_empty(&list)) {
		de = list_first_entry(&list, struct io_defer_entry, list);
		list_del_init(&de->list);
		io_req_complete_failed(de->req, -ECANCELED);
		kfree(de);
	}
	return true;
}

static __cold bool io_uring_try_cancel_iowq(struct io_ring_ctx *ctx)
{
	struct io_tctx_node *node;
	enum io_wq_cancel cret;
	bool ret = false;

	mutex_lock(&ctx->uring_lock);
	list_for_each_entry(node, &ctx->tctx_list, ctx_node) {
		struct io_uring_task *tctx = node->task->io_uring;

		/*
		 * io_wq will stay alive while we hold uring_lock, because it's
		 * killed after ctx nodes, which requires to take the lock.
		 */
		if (!tctx || !tctx->io_wq)
			continue;
		cret = io_wq_cancel_cb(tctx->io_wq, io_cancel_ctx_cb, ctx, true);
		ret |= (cret != IO_WQ_CANCEL_NOTFOUND);
	}
	mutex_unlock(&ctx->uring_lock);

	return ret;
}

static __cold void io_uring_try_cancel_requests(struct io_ring_ctx *ctx,
						struct task_struct *task,
						bool cancel_all)
{
	struct io_task_cancel cancel = { .task = task, .all = cancel_all, };
	struct io_uring_task *tctx = task ? task->io_uring : NULL;

	/* failed during ring init, it couldn't have issued any requests */
	if (!ctx->rings)
		return;

	while (1) {
		enum io_wq_cancel cret;
		bool ret = false;

		if (!task) {
			ret |= io_uring_try_cancel_iowq(ctx);
		} else if (tctx && tctx->io_wq) {
			/*
			 * Cancels requests of all rings, not only @ctx, but
			 * it's fine as the task is in exit/exec.
			 */
			cret = io_wq_cancel_cb(tctx->io_wq, io_cancel_task_cb,
					       &cancel, true);
			ret |= (cret != IO_WQ_CANCEL_NOTFOUND);
		}

		/* SQPOLL thread does its own polling */
		if ((!(ctx->flags & IORING_SETUP_SQPOLL) && cancel_all) ||
		    (ctx->sq_data && ctx->sq_data->thread == current)) {
			while (!wq_list_empty(&ctx->iopoll_list)) {
				io_iopoll_try_reap_events(ctx);
				ret = true;
			}
		}

		ret |= io_cancel_defer_files(ctx, task, cancel_all);
		ret |= io_poll_remove_all(ctx, task, cancel_all);
		ret |= io_kill_timeouts(ctx, task, cancel_all);
		if (task)
			ret |= io_run_task_work();
		if (!ret)
			break;
		cond_resched();
	}
}

static int __io_uring_add_tctx_node(struct io_ring_ctx *ctx)
{
	struct io_uring_task *tctx = current->io_uring;
	struct io_tctx_node *node;
	int ret;

	if (unlikely(!tctx)) {
		ret = io_uring_alloc_task_context(current, ctx);
		if (unlikely(ret))
			return ret;

		tctx = current->io_uring;
		if (ctx->iowq_limits_set) {
			unsigned int limits[2] = { ctx->iowq_limits[0],
						   ctx->iowq_limits[1], };

			ret = io_wq_max_workers(tctx->io_wq, limits);
			if (ret)
				return ret;
		}
	}
	if (!xa_load(&tctx->xa, (unsigned long)ctx)) {
		node = kmalloc(sizeof(*node), GFP_KERNEL);
		if (!node)
			return -ENOMEM;
		node->ctx = ctx;
		node->task = current;

		ret = xa_err(xa_store(&tctx->xa, (unsigned long)ctx,
					node, GFP_KERNEL));
		if (ret) {
			kfree(node);
			return ret;
		}

		mutex_lock(&ctx->uring_lock);
		list_add(&node->ctx_node, &ctx->tctx_list);
		mutex_unlock(&ctx->uring_lock);
	}
	tctx->last = ctx;
	return 0;
}

/*
 * Note that this task has used io_uring. We use it for cancelation purposes.
 */
static inline int io_uring_add_tctx_node(struct io_ring_ctx *ctx)
{
	struct io_uring_task *tctx = current->io_uring;

	if (likely(tctx && tctx->last == ctx))
		return 0;
	return __io_uring_add_tctx_node(ctx);
}

/*
 * Remove this io_uring_file -> task mapping.
 */
static __cold void io_uring_del_tctx_node(unsigned long index)
{
	struct io_uring_task *tctx = current->io_uring;
	struct io_tctx_node *node;

	if (!tctx)
		return;
	node = xa_erase(&tctx->xa, index);
	if (!node)
		return;

	WARN_ON_ONCE(current != node->task);
	WARN_ON_ONCE(list_empty(&node->ctx_node));

	mutex_lock(&node->ctx->uring_lock);
	list_del(&node->ctx_node);
	mutex_unlock(&node->ctx->uring_lock);

	if (tctx->last == node->ctx)
		tctx->last = NULL;
	kfree(node);
}

static __cold void io_uring_clean_tctx(struct io_uring_task *tctx)
{
	struct io_wq *wq = tctx->io_wq;
	struct io_tctx_node *node;
	unsigned long index;

	xa_for_each(&tctx->xa, index, node) {
		io_uring_del_tctx_node(index);
		cond_resched();
	}
	if (wq) {
		/*
		 * Must be after io_uring_del_tctx_node() (removes nodes under
		 * uring_lock) to avoid race with io_uring_try_cancel_iowq().
		 */
		io_wq_put_and_exit(wq);
		tctx->io_wq = NULL;
	}
}

static s64 tctx_inflight(struct io_uring_task *tctx, bool tracked)
{
	if (tracked)
		return atomic_read(&tctx->inflight_tracked);
	return percpu_counter_sum(&tctx->inflight);
}

/*
 * Find any io_uring ctx that this task has registered or done IO on, and cancel
 * requests. @sqd should be not-null IFF it's an SQPOLL thread cancellation.
 */
static __cold void io_uring_cancel_generic(bool cancel_all,
					   struct io_sq_data *sqd)
{
	struct io_uring_task *tctx = current->io_uring;
	struct io_ring_ctx *ctx;
	s64 inflight;
	DEFINE_WAIT(wait);

	WARN_ON_ONCE(sqd && sqd->thread != current);

	if (!current->io_uring)
		return;
	if (tctx->io_wq)
		io_wq_exit_start(tctx->io_wq);

	atomic_inc(&tctx->in_idle);
	do {
		io_uring_drop_tctx_refs(current);
		/* read completions before cancelations */
		inflight = tctx_inflight(tctx, !cancel_all);
		if (!inflight)
			break;

		if (!sqd) {
			struct io_tctx_node *node;
			unsigned long index;

			xa_for_each(&tctx->xa, index, node) {
				/* sqpoll task will cancel all its requests */
				if (node->ctx->sq_data)
					continue;
				io_uring_try_cancel_requests(node->ctx, current,
							     cancel_all);
			}
		} else {
			list_for_each_entry(ctx, &sqd->ctx_list, sqd_list)
				io_uring_try_cancel_requests(ctx, current,
							     cancel_all);
		}

		prepare_to_wait(&tctx->wait, &wait, TASK_INTERRUPTIBLE);
		io_run_task_work();
		io_uring_drop_tctx_refs(current);

		/*
		 * If we've seen completions, retry without waiting. This
		 * avoids a race where a completion comes in before we did
		 * prepare_to_wait().
		 */
		if (inflight == tctx_inflight(tctx, !cancel_all))
			schedule();
		finish_wait(&tctx->wait, &wait);
	} while (1);

	io_uring_clean_tctx(tctx);
	if (cancel_all) {
		/*
		 * We shouldn't run task_works after cancel, so just leave
		 * ->in_idle set for normal exit.
		 */
		atomic_dec(&tctx->in_idle);
		/* for exec all current's requests should be gone, kill tctx */
		__io_uring_free(current);
	}
}

void __io_uring_cancel(bool cancel_all)
{
	io_uring_cancel_generic(cancel_all, NULL);
}

void io_uring_unreg_ringfd(void)
{
	struct io_uring_task *tctx = current->io_uring;
	int i;

	for (i = 0; i < IO_RINGFD_REG_MAX; i++) {
		if (tctx->registered_rings[i]) {
			fput(tctx->registered_rings[i]);
			tctx->registered_rings[i] = NULL;
		}
	}
}

static int io_ring_add_registered_fd(struct io_uring_task *tctx, int fd,
				     int start, int end)
{
	struct file *file;
	int offset;

	for (offset = start; offset < end; offset++) {
		offset = array_index_nospec(offset, IO_RINGFD_REG_MAX);
		if (tctx->registered_rings[offset])
			continue;

		file = fget(fd);
		if (!file) {
			return -EBADF;
		} else if (file->f_op != &io_uring_fops) {
			fput(file);
			return -EOPNOTSUPP;
		}
		tctx->registered_rings[offset] = file;
		return offset;
	}

	return -EBUSY;
}

/*
 * Register a ring fd to avoid fdget/fdput for each io_uring_enter()
 * invocation. User passes in an array of struct io_uring_rsrc_update
 * with ->data set to the ring_fd, and ->offset given for the desired
 * index. If no index is desired, application may set ->offset == -1U
 * and we'll find an available index. Returns number of entries
 * successfully processed, or < 0 on error if none were processed.
 */
static int io_ringfd_register(struct io_ring_ctx *ctx, void __user *__arg,
			      unsigned nr_args)
{
	struct io_uring_rsrc_update __user *arg = __arg;
	struct io_uring_rsrc_update reg;
	struct io_uring_task *tctx;
	int ret, i;

	if (!nr_args || nr_args > IO_RINGFD_REG_MAX)
		return -EINVAL;

	mutex_unlock(&ctx->uring_lock);
	ret = io_uring_add_tctx_node(ctx);
	mutex_lock(&ctx->uring_lock);
	if (ret)
		return ret;

	tctx = current->io_uring;
	for (i = 0; i < nr_args; i++) {
		int start, end;

		if (copy_from_user(&reg, &arg[i], sizeof(reg))) {
			ret = -EFAULT;
			break;
		}

		if (reg.resv) {
			ret = -EINVAL;
			break;
		}

		if (reg.offset == -1U) {
			start = 0;
			end = IO_RINGFD_REG_MAX;
		} else {
			if (reg.offset >= IO_RINGFD_REG_MAX) {
				ret = -EINVAL;
				break;
			}
			start = reg.offset;
			end = start + 1;
		}

		ret = io_ring_add_registered_fd(tctx, reg.data, start, end);
		if (ret < 0)
			break;

		reg.offset = ret;
		if (copy_to_user(&arg[i], &reg, sizeof(reg))) {
			fput(tctx->registered_rings[reg.offset]);
			tctx->registered_rings[reg.offset] = NULL;
			ret = -EFAULT;
			break;
		}
	}

	return i ? i : ret;
}

static int io_ringfd_unregister(struct io_ring_ctx *ctx, void __user *__arg,
				unsigned nr_args)
{
	struct io_uring_rsrc_update __user *arg = __arg;
	struct io_uring_task *tctx = current->io_uring;
	struct io_uring_rsrc_update reg;
	int ret = 0, i;

	if (!nr_args || nr_args > IO_RINGFD_REG_MAX)
		return -EINVAL;
	if (!tctx)
		return 0;

	for (i = 0; i < nr_args; i++) {
		if (copy_from_user(&reg, &arg[i], sizeof(reg))) {
			ret = -EFAULT;
			break;
		}
		if (reg.resv || reg.data || reg.offset >= IO_RINGFD_REG_MAX) {
			ret = -EINVAL;
			break;
		}

		reg.offset = array_index_nospec(reg.offset, IO_RINGFD_REG_MAX);
		if (tctx->registered_rings[reg.offset]) {
			fput(tctx->registered_rings[reg.offset]);
			tctx->registered_rings[reg.offset] = NULL;
		}
	}

	return i ? i : ret;
}

static void *io_uring_validate_mmap_request(struct file *file,
					    loff_t pgoff, size_t sz)
{
	struct io_ring_ctx *ctx = file->private_data;
	loff_t offset = pgoff << PAGE_SHIFT;
	struct page *page;
	void *ptr;

	switch (offset) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		ptr = ctx->rings;
		break;
	case IORING_OFF_SQES:
		ptr = ctx->sq_sqes;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	page = virt_to_head_page(ptr);
	if (sz > page_size(page))
		return ERR_PTR(-EINVAL);

	return ptr;
}

#ifdef CONFIG_MMU

static __cold int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t sz = vma->vm_end - vma->vm_start;
	unsigned long pfn;
	void *ptr;

	ptr = io_uring_validate_mmap_request(file, vma->vm_pgoff, sz);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	pfn = virt_to_phys(ptr) >> PAGE_SHIFT;
	return remap_pfn_range(vma, vma->vm_start, pfn, sz, vma->vm_page_prot);
}

#else /* !CONFIG_MMU */

static int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	return vma->vm_flags & (VM_SHARED | VM_MAYSHARE) ? 0 : -EINVAL;
}

static unsigned int io_uring_nommu_mmap_capabilities(struct file *file)
{
	return NOMMU_MAP_DIRECT | NOMMU_MAP_READ | NOMMU_MAP_WRITE;
}

static unsigned long io_uring_nommu_get_unmapped_area(struct file *file,
	unsigned long addr, unsigned long len,
	unsigned long pgoff, unsigned long flags)
{
	void *ptr;

	ptr = io_uring_validate_mmap_request(file, pgoff, len);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	return (unsigned long) ptr;
}

#endif /* !CONFIG_MMU */

static int io_sqpoll_wait_sq(struct io_ring_ctx *ctx)
{
	DEFINE_WAIT(wait);

	do {
		if (!io_sqring_full(ctx))
			break;
		prepare_to_wait(&ctx->sqo_sq_wait, &wait, TASK_INTERRUPTIBLE);

		if (!io_sqring_full(ctx))
			break;
		schedule();
	} while (!signal_pending(current));

	finish_wait(&ctx->sqo_sq_wait, &wait);
	return 0;
}

static int io_validate_ext_arg(unsigned flags, const void __user *argp, size_t argsz)
{
	if (flags & IORING_ENTER_EXT_ARG) {
		struct io_uring_getevents_arg arg;

		if (argsz != sizeof(arg))
			return -EINVAL;
		if (copy_from_user(&arg, argp, sizeof(arg)))
			return -EFAULT;
	}
	return 0;
}

static int io_get_ext_arg(unsigned flags, const void __user *argp, size_t *argsz,
			  struct __kernel_timespec __user **ts,
			  const sigset_t __user **sig)
{
	struct io_uring_getevents_arg arg;

	/*
	 * If EXT_ARG isn't set, then we have no timespec and the argp pointer
	 * is just a pointer to the sigset_t.
	 */
	if (!(flags & IORING_ENTER_EXT_ARG)) {
		*sig = (const sigset_t __user *) argp;
		*ts = NULL;
		return 0;
	}

	/*
	 * EXT_ARG is set - ensure we agree on the size of it and copy in our
	 * timespec and sigset_t pointers if good.
	 */
	if (*argsz != sizeof(arg))
		return -EINVAL;
	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;
	if (arg.pad)
		return -EINVAL;
	*sig = u64_to_user_ptr(arg.sigmask);
	*argsz = arg.sigmask_sz;
	*ts = u64_to_user_ptr(arg.ts);
	return 0;
}

SYSCALL_DEFINE6(io_uring_enter, unsigned int, fd, u32, to_submit,
		u32, min_complete, u32, flags, const void __user *, argp,
		size_t, argsz)
{
	struct io_ring_ctx *ctx;
	struct fd f;
	long ret;

	io_run_task_work();

	if (unlikely(flags & ~(IORING_ENTER_GETEVENTS | IORING_ENTER_SQ_WAKEUP |
			       IORING_ENTER_SQ_WAIT | IORING_ENTER_EXT_ARG |
			       IORING_ENTER_REGISTERED_RING)))
		return -EINVAL;

	/*
	 * Ring fd has been registered via IORING_REGISTER_RING_FDS, we
	 * need only dereference our task private array to find it.
	 */
	if (flags & IORING_ENTER_REGISTERED_RING) {
		struct io_uring_task *tctx = current->io_uring;

		if (!tctx || fd >= IO_RINGFD_REG_MAX)
			return -EINVAL;
		fd = array_index_nospec(fd, IO_RINGFD_REG_MAX);
		f.file = tctx->registered_rings[fd];
		f.flags = 0;
	} else {
		f = fdget(fd);
	}

	if (unlikely(!f.file))
		return -EBADF;

	ret = -EOPNOTSUPP;
	if (unlikely(f.file->f_op != &io_uring_fops))
		goto out_fput;

	ret = -ENXIO;
	ctx = f.file->private_data;
	if (unlikely(!percpu_ref_tryget(&ctx->refs)))
		goto out_fput;

	ret = -EBADFD;
	if (unlikely(ctx->flags & IORING_SETUP_R_DISABLED))
		goto out;

	/*
	 * For SQ polling, the thread will do all submissions and completions.
	 * Just return the requested submit count, and wake the thread if
	 * we were asked to.
	 */
	ret = 0;
	if (ctx->flags & IORING_SETUP_SQPOLL) {
		io_cqring_overflow_flush(ctx);

		if (unlikely(ctx->sq_data->thread == NULL)) {
			ret = -EOWNERDEAD;
			goto out;
		}
		if (flags & IORING_ENTER_SQ_WAKEUP)
			wake_up(&ctx->sq_data->wait);
		if (flags & IORING_ENTER_SQ_WAIT) {
			ret = io_sqpoll_wait_sq(ctx);
			if (ret)
				goto out;
		}
		ret = to_submit;
	} else if (to_submit) {
		ret = io_uring_add_tctx_node(ctx);
		if (unlikely(ret))
			goto out;

		mutex_lock(&ctx->uring_lock);
		ret = io_submit_sqes(ctx, to_submit);
		if (ret != to_submit) {
			mutex_unlock(&ctx->uring_lock);
			goto out;
		}
		if ((flags & IORING_ENTER_GETEVENTS) && ctx->syscall_iopoll)
			goto iopoll_locked;
		mutex_unlock(&ctx->uring_lock);
	}
	if (flags & IORING_ENTER_GETEVENTS) {
		int ret2;
		if (ctx->syscall_iopoll) {
			/*
			 * We disallow the app entering submit/complete with
			 * polling, but we still need to lock the ring to
			 * prevent racing with polled issue that got punted to
			 * a workqueue.
			 */
			mutex_lock(&ctx->uring_lock);
iopoll_locked:
			ret2 = io_validate_ext_arg(flags, argp, argsz);
			if (likely(!ret2)) {
				min_complete = min(min_complete,
						   ctx->cq_entries);
				ret2 = io_iopoll_check(ctx, min_complete);
			}
			mutex_unlock(&ctx->uring_lock);
		} else {
			const sigset_t __user *sig;
			struct __kernel_timespec __user *ts;

			ret2 = io_get_ext_arg(flags, argp, &argsz, &ts, &sig);
			if (likely(!ret2)) {
				min_complete = min(min_complete,
						   ctx->cq_entries);
				ret2 = io_cqring_wait(ctx, min_complete, sig,
						      argsz, ts);
			}
		}

		if (!ret) {
			ret = ret2;

			/*
			 * EBADR indicates that one or more CQE were dropped.
			 * Once the user has been informed we can clear the bit
			 * as they are obviously ok with those drops.
			 */
			if (unlikely(ret2 == -EBADR))
				clear_bit(IO_CHECK_CQ_DROPPED_BIT,
					  &ctx->check_cq);
		}
	}

out:
	percpu_ref_put(&ctx->refs);
out_fput:
	fdput(f);
	return ret;
}

#ifdef CONFIG_PROC_FS
static __cold int io_uring_show_cred(struct seq_file *m, unsigned int id,
		const struct cred *cred)
{
	struct user_namespace *uns = seq_user_ns(m);
	struct group_info *gi;
	kernel_cap_t cap;
	unsigned __capi;
	int g;

	seq_printf(m, "%5d\n", id);
	seq_put_decimal_ull(m, "\tUid:\t", from_kuid_munged(uns, cred->uid));
	seq_put_decimal_ull(m, "\t\t", from_kuid_munged(uns, cred->euid));
	seq_put_decimal_ull(m, "\t\t", from_kuid_munged(uns, cred->suid));
	seq_put_decimal_ull(m, "\t\t", from_kuid_munged(uns, cred->fsuid));
	seq_put_decimal_ull(m, "\n\tGid:\t", from_kgid_munged(uns, cred->gid));
	seq_put_decimal_ull(m, "\t\t", from_kgid_munged(uns, cred->egid));
	seq_put_decimal_ull(m, "\t\t", from_kgid_munged(uns, cred->sgid));
	seq_put_decimal_ull(m, "\t\t", from_kgid_munged(uns, cred->fsgid));
	seq_puts(m, "\n\tGroups:\t");
	gi = cred->group_info;
	for (g = 0; g < gi->ngroups; g++) {
		seq_put_decimal_ull(m, g ? " " : "",
					from_kgid_munged(uns, gi->gid[g]));
	}
	seq_puts(m, "\n\tCapEff:\t");
	cap = cred->cap_effective;
	CAP_FOR_EACH_U32(__capi)
		seq_put_hex_ll(m, NULL, cap.cap[CAP_LAST_U32 - __capi], 8);
	seq_putc(m, '\n');
	return 0;
}

static __cold void __io_uring_show_fdinfo(struct io_ring_ctx *ctx,
					  struct seq_file *m)
{
	struct io_sq_data *sq = NULL;
	struct io_overflow_cqe *ocqe;
	struct io_rings *r = ctx->rings;
	unsigned int sq_mask = ctx->sq_entries - 1, cq_mask = ctx->cq_entries - 1;
	unsigned int sq_head = READ_ONCE(r->sq.head);
	unsigned int sq_tail = READ_ONCE(r->sq.tail);
	unsigned int cq_head = READ_ONCE(r->cq.head);
	unsigned int cq_tail = READ_ONCE(r->cq.tail);
	unsigned int cq_shift = 0;
	unsigned int sq_entries, cq_entries;
	bool has_lock;
	bool is_cqe32 = (ctx->flags & IORING_SETUP_CQE32);
	unsigned int i;

	if (is_cqe32)
		cq_shift = 1;

	/*
	 * we may get imprecise sqe and cqe info if uring is actively running
	 * since we get cached_sq_head and cached_cq_tail without uring_lock
	 * and sq_tail and cq_head are changed by userspace. But it's ok since
	 * we usually use these info when it is stuck.
	 */
	seq_printf(m, "SqMask:\t0x%x\n", sq_mask);
	seq_printf(m, "SqHead:\t%u\n", sq_head);
	seq_printf(m, "SqTail:\t%u\n", sq_tail);
	seq_printf(m, "CachedSqHead:\t%u\n", ctx->cached_sq_head);
	seq_printf(m, "CqMask:\t0x%x\n", cq_mask);
	seq_printf(m, "CqHead:\t%u\n", cq_head);
	seq_printf(m, "CqTail:\t%u\n", cq_tail);
	seq_printf(m, "CachedCqTail:\t%u\n", ctx->cached_cq_tail);
	seq_printf(m, "SQEs:\t%u\n", sq_tail - ctx->cached_sq_head);
	sq_entries = min(sq_tail - sq_head, ctx->sq_entries);
	for (i = 0; i < sq_entries; i++) {
		unsigned int entry = i + sq_head;
		unsigned int sq_idx = READ_ONCE(ctx->sq_array[entry & sq_mask]);
		struct io_uring_sqe *sqe;

		if (sq_idx > sq_mask)
			continue;
		sqe = &ctx->sq_sqes[sq_idx];
		seq_printf(m, "%5u: opcode:%d, fd:%d, flags:%x, user_data:%llu\n",
			   sq_idx, sqe->opcode, sqe->fd, sqe->flags,
			   sqe->user_data);
	}
	seq_printf(m, "CQEs:\t%u\n", cq_tail - cq_head);
	cq_entries = min(cq_tail - cq_head, ctx->cq_entries);
	for (i = 0; i < cq_entries; i++) {
		unsigned int entry = i + cq_head;
		struct io_uring_cqe *cqe = &r->cqes[(entry & cq_mask) << cq_shift];

		if (!is_cqe32) {
			seq_printf(m, "%5u: user_data:%llu, res:%d, flag:%x\n",
			   entry & cq_mask, cqe->user_data, cqe->res,
			   cqe->flags);
		} else {
			seq_printf(m, "%5u: user_data:%llu, res:%d, flag:%x, "
				"extra1:%llu, extra2:%llu\n",
				entry & cq_mask, cqe->user_data, cqe->res,
				cqe->flags, cqe->big_cqe[0], cqe->big_cqe[1]);
		}
	}

	/*
	 * Avoid ABBA deadlock between the seq lock and the io_uring mutex,
	 * since fdinfo case grabs it in the opposite direction of normal use
	 * cases. If we fail to get the lock, we just don't iterate any
	 * structures that could be going away outside the io_uring mutex.
	 */
	has_lock = mutex_trylock(&ctx->uring_lock);

	if (has_lock && (ctx->flags & IORING_SETUP_SQPOLL)) {
		sq = ctx->sq_data;
		if (!sq->thread)
			sq = NULL;
	}

	seq_printf(m, "SqThread:\t%d\n", sq ? task_pid_nr(sq->thread) : -1);
	seq_printf(m, "SqThreadCpu:\t%d\n", sq ? task_cpu(sq->thread) : -1);
	seq_printf(m, "UserFiles:\t%u\n", ctx->nr_user_files);
	for (i = 0; has_lock && i < ctx->nr_user_files; i++) {
		struct file *f = io_file_from_index(ctx, i);

		if (f)
			seq_printf(m, "%5u: %s\n", i, file_dentry(f)->d_iname);
		else
			seq_printf(m, "%5u: <none>\n", i);
	}
	seq_printf(m, "UserBufs:\t%u\n", ctx->nr_user_bufs);
	for (i = 0; has_lock && i < ctx->nr_user_bufs; i++) {
		struct io_mapped_ubuf *buf = ctx->user_bufs[i];
		unsigned int len = buf->ubuf_end - buf->ubuf;

		seq_printf(m, "%5u: 0x%llx/%u\n", i, buf->ubuf, len);
	}
	if (has_lock && !xa_empty(&ctx->personalities)) {
		unsigned long index;
		const struct cred *cred;

		seq_printf(m, "Personalities:\n");
		xa_for_each(&ctx->personalities, index, cred)
			io_uring_show_cred(m, index, cred);
	}
	if (has_lock)
		mutex_unlock(&ctx->uring_lock);

	seq_puts(m, "PollList:\n");
	spin_lock(&ctx->completion_lock);
	for (i = 0; i < (1U << ctx->cancel_hash_bits); i++) {
		struct hlist_head *list = &ctx->cancel_hash[i];
		struct io_kiocb *req;

		hlist_for_each_entry(req, list, hash_node)
			seq_printf(m, "  op=%d, task_works=%d\n", req->opcode,
					task_work_pending(req->task));
	}

	seq_puts(m, "CqOverflowList:\n");
	list_for_each_entry(ocqe, &ctx->cq_overflow_list, list) {
		struct io_uring_cqe *cqe = &ocqe->cqe;

		seq_printf(m, "  user_data=%llu, res=%d, flags=%x\n",
			   cqe->user_data, cqe->res, cqe->flags);

	}

	spin_unlock(&ctx->completion_lock);
}

static __cold void io_uring_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct io_ring_ctx *ctx = f->private_data;

	if (percpu_ref_tryget(&ctx->refs)) {
		__io_uring_show_fdinfo(ctx, m);
		percpu_ref_put(&ctx->refs);
	}
}
#endif

static const struct file_operations io_uring_fops = {
	.release	= io_uring_release,
	.mmap		= io_uring_mmap,
#ifndef CONFIG_MMU
	.get_unmapped_area = io_uring_nommu_get_unmapped_area,
	.mmap_capabilities = io_uring_nommu_mmap_capabilities,
#endif
	.poll		= io_uring_poll,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= io_uring_show_fdinfo,
#endif
};

static __cold int io_allocate_scq_urings(struct io_ring_ctx *ctx,
					 struct io_uring_params *p)
{
	struct io_rings *rings;
	size_t size, sq_array_offset;

	/* make sure these are sane, as we already accounted them */
	ctx->sq_entries = p->sq_entries;
	ctx->cq_entries = p->cq_entries;

	size = rings_size(ctx, p->sq_entries, p->cq_entries, &sq_array_offset);
	if (size == SIZE_MAX)
		return -EOVERFLOW;

	rings = io_mem_alloc(size);
	if (!rings)
		return -ENOMEM;

	ctx->rings = rings;
	ctx->sq_array = (u32 *)((char *)rings + sq_array_offset);
	rings->sq_ring_mask = p->sq_entries - 1;
	rings->cq_ring_mask = p->cq_entries - 1;
	rings->sq_ring_entries = p->sq_entries;
	rings->cq_ring_entries = p->cq_entries;

	if (p->flags & IORING_SETUP_SQE128)
		size = array_size(2 * sizeof(struct io_uring_sqe), p->sq_entries);
	else
		size = array_size(sizeof(struct io_uring_sqe), p->sq_entries);
	if (size == SIZE_MAX) {
		io_mem_free(ctx->rings);
		ctx->rings = NULL;
		return -EOVERFLOW;
	}

	ctx->sq_sqes = io_mem_alloc(size);
	if (!ctx->sq_sqes) {
		io_mem_free(ctx->rings);
		ctx->rings = NULL;
		return -ENOMEM;
	}

	return 0;
}

static int io_uring_install_fd(struct io_ring_ctx *ctx, struct file *file)
{
	int ret, fd;

	fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return fd;

	ret = io_uring_add_tctx_node(ctx);
	if (ret) {
		put_unused_fd(fd);
		return ret;
	}
	fd_install(fd, file);
	return fd;
}

/*
 * Allocate an anonymous fd, this is what constitutes the application
 * visible backing of an io_uring instance. The application mmaps this
 * fd to gain access to the SQ/CQ ring details. If UNIX sockets are enabled,
 * we have to tie this fd to a socket for file garbage collection purposes.
 */
static struct file *io_uring_get_file(struct io_ring_ctx *ctx)
{
	struct file *file;
#if defined(CONFIG_UNIX)
	int ret;

	ret = sock_create_kern(&init_net, PF_UNIX, SOCK_RAW, IPPROTO_IP,
				&ctx->ring_sock);
	if (ret)
		return ERR_PTR(ret);
#endif

	file = anon_inode_getfile_secure("[io_uring]", &io_uring_fops, ctx,
					 O_RDWR | O_CLOEXEC, NULL);
#if defined(CONFIG_UNIX)
	if (IS_ERR(file)) {
		sock_release(ctx->ring_sock);
		ctx->ring_sock = NULL;
	} else {
		ctx->ring_sock->file = file;
	}
#endif
	return file;
}

static __cold int io_uring_create(unsigned entries, struct io_uring_params *p,
				  struct io_uring_params __user *params)
{
	struct io_ring_ctx *ctx;
	struct file *file;
	int ret;

	if (!entries)
		return -EINVAL;
	if (entries > IORING_MAX_ENTRIES) {
		if (!(p->flags & IORING_SETUP_CLAMP))
			return -EINVAL;
		entries = IORING_MAX_ENTRIES;
	}

	/*
	 * Use twice as many entries for the CQ ring. It's possible for the
	 * application to drive a higher depth than the size of the SQ ring,
	 * since the sqes are only used at submission time. This allows for
	 * some flexibility in overcommitting a bit. If the application has
	 * set IORING_SETUP_CQSIZE, it will have passed in the desired number
	 * of CQ ring entries manually.
	 */
	p->sq_entries = roundup_pow_of_two(entries);
	if (p->flags & IORING_SETUP_CQSIZE) {
		/*
		 * If IORING_SETUP_CQSIZE is set, we do the same roundup
		 * to a power-of-two, if it isn't already. We do NOT impose
		 * any cq vs sq ring sizing.
		 */
		if (!p->cq_entries)
			return -EINVAL;
		if (p->cq_entries > IORING_MAX_CQ_ENTRIES) {
			if (!(p->flags & IORING_SETUP_CLAMP))
				return -EINVAL;
			p->cq_entries = IORING_MAX_CQ_ENTRIES;
		}
		p->cq_entries = roundup_pow_of_two(p->cq_entries);
		if (p->cq_entries < p->sq_entries)
			return -EINVAL;
	} else {
		p->cq_entries = 2 * p->sq_entries;
	}

	ctx = io_ring_ctx_alloc(p);
	if (!ctx)
		return -ENOMEM;

	/*
	 * When SETUP_IOPOLL and SETUP_SQPOLL are both enabled, user
	 * space applications don't need to do io completion events
	 * polling again, they can rely on io_sq_thread to do polling
	 * work, which can reduce cpu usage and uring_lock contention.
	 */
	if (ctx->flags & IORING_SETUP_IOPOLL &&
	    !(ctx->flags & IORING_SETUP_SQPOLL))
		ctx->syscall_iopoll = 1;

	ctx->compat = in_compat_syscall();
	if (!capable(CAP_IPC_LOCK))
		ctx->user = get_uid(current_user());

	/*
	 * For SQPOLL, we just need a wakeup, always. For !SQPOLL, if
	 * COOP_TASKRUN is set, then IPIs are never needed by the app.
	 */
	ret = -EINVAL;
	if (ctx->flags & IORING_SETUP_SQPOLL) {
		/* IPI related flags don't make sense with SQPOLL */
		if (ctx->flags & (IORING_SETUP_COOP_TASKRUN |
				  IORING_SETUP_TASKRUN_FLAG))
			goto err;
		ctx->notify_method = TWA_SIGNAL_NO_IPI;
	} else if (ctx->flags & IORING_SETUP_COOP_TASKRUN) {
		ctx->notify_method = TWA_SIGNAL_NO_IPI;
	} else {
		if (ctx->flags & IORING_SETUP_TASKRUN_FLAG)
			goto err;
		ctx->notify_method = TWA_SIGNAL;
	}

	/*
	 * This is just grabbed for accounting purposes. When a process exits,
	 * the mm is exited and dropped before the files, hence we need to hang
	 * on to this mm purely for the purposes of being able to unaccount
	 * memory (locked/pinned vm). It's not used for anything else.
	 */
	mmgrab(current->mm);
	ctx->mm_account = current->mm;

	ret = io_allocate_scq_urings(ctx, p);
	if (ret)
		goto err;

	ret = io_sq_offload_create(ctx, p);
	if (ret)
		goto err;
	/* always set a rsrc node */
	ret = io_rsrc_node_switch_start(ctx);
	if (ret)
		goto err;
	io_rsrc_node_switch(ctx, NULL);

	memset(&p->sq_off, 0, sizeof(p->sq_off));
	p->sq_off.head = offsetof(struct io_rings, sq.head);
	p->sq_off.tail = offsetof(struct io_rings, sq.tail);
	p->sq_off.ring_mask = offsetof(struct io_rings, sq_ring_mask);
	p->sq_off.ring_entries = offsetof(struct io_rings, sq_ring_entries);
	p->sq_off.flags = offsetof(struct io_rings, sq_flags);
	p->sq_off.dropped = offsetof(struct io_rings, sq_dropped);
	p->sq_off.array = (char *)ctx->sq_array - (char *)ctx->rings;

	memset(&p->cq_off, 0, sizeof(p->cq_off));
	p->cq_off.head = offsetof(struct io_rings, cq.head);
	p->cq_off.tail = offsetof(struct io_rings, cq.tail);
	p->cq_off.ring_mask = offsetof(struct io_rings, cq_ring_mask);
	p->cq_off.ring_entries = offsetof(struct io_rings, cq_ring_entries);
	p->cq_off.overflow = offsetof(struct io_rings, cq_overflow);
	p->cq_off.cqes = offsetof(struct io_rings, cqes);
	p->cq_off.flags = offsetof(struct io_rings, cq_flags);

	p->features = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP |
			IORING_FEAT_SUBMIT_STABLE | IORING_FEAT_RW_CUR_POS |
			IORING_FEAT_CUR_PERSONALITY | IORING_FEAT_FAST_POLL |
			IORING_FEAT_POLL_32BITS | IORING_FEAT_SQPOLL_NONFIXED |
			IORING_FEAT_EXT_ARG | IORING_FEAT_NATIVE_WORKERS |
			IORING_FEAT_RSRC_TAGS | IORING_FEAT_CQE_SKIP |
			IORING_FEAT_LINKED_FILE;

	if (copy_to_user(params, p, sizeof(*p))) {
		ret = -EFAULT;
		goto err;
	}

	file = io_uring_get_file(ctx);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err;
	}

	/*
	 * Install ring fd as the very last thing, so we don't risk someone
	 * having closed it before we finish setup
	 */
	ret = io_uring_install_fd(ctx, file);
	if (ret < 0) {
		/* fput will clean it up */
		fput(file);
		return ret;
	}

	trace_io_uring_create(ret, ctx, p->sq_entries, p->cq_entries, p->flags);
	return ret;
err:
	io_ring_ctx_wait_and_kill(ctx);
	return ret;
}

/*
 * Sets up an aio uring context, and returns the fd. Applications asks for a
 * ring size, we return the actual sq/cq ring sizes (among other things) in the
 * params structure passed in.
 */
static long io_uring_setup(u32 entries, struct io_uring_params __user *params)
{
	struct io_uring_params p;
	int i;

	if (copy_from_user(&p, params, sizeof(p)))
		return -EFAULT;
	for (i = 0; i < ARRAY_SIZE(p.resv); i++) {
		if (p.resv[i])
			return -EINVAL;
	}

	if (p.flags & ~(IORING_SETUP_IOPOLL | IORING_SETUP_SQPOLL |
			IORING_SETUP_SQ_AFF | IORING_SETUP_CQSIZE |
			IORING_SETUP_CLAMP | IORING_SETUP_ATTACH_WQ |
			IORING_SETUP_R_DISABLED | IORING_SETUP_SUBMIT_ALL |
			IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
			IORING_SETUP_SQE128 | IORING_SETUP_CQE32))
		return -EINVAL;

	return io_uring_create(entries, &p, params);
}

SYSCALL_DEFINE2(io_uring_setup, u32, entries,
		struct io_uring_params __user *, params)
{
	return io_uring_setup(entries, params);
}

static __cold int io_probe(struct io_ring_ctx *ctx, void __user *arg,
			   unsigned nr_args)
{
	struct io_uring_probe *p;
	size_t size;
	int i, ret;

	size = struct_size(p, ops, nr_args);
	if (size == SIZE_MAX)
		return -EOVERFLOW;
	p = kzalloc(size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(p, arg, size))
		goto out;
	ret = -EINVAL;
	if (memchr_inv(p, 0, size))
		goto out;

	p->last_op = IORING_OP_LAST - 1;
	if (nr_args > IORING_OP_LAST)
		nr_args = IORING_OP_LAST;

	for (i = 0; i < nr_args; i++) {
		p->ops[i].op = i;
		if (!io_op_defs[i].not_supported)
			p->ops[i].flags = IO_URING_OP_SUPPORTED;
	}
	p->ops_len = i;

	ret = 0;
	if (copy_to_user(arg, p, size))
		ret = -EFAULT;
out:
	kfree(p);
	return ret;
}

static int io_register_personality(struct io_ring_ctx *ctx)
{
	const struct cred *creds;
	u32 id;
	int ret;

	creds = get_current_cred();

	ret = xa_alloc_cyclic(&ctx->personalities, &id, (void *)creds,
			XA_LIMIT(0, USHRT_MAX), &ctx->pers_next, GFP_KERNEL);
	if (ret < 0) {
		put_cred(creds);
		return ret;
	}
	return id;
}

static __cold int io_register_restrictions(struct io_ring_ctx *ctx,
					   void __user *arg, unsigned int nr_args)
{
	struct io_uring_restriction *res;
	size_t size;
	int i, ret;

	/* Restrictions allowed only if rings started disabled */
	if (!(ctx->flags & IORING_SETUP_R_DISABLED))
		return -EBADFD;

	/* We allow only a single restrictions registration */
	if (ctx->restrictions.registered)
		return -EBUSY;

	if (!arg || nr_args > IORING_MAX_RESTRICTIONS)
		return -EINVAL;

	size = array_size(nr_args, sizeof(*res));
	if (size == SIZE_MAX)
		return -EOVERFLOW;

	res = memdup_user(arg, size);
	if (IS_ERR(res))
		return PTR_ERR(res);

	ret = 0;

	for (i = 0; i < nr_args; i++) {
		switch (res[i].opcode) {
		case IORING_RESTRICTION_REGISTER_OP:
			if (res[i].register_op >= IORING_REGISTER_LAST) {
				ret = -EINVAL;
				goto out;
			}

			__set_bit(res[i].register_op,
				  ctx->restrictions.register_op);
			break;
		case IORING_RESTRICTION_SQE_OP:
			if (res[i].sqe_op >= IORING_OP_LAST) {
				ret = -EINVAL;
				goto out;
			}

			__set_bit(res[i].sqe_op, ctx->restrictions.sqe_op);
			break;
		case IORING_RESTRICTION_SQE_FLAGS_ALLOWED:
			ctx->restrictions.sqe_flags_allowed = res[i].sqe_flags;
			break;
		case IORING_RESTRICTION_SQE_FLAGS_REQUIRED:
			ctx->restrictions.sqe_flags_required = res[i].sqe_flags;
			break;
		default:
			ret = -EINVAL;
			goto out;
		}
	}

out:
	/* Reset all restrictions if an error happened */
	if (ret != 0)
		memset(&ctx->restrictions, 0, sizeof(ctx->restrictions));
	else
		ctx->restrictions.registered = true;

	kfree(res);
	return ret;
}

static int io_register_enable_rings(struct io_ring_ctx *ctx)
{
	if (!(ctx->flags & IORING_SETUP_R_DISABLED))
		return -EBADFD;

	if (ctx->restrictions.registered)
		ctx->restricted = 1;

	ctx->flags &= ~IORING_SETUP_R_DISABLED;
	if (ctx->sq_data && wq_has_sleeper(&ctx->sq_data->wait))
		wake_up(&ctx->sq_data->wait);
	return 0;
}

static int __io_register_rsrc_update(struct io_ring_ctx *ctx, unsigned type,
				     struct io_uring_rsrc_update2 *up,
				     unsigned nr_args)
{
	__u32 tmp;
	int err;

	if (check_add_overflow(up->offset, nr_args, &tmp))
		return -EOVERFLOW;
	err = io_rsrc_node_switch_start(ctx);
	if (err)
		return err;

	switch (type) {
	case IORING_RSRC_FILE:
		return __io_sqe_files_update(ctx, up, nr_args);
	case IORING_RSRC_BUFFER:
		return __io_sqe_buffers_update(ctx, up, nr_args);
	}
	return -EINVAL;
}

static int io_register_files_update(struct io_ring_ctx *ctx, void __user *arg,
				    unsigned nr_args)
{
	struct io_uring_rsrc_update2 up;

	if (!nr_args)
		return -EINVAL;
	memset(&up, 0, sizeof(up));
	if (copy_from_user(&up, arg, sizeof(struct io_uring_rsrc_update)))
		return -EFAULT;
	if (up.resv || up.resv2)
		return -EINVAL;
	return __io_register_rsrc_update(ctx, IORING_RSRC_FILE, &up, nr_args);
}

static int io_register_rsrc_update(struct io_ring_ctx *ctx, void __user *arg,
				   unsigned size, unsigned type)
{
	struct io_uring_rsrc_update2 up;

	if (size != sizeof(up))
		return -EINVAL;
	if (copy_from_user(&up, arg, sizeof(up)))
		return -EFAULT;
	if (!up.nr || up.resv || up.resv2)
		return -EINVAL;
	return __io_register_rsrc_update(ctx, type, &up, up.nr);
}

static __cold int io_register_rsrc(struct io_ring_ctx *ctx, void __user *arg,
			    unsigned int size, unsigned int type)
{
	struct io_uring_rsrc_register rr;

	/* keep it extendible */
	if (size != sizeof(rr))
		return -EINVAL;

	memset(&rr, 0, sizeof(rr));
	if (copy_from_user(&rr, arg, size))
		return -EFAULT;
	if (!rr.nr || rr.resv2)
		return -EINVAL;
	if (rr.flags & ~IORING_RSRC_REGISTER_SPARSE)
		return -EINVAL;

	switch (type) {
	case IORING_RSRC_FILE:
		if (rr.flags & IORING_RSRC_REGISTER_SPARSE && rr.data)
			break;
		return io_sqe_files_register(ctx, u64_to_user_ptr(rr.data),
					     rr.nr, u64_to_user_ptr(rr.tags));
	case IORING_RSRC_BUFFER:
		if (rr.flags & IORING_RSRC_REGISTER_SPARSE && rr.data)
			break;
		return io_sqe_buffers_register(ctx, u64_to_user_ptr(rr.data),
					       rr.nr, u64_to_user_ptr(rr.tags));
	}
	return -EINVAL;
}

static __cold int io_register_iowq_aff(struct io_ring_ctx *ctx,
				       void __user *arg, unsigned len)
{
	struct io_uring_task *tctx = current->io_uring;
	cpumask_var_t new_mask;
	int ret;

	if (!tctx || !tctx->io_wq)
		return -EINVAL;

	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL))
		return -ENOMEM;

	cpumask_clear(new_mask);
	if (len > cpumask_size())
		len = cpumask_size();

	if (in_compat_syscall()) {
		ret = compat_get_bitmap(cpumask_bits(new_mask),
					(const compat_ulong_t __user *)arg,
					len * 8 /* CHAR_BIT */);
	} else {
		ret = copy_from_user(new_mask, arg, len);
	}

	if (ret) {
		free_cpumask_var(new_mask);
		return -EFAULT;
	}

	ret = io_wq_cpu_affinity(tctx->io_wq, new_mask);
	free_cpumask_var(new_mask);
	return ret;
}

static __cold int io_unregister_iowq_aff(struct io_ring_ctx *ctx)
{
	struct io_uring_task *tctx = current->io_uring;

	if (!tctx || !tctx->io_wq)
		return -EINVAL;

	return io_wq_cpu_affinity(tctx->io_wq, NULL);
}

static __cold int io_register_iowq_max_workers(struct io_ring_ctx *ctx,
					       void __user *arg)
	__must_hold(&ctx->uring_lock)
{
	struct io_tctx_node *node;
	struct io_uring_task *tctx = NULL;
	struct io_sq_data *sqd = NULL;
	__u32 new_count[2];
	int i, ret;

	if (copy_from_user(new_count, arg, sizeof(new_count)))
		return -EFAULT;
	for (i = 0; i < ARRAY_SIZE(new_count); i++)
		if (new_count[i] > INT_MAX)
			return -EINVAL;

	if (ctx->flags & IORING_SETUP_SQPOLL) {
		sqd = ctx->sq_data;
		if (sqd) {
			/*
			 * Observe the correct sqd->lock -> ctx->uring_lock
			 * ordering. Fine to drop uring_lock here, we hold
			 * a ref to the ctx.
			 */
			refcount_inc(&sqd->refs);
			mutex_unlock(&ctx->uring_lock);
			mutex_lock(&sqd->lock);
			mutex_lock(&ctx->uring_lock);
			if (sqd->thread)
				tctx = sqd->thread->io_uring;
		}
	} else {
		tctx = current->io_uring;
	}

	BUILD_BUG_ON(sizeof(new_count) != sizeof(ctx->iowq_limits));

	for (i = 0; i < ARRAY_SIZE(new_count); i++)
		if (new_count[i])
			ctx->iowq_limits[i] = new_count[i];
	ctx->iowq_limits_set = true;

	if (tctx && tctx->io_wq) {
		ret = io_wq_max_workers(tctx->io_wq, new_count);
		if (ret)
			goto err;
	} else {
		memset(new_count, 0, sizeof(new_count));
	}

	if (sqd) {
		mutex_unlock(&sqd->lock);
		io_put_sq_data(sqd);
	}

	if (copy_to_user(arg, new_count, sizeof(new_count)))
		return -EFAULT;

	/* that's it for SQPOLL, only the SQPOLL task creates requests */
	if (sqd)
		return 0;

	/* now propagate the restriction to all registered users */
	list_for_each_entry(node, &ctx->tctx_list, ctx_node) {
		struct io_uring_task *tctx = node->task->io_uring;

		if (WARN_ON_ONCE(!tctx->io_wq))
			continue;

		for (i = 0; i < ARRAY_SIZE(new_count); i++)
			new_count[i] = ctx->iowq_limits[i];
		/* ignore errors, it always returns zero anyway */
		(void)io_wq_max_workers(tctx->io_wq, new_count);
	}
	return 0;
err:
	if (sqd) {
		mutex_unlock(&sqd->lock);
		io_put_sq_data(sqd);
	}
	return ret;
}

static int io_register_pbuf_ring(struct io_ring_ctx *ctx, void __user *arg)
{
	struct io_uring_buf_ring *br;
	struct io_uring_buf_reg reg;
	struct io_buffer_list *bl;
	struct page **pages;
	int nr_pages;

	if (copy_from_user(&reg, arg, sizeof(reg)))
		return -EFAULT;

	if (reg.pad || reg.resv[0] || reg.resv[1] || reg.resv[2])
		return -EINVAL;
	if (!reg.ring_addr)
		return -EFAULT;
	if (reg.ring_addr & ~PAGE_MASK)
		return -EINVAL;
	if (!is_power_of_2(reg.ring_entries))
		return -EINVAL;

	/* cannot disambiguate full vs empty due to head/tail size */
	if (reg.ring_entries >= 65536)
		return -EINVAL;

	if (unlikely(reg.bgid < BGID_ARRAY && !ctx->io_bl)) {
		int ret = io_init_bl_list(ctx);
		if (ret)
			return ret;
	}

	bl = io_buffer_get_list(ctx, reg.bgid);
	if (bl) {
		/* if mapped buffer ring OR classic exists, don't allow */
		if (bl->buf_nr_pages || !list_empty(&bl->buf_list))
			return -EEXIST;
	} else {
		bl = kzalloc(sizeof(*bl), GFP_KERNEL);
		if (!bl)
			return -ENOMEM;
	}

	pages = io_pin_pages(reg.ring_addr,
			     struct_size(br, bufs, reg.ring_entries),
			     &nr_pages);
	if (IS_ERR(pages)) {
		kfree(bl);
		return PTR_ERR(pages);
	}

	br = page_address(pages[0]);
	bl->buf_pages = pages;
	bl->buf_nr_pages = nr_pages;
	bl->nr_entries = reg.ring_entries;
	bl->buf_ring = br;
	bl->mask = reg.ring_entries - 1;
	io_buffer_add_list(ctx, bl, reg.bgid);
	return 0;
}

static int io_unregister_pbuf_ring(struct io_ring_ctx *ctx, void __user *arg)
{
	struct io_uring_buf_reg reg;
	struct io_buffer_list *bl;

	if (copy_from_user(&reg, arg, sizeof(reg)))
		return -EFAULT;
	if (reg.pad || reg.resv[0] || reg.resv[1] || reg.resv[2])
		return -EINVAL;

	bl = io_buffer_get_list(ctx, reg.bgid);
	if (!bl)
		return -ENOENT;
	if (!bl->buf_nr_pages)
		return -EINVAL;

	__io_remove_buffers(ctx, bl, -1U);
	if (bl->bgid >= BGID_ARRAY) {
		xa_erase(&ctx->io_bl_xa, bl->bgid);
		kfree(bl);
	}
	return 0;
}

static int __io_uring_register(struct io_ring_ctx *ctx, unsigned opcode,
			       void __user *arg, unsigned nr_args)
	__releases(ctx->uring_lock)
	__acquires(ctx->uring_lock)
{
	int ret;

	/*
	 * We're inside the ring mutex, if the ref is already dying, then
	 * someone else killed the ctx or is already going through
	 * io_uring_register().
	 */
	if (percpu_ref_is_dying(&ctx->refs))
		return -ENXIO;

	if (ctx->restricted) {
		if (opcode >= IORING_REGISTER_LAST)
			return -EINVAL;
		opcode = array_index_nospec(opcode, IORING_REGISTER_LAST);
		if (!test_bit(opcode, ctx->restrictions.register_op))
			return -EACCES;
	}

	switch (opcode) {
	case IORING_REGISTER_BUFFERS:
		ret = -EFAULT;
		if (!arg)
			break;
		ret = io_sqe_buffers_register(ctx, arg, nr_args, NULL);
		break;
	case IORING_UNREGISTER_BUFFERS:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_sqe_buffers_unregister(ctx);
		break;
	case IORING_REGISTER_FILES:
		ret = -EFAULT;
		if (!arg)
			break;
		ret = io_sqe_files_register(ctx, arg, nr_args, NULL);
		break;
	case IORING_UNREGISTER_FILES:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_sqe_files_unregister(ctx);
		break;
	case IORING_REGISTER_FILES_UPDATE:
		ret = io_register_files_update(ctx, arg, nr_args);
		break;
	case IORING_REGISTER_EVENTFD:
		ret = -EINVAL;
		if (nr_args != 1)
			break;
		ret = io_eventfd_register(ctx, arg, 0);
		break;
	case IORING_REGISTER_EVENTFD_ASYNC:
		ret = -EINVAL;
		if (nr_args != 1)
			break;
		ret = io_eventfd_register(ctx, arg, 1);
		break;
	case IORING_UNREGISTER_EVENTFD:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_eventfd_unregister(ctx);
		break;
	case IORING_REGISTER_PROBE:
		ret = -EINVAL;
		if (!arg || nr_args > 256)
			break;
		ret = io_probe(ctx, arg, nr_args);
		break;
	case IORING_REGISTER_PERSONALITY:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_register_personality(ctx);
		break;
	case IORING_UNREGISTER_PERSONALITY:
		ret = -EINVAL;
		if (arg)
			break;
		ret = io_unregister_personality(ctx, nr_args);
		break;
	case IORING_REGISTER_ENABLE_RINGS:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_register_enable_rings(ctx);
		break;
	case IORING_REGISTER_RESTRICTIONS:
		ret = io_register_restrictions(ctx, arg, nr_args);
		break;
	case IORING_REGISTER_FILES2:
		ret = io_register_rsrc(ctx, arg, nr_args, IORING_RSRC_FILE);
		break;
	case IORING_REGISTER_FILES_UPDATE2:
		ret = io_register_rsrc_update(ctx, arg, nr_args,
					      IORING_RSRC_FILE);
		break;
	case IORING_REGISTER_BUFFERS2:
		ret = io_register_rsrc(ctx, arg, nr_args, IORING_RSRC_BUFFER);
		break;
	case IORING_REGISTER_BUFFERS_UPDATE:
		ret = io_register_rsrc_update(ctx, arg, nr_args,
					      IORING_RSRC_BUFFER);
		break;
	case IORING_REGISTER_IOWQ_AFF:
		ret = -EINVAL;
		if (!arg || !nr_args)
			break;
		ret = io_register_iowq_aff(ctx, arg, nr_args);
		break;
	case IORING_UNREGISTER_IOWQ_AFF:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_unregister_iowq_aff(ctx);
		break;
	case IORING_REGISTER_IOWQ_MAX_WORKERS:
		ret = -EINVAL;
		if (!arg || nr_args != 2)
			break;
		ret = io_register_iowq_max_workers(ctx, arg);
		break;
	case IORING_REGISTER_RING_FDS:
		ret = io_ringfd_register(ctx, arg, nr_args);
		break;
	case IORING_UNREGISTER_RING_FDS:
		ret = io_ringfd_unregister(ctx, arg, nr_args);
		break;
	case IORING_REGISTER_PBUF_RING:
		ret = -EINVAL;
		if (!arg || nr_args != 1)
			break;
		ret = io_register_pbuf_ring(ctx, arg);
		break;
	case IORING_UNREGISTER_PBUF_RING:
		ret = -EINVAL;
		if (!arg || nr_args != 1)
			break;
		ret = io_unregister_pbuf_ring(ctx, arg);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

SYSCALL_DEFINE4(io_uring_register, unsigned int, fd, unsigned int, opcode,
		void __user *, arg, unsigned int, nr_args)
{
	struct io_ring_ctx *ctx;
	long ret = -EBADF;
	struct fd f;

	f = fdget(fd);
	if (!f.file)
		return -EBADF;

	ret = -EOPNOTSUPP;
	if (f.file->f_op != &io_uring_fops)
		goto out_fput;

	ctx = f.file->private_data;

	io_run_task_work();

	mutex_lock(&ctx->uring_lock);
	ret = __io_uring_register(ctx, opcode, arg, nr_args);
	mutex_unlock(&ctx->uring_lock);
	trace_io_uring_register(ctx, opcode, ctx->nr_user_files, ctx->nr_user_bufs, ret);
out_fput:
	fdput(f);
	return ret;
}

static int __init io_uring_init(void)
{
#define __BUILD_BUG_VERIFY_ELEMENT(stype, eoffset, etype, ename) do { \
	BUILD_BUG_ON(offsetof(stype, ename) != eoffset); \
	BUILD_BUG_ON(sizeof(etype) != sizeof_field(stype, ename)); \
} while (0)

#define BUILD_BUG_SQE_ELEM(eoffset, etype, ename) \
	__BUILD_BUG_VERIFY_ELEMENT(struct io_uring_sqe, eoffset, etype, ename)
	BUILD_BUG_ON(sizeof(struct io_uring_sqe) != 64);
	BUILD_BUG_SQE_ELEM(0,  __u8,   opcode);
	BUILD_BUG_SQE_ELEM(1,  __u8,   flags);
	BUILD_BUG_SQE_ELEM(2,  __u16,  ioprio);
	BUILD_BUG_SQE_ELEM(4,  __s32,  fd);
	BUILD_BUG_SQE_ELEM(8,  __u64,  off);
	BUILD_BUG_SQE_ELEM(8,  __u64,  addr2);
	BUILD_BUG_SQE_ELEM(16, __u64,  addr);
	BUILD_BUG_SQE_ELEM(16, __u64,  splice_off_in);
	BUILD_BUG_SQE_ELEM(24, __u32,  len);
	BUILD_BUG_SQE_ELEM(28,     __kernel_rwf_t, rw_flags);
	BUILD_BUG_SQE_ELEM(28, /* compat */   int, rw_flags);
	BUILD_BUG_SQE_ELEM(28, /* compat */ __u32, rw_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  fsync_flags);
	BUILD_BUG_SQE_ELEM(28, /* compat */ __u16,  poll_events);
	BUILD_BUG_SQE_ELEM(28, __u32,  poll32_events);
	BUILD_BUG_SQE_ELEM(28, __u32,  sync_range_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  msg_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  timeout_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  accept_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  cancel_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  open_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  statx_flags);
	BUILD_BUG_SQE_ELEM(28, __u32,  fadvise_advice);
	BUILD_BUG_SQE_ELEM(28, __u32,  splice_flags);
	BUILD_BUG_SQE_ELEM(32, __u64,  user_data);
	BUILD_BUG_SQE_ELEM(40, __u16,  buf_index);
	BUILD_BUG_SQE_ELEM(40, __u16,  buf_group);
	BUILD_BUG_SQE_ELEM(42, __u16,  personality);
	BUILD_BUG_SQE_ELEM(44, __s32,  splice_fd_in);
	BUILD_BUG_SQE_ELEM(44, __u32,  file_index);
	BUILD_BUG_SQE_ELEM(48, __u64,  addr3);

	BUILD_BUG_ON(sizeof(struct io_uring_files_update) !=
		     sizeof(struct io_uring_rsrc_update));
	BUILD_BUG_ON(sizeof(struct io_uring_rsrc_update) >
		     sizeof(struct io_uring_rsrc_update2));

	/* ->buf_index is u16 */
	BUILD_BUG_ON(IORING_MAX_REG_BUFFERS >= (1u << 16));
	BUILD_BUG_ON(BGID_ARRAY * sizeof(struct io_buffer_list) > PAGE_SIZE);
	BUILD_BUG_ON(offsetof(struct io_uring_buf_ring, bufs) != 0);
	BUILD_BUG_ON(offsetof(struct io_uring_buf, resv) !=
		     offsetof(struct io_uring_buf_ring, tail));

	/* should fit into one byte */
	BUILD_BUG_ON(SQE_VALID_FLAGS >= (1 << 8));
	BUILD_BUG_ON(SQE_COMMON_FLAGS >= (1 << 8));
	BUILD_BUG_ON((SQE_VALID_FLAGS | SQE_COMMON_FLAGS) != SQE_VALID_FLAGS);

	BUILD_BUG_ON(ARRAY_SIZE(io_op_defs) != IORING_OP_LAST);
	BUILD_BUG_ON(__REQ_F_LAST_BIT > 8 * sizeof(int));

	BUILD_BUG_ON(sizeof(atomic_t) != sizeof(u32));

	BUILD_BUG_ON(sizeof(struct io_uring_cmd) > 64);

	req_cachep = KMEM_CACHE(io_kiocb, SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				SLAB_ACCOUNT);
	return 0;
};
__initcall(io_uring_init);
