#ifndef _VMON_H
#define _VMON_H

#include <sys/types.h>
#include <dirent.h>
#include <stdint.h>
#include <string.h>	/* I use strcmp() in the type comparator definitions */

#include "bitmap.h"
#include "list.h"

#define VMON_HTAB_SIZE		128				/* number of buckets in the processes hash table */
#define VMON_ARRAY_GROWBY	5				/* number of elements to grow the processes array */

typedef enum _vmon_flags_t {
	VMON_FLAG_NONE			= 0,
	VMON_FLAG_PROC_ARRAY		= 1L,			/* maintain a process array (useful if you need to do things like implement top(1) */
	VMON_FLAG_PROC_ALL		= 1L << 1,		/* monitor all the processes in the system (XXX this has some follow_children implications...)  */
	VMON_FLAG_2PASS			= 1L << 2,		/* perform all sampling/wants in a first pass, then invoke all callbacks in second in vmon_sample(), important if your callbacks are layout-sensitive (vwm) */
} vmon_flags_t;

/* store ids, used as indices into the stores array, and shift offsets for the wants mask */
typedef enum _vmon_sys_store_t {
#define vmon_want(_sym, _name, _func)	VMON_STORE_ ## _sym,
#include "defs/sys_wants.def"
	VMON_STORE_SYS_NR
} vmon_sys_store_t;

typedef enum _vmon_sys_wants_t {
	VMON_WANT_SYS_NONE		= 0,
#define vmon_want(_sym, _name, _func)	VMON_WANT_ ## _sym = 1L << VMON_STORE_ ## _sym,
#include "defs/sys_wants.def"
} vmon_sys_wants_t;

typedef enum _vmon_proc_store_t {
#define vmon_want(_sym, _name, _func)	VMON_STORE_ ## _sym,
#include "defs/proc_wants.def"
	VMON_STORE_PROC_NR
} vmon_proc_store_t;

typedef enum _vmon_proc_wants_t {
	VMON_WANT_PROC_INHERIT		= 0,
#define vmon_want(_sym, _name, _func)	VMON_WANT_ ## _sym = 1L << VMON_STORE_ ## _sym,
#include "defs/proc_wants.def"
	/* XXX: note proc_wants is internally overloaded in libvmon to communicate when a process is a thread-child of a process, so not all bits are available. */
} vmon_proc_wants_t;


/* we add some new complex types */
typedef struct _vmon_char_array_t {
	char	*array;
	int	len;
	int	alloc_len;
} vmon_char_array_t;

typedef struct _vmon_str_array_t {
	char	**array;
	int	len;
	int	alloc_len;
} vmon_str_array_t;


/* system stat stuff (/proc/stat...) */
typedef enum _vmon_sys_stat_sym_t {
#define VMON_ENUM_SYMBOLS
#include "defs/sys_stat.def"
	VMON_SYS_STAT_NR					/* append this symbol to the end so we have a count */
} vmon_sys_stat_sym_t;

typedef struct _vmon_sys_stat_t {
	int	stat_fd;

	char	changed[BITNSLOTS(VMON_SYS_STAT_NR)];		/* bitmap for indicating changed fields */

#define VMON_DECLARE_MEMBERS
#include "defs/sys_stat.def"
} vmon_sys_stat_t;


/* system vm/mem stuff (/proc/meminfo, /proc/vmstat...) */
typedef enum _vmon_sys_vm_sym_t {
#define VMON_ENUM_SYMBOLS
#include "defs/sys_vm.def"
	VMON_SYS_VM_NR						/* append this symbol to the end so we have a count */
} vmon_sys_vm_sym_t;

typedef struct _vmon_sys_vm_t {
	int	meminfo_fd;

	char	changed[BITNSLOTS(VMON_SYS_VM_NR)];		/* bitmap for indicating changed fields */

#define VMON_DECLARE_MEMBERS
#include "defs/sys_vm.def"
} vmon_sys_vm_t;


/* stat things we always monitor for a process, regardless of the caller's wants */
typedef enum _vmon_proc_stat_sym_t {
#define VMON_ENUM_SYMBOLS
#include "defs/proc_stat.def"
	VMON_PROC_STAT_NR					/* append this symbol to the end so we have a count */
} vmon_proc_stat_sym_t;

typedef struct _vmon_proc_stat_t {
	int	cmdline_fd, comm_fd, wchan_fd, stat_fd;		/* per-process stat monitoring /proc/$pid/{wchan,stat} file handles */

	char	changed[BITNSLOTS(VMON_PROC_STAT_NR)];		/* bitmap for indicating changed fields */

#define VMON_DECLARE_MEMBERS
#include "defs/proc_stat.def"
} vmon_proc_stat_t;


/* vm-related per-process things we can monitor */
typedef enum _vmon_proc_vm_sym_t {
#define VMON_ENUM_SYMBOLS
#include "defs/proc_vm.def"
	VMON_PROC_VM_NR						/* append this symbol to the end so we have a count */
} vmon_proc_vm_sym_t;

typedef struct _vmon_proc_vm_t {
	int	statm_fd;					/* per-process vm monitoring /proc/$pid/statm file handle */

	char	changed[BITNSLOTS(VMON_PROC_VM_NR)];		/* bitmap for indicating changed fields */

#define VMON_DECLARE_MEMBERS
#include "defs/proc_vm.def"
} vmon_proc_vm_t;


/* io-related per-process things we can monitor */
typedef enum _vmon_proc_io_sym_t {
#define VMON_ENUM_SYMBOLS
#include "defs/proc_io.def"
	VMON_PROC_IO_NR						/* append this symbol to the end so we have a count */
} vmon_proc_io_sym_t;

typedef struct _vmon_proc_io_t {
	int	io_fd;						/* per-process io monitoring /proc/$pid/io file handle */

	char	changed[BITNSLOTS(VMON_PROC_IO_NR)];		/* bitmap for indicating changed fields */

#define VMON_DECLARE_MEMBERS
#include "defs/proc_io.def"
} vmon_proc_io_t;


/* follow children want context */
typedef struct _vmon_proc_follow_children_t {
	int	children_fd;					/* per-process children following /proc/$pid/task/$pid/children file handle */
} vmon_proc_follow_children_t;


/* follow threads want context */
typedef struct _vmon_proc_follow_threads_t {
	DIR	*task_dir;					/* per-process threads monitoring /proc/$pid/task handle */
} vmon_proc_follow_threads_t;


/* open files monitoring */
typedef enum _vmon_proc_files_sym_t {
#define VMON_ENUM_SYMBOLS
#include "defs/proc_files.def"
} vmon_proc_files_sym_t;

typedef enum _vmon_fobject_type_t {
	VMON_FOBJECT_TYPE_UNRECOGNIZED = 0,
	/* TODO, enumerate the possible opened fobject types, this should come out of a .def */
	VMON_FOBJECT_TYPE_PIPE,
	VMON_FOBJECT_TYPE_SOCKET,
	VMON_FOBJECT_TYPE_ANON,
	VMON_FOBJECT_TYPE_NR
} vmon_fobject_type_t;

typedef struct _vmon_fobject_t {
	list_head_t		bucket;				/* fobjects hash table bucket this fobject belongs to */
	uint64_t		inum;				/* inode number */
	list_head_t		ref_fds;			/* referring fds list, this is a list of proc_fd_t's which appear to have this object open */
	int			refcnt;				/* reference count, this may be greater than the # of nodes in ref_fds, for instance the
								   monitoring of /proc/net/unix will maintain references to fobjects for every row, without
								   maintaing nodes on ref_fds.   But there are certain fobjects which simply emerge due to
								   processes having them open (pipes) which are not explicitly monitored elsewhere, in those
								   cases the refcnt and number of nodes in ref_fds shall match. */
	vmon_fobject_type_t	type;				/* type for this fobject */


		/* TODO: possibly hook into the object-type-specific information here, if so, it should come out of .defs */
		/* XXX the question is do I want to introduce that resource allocation here as part of the fobject, or do I want
		 * it elsewhere and potentially optional?  I think it makes sense to make it optional and triggered by asking for
		 * fobject detail monitoring, so instead of a union here we should have a pointer, and that pointer becomes useful
		 * when fobject-type monitors become active */
	void			*foo;				/* hook for caller's data, if needed (expected to be used together with fobject_[cd]tor_cb) */
} vmon_fobject_t;

typedef struct _vmon_proc_fd_t {
	list_head_t		fds;				/* per-process files list node */
	int			generation;			/* generation number, for convenient detection of closed files */
	struct _vmon_proc_t	*process;			/* process this fd belongs to (annoying, but enables linking processes with fobject refs) */
	list_head_t		ref_fds;			/* ref_fds list node */
	vmon_fobject_t		*object;			/* the object instance this fd refers to (this object also contains the head for the refs list) */

#define VMON_DECLARE_MEMBERS
#include "defs/proc_files.def"
} vmon_proc_fd_t;

typedef struct _vmon_proc_files_t {
	int			refcnt;				/* reference count for dealing with sharing of open files (like threads...) */
	DIR			*fd_dir;			/* per-process files /proc/$pid/fd handle */
	list_head_t		fds;				/* per-process files linked list head */
} vmon_proc_files_t;


/* TODO per-process VM stats monitoring */

struct _vmon_t;


/* list of callbacks is maintained for the per-process callbacks, it's convenient to do things like update multiple dynamic contexts associated with a given process monitor (think multiple windows) */
typedef struct _vmon_proc_callback_t {
	void			(*func)(struct _vmon_t *, struct _vmon_proc_t *, void *);
	void			*arg;
	list_head_t		callbacks;
} vmon_proc_callback_t;

typedef struct _vmon_proc_t {
	list_head_t		bucket;				/* processes hash table bucket this process belongs to */

	list_head_t		children;			/* head of the children of this process, empty when no children */
	list_head_t		siblings;			/* node in siblings list */
	list_head_t		threads;			/* head or node for the threads list, empty when process has no threads */

	struct _vmon_proc_t	*parent;			/* reference to the parent */

	int			array_hint_pos;			/* hint for the process's position in the array (when array maintenance has been requested) */

	int			pid;				/* the PID of the process being monitored */

	int			generation;			/* generation number, for convenient detection of exited processes */
	int			refcnt;				/* reference count on this node */
	vmon_proc_wants_t	wants;				/* wants @ this node */

	vmon_proc_wants_t	activity;			/* bits updated when there's activity on the respective wants (stores have changes) */
	void			*stores[VMON_STORE_PROC_NR];	/* pointers to the per-want per-monitored-process storage space */

								/* callbacks invoked after sampling wants at and below this node */
	list_head_t		sample_callbacks;		/* list of callbacks to invoke sample_cb on behalf of (and supply as parameteres to) */
	void			*foo;				/* another per-process hook for whatever per-process uses the caller may have, but not managed by the api */

	int			children_changed:1;		/* gets set when any of my immediate children have had is_new or is_stale set in the last sample */
	int			threads_changed:1;		/* gets set when any of my threads have had is_new or is_stale set in the last sample */
	int			is_new:1;			/* process is new in the most recent sample, automatically cleared on subsequent sample */
	int			is_stale:1;			/* process became stale in the most recent sample, automatically cleared on subsequent sample (process will be discarded) */
	int			is_thread:1;			/* process is a thread belonging to parent */
} vmon_proc_t;


typedef struct _vmon_t {
	DIR			*proc_dir;			/* /proc is opened @ vmon_init() */

								/* TODO: rename this to something more contextually processes-specific */
	/* these array members are only relevant when array maintenance has been requested via VMON_FLAG_PROC_ARRAY @ vmon_init() */
	vmon_proc_t		**array;			/* array of processes being monitored (flat) */
	int			array_allocated_nr;		/* number of entries in the table */
	int			array_active_nr;		/* number of processes present in the table (including stale) */
	int			array_hint_free;		/* hint for a free element in the list */

	list_head_t		htab[VMON_HTAB_SIZE];		/* hash table for quickly finding processes being monitored */
	list_head_t		processes;			/* top of the processes heirarchy */
	list_head_t		orphans;			/* ephemeral list of processes orphaned this sample, orphans wind up becoming top-level processes */
	int			processes_changed:1;		/* flag set when the toplevel processes list changes */

	list_head_t		fobjects;			/* XXX TODO: temporary single fobjects table, in the future this will be an array of type-indexed
								 * fobject hash tables */
	int			fobjects_nr;			/* XXX TODO: temporary simple counter of number of fobjects, this will change into something
								 * more like we have for the proceses array */
	vmon_flags_t		flags;				/* instance flags */
	vmon_sys_wants_t	sys_wants;			/* system-wide wants mask */
	vmon_proc_wants_t	proc_wants;			/* inherited per-process wants mask */

								/* function tables for mapping of wants bits to functions (sys-wide and per-process) */
	int			(*sys_funcs[VMON_STORE_SYS_NR])(struct _vmon_t *, void **);
	int			(*proc_funcs[VMON_STORE_PROC_NR])(struct _vmon_t *, vmon_proc_t *, void **);

	vmon_sys_wants_t	activity;			/* bits updated when there's activity on the respective wants (stores have changes) */
	void			*stores[VMON_STORE_SYS_NR];	/* stores for the sys-wide wants */
	void			(*sample_cb)(struct _vmon_t *);	/* callback invoked after executing the selected sys wants (once per vmon_sample() call)) */

	char			buf[8192];			/* scratch buffer for private use XXX: it may make sense to dynamically size these... */
	char			buf_bis[8192];			/* secondary scratch buffer for private use */
	int			generation;			/* generation counter for whatever might need it, increments with vmon_sample() calls */

								/* callbacks we'll invoke in response to processes becoming instantiated and destroyed, when set */
	void			(*proc_ctor_cb)(struct _vmon_t *, vmon_proc_t *);
	void			(*proc_dtor_cb)(struct _vmon_t *, vmon_proc_t *);

								/* callbacks we'll invoke in response to fobjects becoming instantiated and destroyed, when set */
	void			(*fobject_ctor_cb)(struct _vmon_t *, vmon_fobject_t *);
	void			(*fobject_dtor_cb)(struct _vmon_t *, vmon_fobject_t *);
} vmon_t;


int vmon_init(vmon_t *, vmon_flags_t, vmon_sys_wants_t, vmon_proc_wants_t);
void vmon_destroy(vmon_t *);
vmon_proc_t * vmon_proc_monitor(vmon_t *, vmon_proc_t *, int, vmon_proc_wants_t, void (*)(vmon_t *, vmon_proc_t *, void *), void *);
void vmon_proc_unmonitor(vmon_t *, vmon_proc_t *, void (*)(vmon_t *, vmon_proc_t *, void *), void *);
int vmon_sample(vmon_t *);

#endif
