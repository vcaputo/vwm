/*
 *  libvmon - a lightweight linux system/process monitoring library,
 *  intended for linking directly into gpl programs.
 *
 *  This has been written specifically for use in vwm, my window manager,
 *  but includes some facilities for other uses.
 *
 *  Copyright (c) 2012-2017  Vito Caputo - <vcaputo@pengaru.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <dirent.h>
#include <errno.h>
#include "list.h"

#include "vmon.h"

#define VMON_INTERNAL_PROC_IS_THREAD	(1L << 31)	/* used to communicate to vmon_proc_monitor() that the pid is a tid */

/* valid return values for the sampler functions, these are private to the library */
typedef enum _sample_ret_t {
	SAMPLE_CHANGED,		/* this sampler invocation has changes */
	SAMPLE_UNCHANGED,	/* this sampler invocation has no changes */
	SAMPLE_ERROR,		/* this sampler invocation encountered errors (XXX it may be interesting to support indicating changing & errors) */
	DTOR_FREE,		/* this dtor(sampler) invocation wants the caller to free the store */
	DTOR_NOFREE		/* this dtor(sampler) invocation does not want the caller to perform a free on the store */
} sample_ret_t;


/* some private convenience functions */
static void try_free(void **ptr)
{
	assert(ptr);

	if ((*ptr)) {
		free((*ptr));
		(*ptr) = NULL;
	}
}


static void try_close(int *fd)
{
	assert(fd);

	if ((*fd) != -1) {
		close((*fd));
		(*fd) = -1;
	}
}


static void try_closedir(DIR **dir)
{
	assert(dir);

	if ((*dir)) {
		closedir((*dir));
		(*dir) = NULL;
	}
}


static ssize_t try_pread(int fd, void *buf, size_t count, off_t offset)
{
	assert(buf);

	if (fd != -1) {
		return pread(fd, buf, count, offset);
	} else {
		errno = EBADF;
		return fd;
	}
}


/* this does a copy from src to dest
 * sets the bit specified by changed_pos in the bitmap *changed if what was copied to src was different from what was already there */
static void memcmpcpy(void *dest, const void *src, size_t n, char *changed, unsigned changed_pos)
{
	size_t	i = 0;

	assert(dest);
	assert(src);
	assert(changed);

	/* if the changed bit is set on entry we don't execute the comparison-copy */
	if (!BITTEST(changed, changed_pos)) {
		/* a simple slow compare and copy loop, break out if we've found a change */
		/* XXX TODO: an obvious optimization would be to compare and copy words at a time... */
		for (; i < n; i++) {
			if (((char *)dest)[i] != ((char *)src)[i]) {
				BITSET(changed, changed_pos);
				break;
			}

			((char *)dest)[i] = ((char *)src)[i];
		}
	}

	/* memcpy whatever remains */
	if (n - i)
		memcpy(&((char *)dest)[i], &((char *)src)[i], n - i);
}


typedef enum _vmon_load_flags_t {
	LOAD_FLAGS_NONE		= 0,
	LOAD_FLAGS_NOTRUNCATE	= 1L
} vmon_load_flags_t;

/* we enlarge *alloc if necessary, but never shrink it.  changed[changed_pos] bit is set if a difference is detected, supply LOAD_FLAGS_NOTRUNCATE if we don't want empty contents to truncate last-known data */
static int load_contents_fd(vmon_t *vmon, vmon_char_array_t *array, int fd, vmon_load_flags_t flags, char *changed, unsigned changed_pos)
{
	size_t	total = 0;
	ssize_t	len;

	assert(vmon);
	assert(array);
	assert(changed);

	if (fd < 0) /* no use attempting the pread() on -1 fds */
		return 0;

	/* FIXME: this is used to read proc files, you can't actually read proc files iteratively
	 * without race conditions; it needs to be all done as a single read.
	 */
	while ((len = pread(fd, vmon->buf, sizeof(vmon->buf), total)) > 0) {
		size_t	newsize = total + len;

		if (newsize > array->alloc_len) {
			char	*tmp;

			tmp = realloc(array->array, newsize);
			if (!tmp)
				return -ENOMEM;

			array->array = tmp;
			array->alloc_len = newsize;
		}

		memcmpcpy(&array->array[total], vmon->buf, len, changed, changed_pos);

		total += len;
	}

	/* if we read something or didn't encounter an error, store the new total */
	if (total || (len == 0 && !(flags & LOAD_FLAGS_NOTRUNCATE))) {
		/* if the new length differs ensure the changed bit is set */
		if (array->len != total)
			BITSET(changed, changed_pos);

		array->len = total;
	}

	return 0;
}


/* convenience function for opening a path using a format string, path is temporarily assembled in vmon->buf */
static int openf(vmon_t *vmon, int flags, DIR *dir, char *fmt, ...)
{
	int	fd;
	va_list	va_arg;
	char	buf[4096];

	assert(vmon);
	assert(dir);
	assert(fmt);

	va_start(va_arg, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va_arg); /* XXX accepting the possibility of truncating the path */
	va_end(va_arg);

	fd = openat(dirfd(dir), buf, flags);

	return fd;
}


/* convenience function for opening a directory using a format string, path is temporarily assembled in vmon->buf */
static DIR * opendirf(vmon_t *vmon, DIR *dir, char *fmt, ...)
{
	int	fd;
	va_list	va_arg;
	char	buf[4096];

	assert(vmon);
	assert(dir);
	assert(fmt);

	va_start(va_arg, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va_arg); /* XXX accepting the possibility of truncating the path */
	va_end(va_arg);

	fd = openat(dirfd(dir), buf, O_RDONLY);
	if (fd == -1)
		return NULL;

	return fdopendir(fd);
}


/* enlarge an array by the specified amount */
static int grow_array(vmon_char_array_t *array, size_t amount)
{
	char	*tmp;

	assert(array);

	tmp = realloc(array->array, array->alloc_len + amount);
	if (!tmp)
		return -ENOMEM;

	array->alloc_len += amount;
	array->array = tmp;

	return 0;
}


/* load the contents of a symlink, dir is not optional */
#define READLINKF_GROWINIT	10
#define READLINKF_GROWBY	2
static int readlinkf(vmon_t *vmon, vmon_char_array_t *array, DIR *dir, char *fmt, ...)
{
	char	buf[4096];
	va_list	va_arg;
	ssize_t	len;

	assert(vmon);
	assert(array);
	assert(dir);
	assert(fmt);

	va_start(va_arg, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va_arg);
	va_end(va_arg);

	if (!array->array && grow_array(array, READLINKF_GROWINIT) < 0)
		return -ENOMEM;

	do {
		len = readlinkat(dirfd(dir), buf, array->array, (array->alloc_len - 1));
	} while (len != -1 && len == (array->alloc_len - 1) && (len = grow_array(array, READLINKF_GROWBY)) >= 0);

	if (len < 0)
		return -errno;

	array->len = len;
	array->array[array->len] = '\0';

	return 0;
}


/* here starts private per-process samplers and other things like following children implementation etc. */

/* simple helper for installing callbacks on the callback lists, currently only used for the per-process sample callbacks */
/* will not install the same callback function & arg combination more than once, and will not install NULL-functioned callbacks at all! */
static int maybe_install_proc_callback(vmon_t *vmon, list_head_t *callbacks, void (*func)(vmon_t *, void *, vmon_proc_t *, void *), void *arg)
{
	assert(vmon);
	assert(callbacks);
	assert(!arg || func);

	if (func) {
		vmon_proc_callback_t	*cb;

		list_for_each_entry(cb, callbacks, callbacks) {
			if (cb->func == func && cb->arg == arg)
				break;
		}

		if (&cb->callbacks == callbacks) {
			cb = calloc(1, sizeof(vmon_proc_callback_t));
			if (!cb)
				return 0;

			cb->func = func;
			cb->arg = arg;
			list_add_tail(&cb->callbacks, callbacks);
		}
	}

	return 1;
}


/* helper for searching for the process in the process array, specify NULL to find a free slot */
static int find_proc_in_array(vmon_t *vmon, vmon_proc_t *proc, int hint)
{
	int	ret = -1;

	assert(vmon);

	if (hint >= 0 && hint < vmon->array_allocated_nr && vmon->array[hint] == proc) {
		/* the hint was accurate, bypass the search */
		ret = hint;
	} else {
		int	i;

		/* search for the entry in the array */
		for (i = 0; i < vmon->array_allocated_nr; i++) {
			if (vmon->array[i] == proc) {
				ret = i;
				break;
			}
		}
	}

	if (!proc && ret == -1) {
		vmon_proc_t	**tmp;

		/* enlarge the array */
		tmp = realloc(vmon->array, (vmon->array_allocated_nr + VMON_ARRAY_GROWBY) * sizeof(vmon_proc_t *));
		if (tmp) {
			memset(&tmp[vmon->array_allocated_nr], 0, VMON_ARRAY_GROWBY * sizeof(vmon_proc_t *));
			ret = vmon->array_hint_free = vmon->array_allocated_nr;
			vmon->array_allocated_nr += VMON_ARRAY_GROWBY;
			vmon->array = tmp;
		} /* XXX TODO: handle realloc failure */
	}

	return ret;
}


/* this is the private variant that allows providing a parent, which libvmon needs for constructing hierarchies, but callers shouldn't be doing themselves */
static vmon_proc_t * proc_monitor(vmon_t *vmon, vmon_proc_t *parent, int pid, vmon_proc_wants_t wants, void (*sample_cb)(vmon_t *, void *, vmon_proc_t *, void *), void *sample_cb_arg)
{
	vmon_proc_t	*proc;
	int		hash = (pid % VMON_HTAB_SIZE), i;
	int		is_thread = (wants & VMON_INTERNAL_PROC_IS_THREAD) ? 1 : 0;

	assert(vmon);
	assert(!sample_cb_arg || sample_cb);

	wants &= ~VMON_INTERNAL_PROC_IS_THREAD;

	if (pid < 0)
		return NULL;

	list_for_each_entry(proc, &vmon->htab[hash], bucket) {
		/* search for the process to see if it's already monitored, we allow threads to exist with the same pid hence the additional is_thread comparison */
		if (proc->pid == pid && proc->is_thread == is_thread) {
			if (!maybe_install_proc_callback(vmon, &proc->sample_callbacks, sample_cb, sample_cb_arg))
				return NULL;

			proc->wants = wants; /* we can alter wants this way, though it clearly needs more consideration XXX */

			if (parent) {
				/* This is a predicament.  If the top-level (externally-established) process monitor wins the race, the process has no parent and is on the vmon->processes list.
				 * Then the follow_children-established monitor comes along and wants to assign a parent, this isn't such a big deal, and we should be able to permit it, however,
				 * we're in the process of iterating the top-level processes list when we run the follow_children() sampler of a descendant which happens to also be the parent.
				 * We can't simply remove the process from the top-level processes list mid-iteration and stick it on the children list of the parent, the iterator could wind up
				 * following the new pointer into the parents children.
				 *
				 * What I'm doing for now is allowing the assignment of a parent when there is no parent, but we don't perform the vmon->processes to parent->children migration
				 * until the top-level sample_siblings() function sees the node with the parent.  At that point, the node will migrate to the parent's children list.  Since
				 * this particular migration happens immediately within the same sample, it
				 */
				if (!proc->parent) {
					/* if a parent was supplied, and there is no current parent, the process is top-level currently by external callers, but now appears to be a child of something as well,
					 * so in this scenario, we'll remove it from the top-level siblings list, and make it a child of the new parent.  I don't think there needs to be concern about its being a thread here. */
					/* Note we can't simply move the process from its current processes list and add it to the supplied parent's children list, as that would break the iterator above us at the top-level, so
					 * we must defer the migration until the processes iterator context can do it for us - but this is tricky because we're in the middle of traversing our hierarchy and this process may
					 * be in a critical is_new state which must be realized this sample at its location in the hierarchy for correctness, there will be no reappearance of that critical state in the correct
					 * tree position for users like vwm. */
					/* the VMON_FLAG_2PASS flag has been introduced for users like vwm */
					proc->parent = parent;
					proc->refcnt++;
				}
#if 0
				else if (parent != proc->parent) {
					/* We're switching parents; this used to be considered unexpected, but then vmon happened and it monitors the whole tree from PID1 down.
					 * PID1 is special in that it inherits orphans, so we can already be monitoring a child of an exited parent, and here PID1's children
					 * following is looking up a newly inherited orphan, which reaches here finding proc with a non-NULL, but different parent.
					 * Note the introduction of PR_SET_CHILD_SUBREAPER has made this no longer limited to PID1 either.
					 */
					/* now, we can't simply switch parents in a single step... since the is_stale=1 state must be seen by the front-end before we can
					 * unlink it from the structure in a subsequent sample.  So instead, we should be queueing an adoption by the new parent, but
					 * for the time being we can just suppress the refcnt bump and let the adoptive parent reestablish the monitor anew after
					 * runs its course under its current parent.
					 */
				}
#endif
			} else {
				proc->refcnt++;
			}

			return proc;
		}
	}

	proc = (vmon_proc_t *)calloc(1, sizeof(vmon_proc_t));
	if (proc == NULL)
		return NULL; /* TODO: report an error */

	proc->pid = pid;
	proc->wants = wants;
	proc->generation = vmon->generation;
	proc->refcnt = 1;
	proc->is_new = 1; /* newly created process */
	proc->is_thread = is_thread;
	proc->parent = parent;
	INIT_LIST_HEAD(&proc->sample_callbacks);
	INIT_LIST_HEAD(&proc->children);
	INIT_LIST_HEAD(&proc->siblings);
	INIT_LIST_HEAD(&proc->threads);

	if (!maybe_install_proc_callback(vmon, &proc->sample_callbacks, sample_cb, sample_cb_arg)) {
		free(proc);
		return NULL;
	}

	if (parent) {
		/* if a parent is specified, attach this process to the parent's children or threads with its siblings */
		if (is_thread) {
			list_add_tail(&proc->threads, &parent->threads);
			parent->threads_changed = 1;
			parent->is_threaded = 1;
		} else {
			list_add_tail(&proc->siblings, &parent->children);
			parent->children_changed = 1;
		}
	} else {
		/* if no parent is specified, this is a toplevel process, attach it to the vmon->processes list with its siblings */
		/* XXX ignoring is_thread if no parent is specified, it shouldn't occur, unless I want to support external callers monitoring specific threads explicitly, maybe...  */
		list_add_tail(&proc->siblings, &vmon->processes);
		vmon->processes_changed = 1;
	}

	/* add this process to the hash table */
	list_add_tail(&proc->bucket, &vmon->htab[hash]);

	/* if process table maintenance is enabled acquire a free slot for this process */
	if ((vmon->flags & VMON_FLAG_PROC_ARRAY) && (i = find_proc_in_array(vmon, NULL, vmon->array_hint_free)) != -1) {
		vmon->array[i] = proc;

		/* cache where we get inserted into the array */
		proc->array_hint_pos = i;
	}

	/* invoke ctor callback if set, note it's only called when a new vmon_proc_t has been instantiated */
	if (vmon->proc_ctor_cb)
		vmon->proc_ctor_cb(vmon, proc);

	return proc;
}


/* implements the children following */
static int proc_follow_children(vmon_t *vmon, vmon_proc_t *proc, vmon_proc_follow_children_t **store)
{
	int		changes = 0;
	int		len, total = 0, i, child_pid = 0, found;
	vmon_proc_t	*tmp, *_tmp;
	list_head_t	*cur, *start;

	assert(vmon);
	assert(store);

	if (!proc) { /* dtor */
		try_close(&(*store)->children_fd);

		return DTOR_FREE;
	}

	if (proc->is_thread) { /* don't follow children of threads */
		assert(!(*store));

		return SAMPLE_UNCHANGED;
	}

	if (!(*store)) { /* implicit ctor on first sample */
		*store = calloc(1, sizeof(vmon_proc_follow_children_t));

		(*store)->children_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/task/%i/children", proc->pid, proc->pid);
	}

	/* unmonitor stale children on entry, this concludes the two-phase removal of a process */
	list_for_each_entry_safe(tmp, _tmp, &proc->children, siblings) {
		if (tmp->is_stale)
			vmon_proc_unmonitor(vmon, tmp, NULL, NULL);
	}

	/* if we have a parent, and our parent has become stale, ensure this, the child, becomes stale as well */
	if (proc->parent && proc->parent->is_stale) {
		proc->is_stale = 1;
		proc->parent->children_changed = 1;
		return SAMPLE_CHANGED;
	}

	/* maintain our awareness of children, if we detect a new child initiate monitoring for it, existing children get their generation number updated */
	start = &proc->children;
	while ((len = try_pread((*store)->children_fd, vmon->buf, sizeof(vmon->buf), total)) > 0) {
		total += len;

		for (i = 0; i < len; i++) {
			switch (vmon->buf[i]) {
				case '0' ... '9':
					/* PID component, accumulate it */
					child_pid *= 10;
					child_pid += (vmon->buf[i] - '0');
					break;

				case ' ':
					/* separator, terminates a PID, search for it in the childrens list */
					found = 0;
					list_for_each(cur, start) {
						if (cur == &proc->children) /* take care to skip the head node */
							continue;

						if (list_entry(cur, vmon_proc_t, siblings)->pid == child_pid) {
							/* found the child already monitored, update its generation number and stop searching */
							tmp = list_entry(cur, vmon_proc_t, siblings);
							tmp->generation = vmon->generation;
							found = 1;
							tmp->is_new = 0;
							break;
						}
					}

					if (found || (tmp = proc_monitor(vmon, proc, child_pid, proc->wants, NULL, NULL))) {
						/* There's an edge case where vmon_proc_monitor() finds child_pid existing as a child of something else,
						 * in that case we're effectively migrating it to a new parent.  This occurs in the vmon use case where
						 * it's monitoring PID1-down, and PID1 of course inherits orphans.  So some descendant proc is already
						 * being monitored with its children monitored too, but that proc has exited, orphaning its children.
						 * The kernel has since moved the orphaned children up to be children of PID1, and we could be performing
						 * children following for PID1 here, discovering those newly inherited orphans whose exited parent hasn't
						 * even been flagged as is_stale yet in libvmon, let alone been unreffed/removed from the htab.
						 *
						 * In such a scenario, we need to _not_ use its siblings node as a search start, because we'll be stepping
						 * into the other parent's children list, which would be Very Broken.  What we instead do, is basically
						 * nothing, so it can be handled in a future sample, after the exited parent can go through its is_stale=1
						 * cycle and unlink itself from the orphaned descendants.  There are more complicated ways to handle this
						 * which would technically be more accurate, but let's just do the simple and correct thing for now.
						 */
						if (tmp->parent == proc)
							start = &tmp->siblings;
					} /* else { proc_monitor failed just move on } */

					child_pid = 0;
					break;

				default:
					assert(0);
			}
		}
	}

	/* look for children which seem to no longer exist (found by stale generation numbers) and queue them for unmonitoring, flag this as a children change too */
	found = 0;
	list_for_each_entry(tmp, &proc->children, siblings) {
		/* set children not found to stale status so the caller can respond and on our next sample invocation we will unmonitor them */
		if (tmp->generation != vmon->generation)
			found = tmp->is_stale = 1;
	}

	/* XXX TODO: does it makes sense for shit to happen here? */
	if (found) {
		if (proc->parent) {
			proc->parent->children_changed = 1;
		} else {
			vmon->processes_changed = 1;
		}
	}

	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;
}


/* implements the thread following */
static int proc_follow_threads(vmon_t *vmon, vmon_proc_t *proc, vmon_proc_follow_threads_t **store)
{
	int		changes = 0;
	struct dirent	*dentry;
	list_head_t	*cur, *start;
	vmon_proc_t	*tmp, *_tmp;
	int		found;

	assert(vmon);
	assert(store);

	if (!proc) { /* dtor */
		try_closedir(&(*store)->task_dir);

		return DTOR_FREE;
	}

	if (proc->is_thread) /* bypass following the threads of threads */
		return SAMPLE_UNCHANGED;

	if (!proc->stores[VMON_STORE_PROC_STAT] ||
	    (((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->num_threads <= 1 && list_empty(&proc->threads)))
		/* bypass following of threads if we either can't determine the number from the proc stat sample or if the sample says there's 1 or less (and an empty threads list, handling stale exited threads) */
		/* XXX I'm not sure if this is always the right thing to do, there may be some situations where one could play games with clone() directly
		 * and escape the monitoring library with a lone thread having had the main thread exit, leaving the count at 1 while having a process
		 * descriptor distinguished from the single thread XXX */
		return SAMPLE_UNCHANGED;

	if (!(*store)) { /* implicit ctor on first sample */
		*store = calloc(1, sizeof(vmon_proc_follow_threads_t));

		(*store)->task_dir = opendirf(vmon, vmon->proc_dir, "%i/task", proc->pid);
	} else if ((*store)->task_dir) {
		seekdir((*store)->task_dir, 0);
	}

	if (!(*store)->task_dir)
		return SAMPLE_ERROR;

	/* unmonitor stale threads on entry, this concludes the two-phase removal of a thread (just like follow_children) */
	list_for_each_entry_safe(tmp, _tmp, &proc->threads, threads) {
		if (tmp->is_stale)
			vmon_proc_unmonitor(vmon, tmp, NULL, NULL);
	}

	/* If proc is stale, assume all the threads are stale as well.  In vwm/charts.c we assume all descendants of a stale node
	 * are implicitly stale, so let's ensure that's a consistent assumumption WRT libvmon's maintenance of the hierarchy.
	 * The readdir below seems like it would't find any threads of a stale process, but maybe there's some potential for a race there,
	 * particularly since we reuse an open reference on the task_dir.
	 * This reflects a similar implicit is_stale propagation in follow_children.
	 */
	if (proc->is_stale) {
		list_for_each_entry(tmp, &proc->threads, threads)
			tmp->is_stale = 1;

		/* FIXME: the changes count seems to be unused here, so this currently always returns SAMPLE_UNCHANGED, and
		 * it's unclear to me if that was intentional or just never finished.
		 */
		return SAMPLE_UNCHANGED;
	}

	start = &proc->threads;
	while ((dentry = readdir((*store)->task_dir))) {
		int	tid;

		if (dentry->d_name[0] == '.' && (dentry->d_name[1] == '\0' || (dentry->d_name[1] == '.' && dentry->d_name[2] == '\0')))
			continue; /* skip . and .. */

		tid = atoi(dentry->d_name);

		found = 0;
		list_for_each(cur, start) {
			if (cur == &proc->threads) /* take care to skip the head node */
				continue;

			if (list_entry(cur, vmon_proc_t, threads)->pid == tid) {
				/* found the thread already monitored, update its generation number and stop searching */
				tmp = list_entry(cur, vmon_proc_t, threads);
				tmp->generation = vmon->generation;
				found = 1;
				tmp->is_new = 0;
				break;
			}
		}

		if (found || (tmp = proc_monitor(vmon, proc, tid, (proc->wants | VMON_INTERNAL_PROC_IS_THREAD), NULL, NULL)))
			start = &tmp->threads;
	}

	list_for_each_entry_safe(tmp, _tmp, &proc->threads, threads) {
		/* set children not found to stale status so the caller can respond and on our next sample invocation we will unmonitor them */
		if (tmp->generation != vmon->generation)
			tmp->is_stale = 1;
	}

	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;
}


/* sample stat process stats */
typedef enum _vmon_proc_stat_fsm_t {
#define VMON_ENUM_PARSER_STATES
#include "defs/proc_stat.def"
} vmon_proc_stat_fsm_t;

static sample_ret_t proc_sample_stat(vmon_t *vmon, vmon_proc_t *proc, vmon_proc_stat_t **store)
{
	int			changes = 0;
	int			i, len, total = 0;
	char			*arg;
	int			argn, prev_argc;
	vmon_proc_stat_fsm_t	state = VMON_PARSER_STATE_PROC_STAT_PID;
#define VMON_PREPARE_PARSER
#include "defs/proc_stat.def"

	assert(vmon);
	assert(store);

	if (!proc) { /* dtor */
		try_close(&(*store)->comm_fd);
		try_free((void **)&(*store)->comm.array);
		try_close(&(*store)->cmdline_fd);
		try_free((void **)&(*store)->cmdline.array);
		try_free((void **)&(*store)->argv);
		try_close(&(*store)->wchan_fd);
		try_free((void **)&(*store)->wchan.array);
		try_close(&(*store)->stat_fd);
		try_free((void **)&(*store)->exe.array);

		return DTOR_FREE;
	}

/* _retry: */
	if (!(*store)) { /* ctor */

		*store = calloc(1, sizeof(vmon_proc_stat_t));

		if (proc->is_thread) {
			(*store)->comm_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/task/%i/comm", proc->pid, proc->pid);
			(*store)->cmdline_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/task/%i/cmdline", proc->pid, proc->pid);
			(*store)->wchan_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/task/%i/wchan", proc->pid, proc->pid);
			(*store)->stat_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/task/%i/stat", proc->pid, proc->pid);
		} else {
			(*store)->comm_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/comm", proc->pid);
			(*store)->cmdline_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/cmdline", proc->pid);
			(*store)->wchan_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/wchan", proc->pid);
			(*store)->stat_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/stat", proc->pid);
		}

		/* initially everything is considered changed */
		memset((*store)->changed, 0xff, sizeof((*store)->changed));
	} else {
		/* clear the entire changed bitmap */
		memset((*store)->changed, 0, sizeof((*store)->changed));
	}

	/* XXX TODO: integrate load_contents_fd() calls into changes++ maintenance */
	/* /proc/$pid/comm */
	load_contents_fd(vmon, &(*store)->comm, (*store)->comm_fd, LOAD_FLAGS_NOTRUNCATE, (*store)->changed, VMON_PROC_STAT_COMM);

	/* /proc/$pid/cmdline */
	load_contents_fd(vmon, &(*store)->cmdline, (*store)->cmdline_fd, LOAD_FLAGS_NOTRUNCATE, (*store)->changed, VMON_PROC_STAT_CMDLINE);
	for (prev_argc = (*store)->argc, (*store)->argc = 0, i = 0; i < (*store)->cmdline.len; i++) {
		if (!(*store)->cmdline.array[i])
			(*store)->argc++;
	}

	/* if the cmdline has changed, allocate argv array and store ptrs to the fields within it */
	if (BITTEST((*store)->changed, VMON_PROC_STAT_CMDLINE)) {
		if (prev_argc != (*store)->argc) {
			try_free((void **)&(*store)->argv); /* XXX could realloc */
			(*store)->argv = calloc(1, (*store)->argc * sizeof(char *));
		}

		for (argn = 0, arg = (*store)->cmdline.array, i = 0; i < (*store)->cmdline.len; i++) {
			if (!(*store)->cmdline.array[i]) {
				(*store)->argv[argn++] = arg;
				arg = &(*store)->cmdline.array[i + 1];
			}
		}
	}

	/* /proc/$pid/wchan */
	load_contents_fd(vmon, &(*store)->wchan, (*store)->wchan_fd, LOAD_FLAGS_NOTRUNCATE, (*store)->changed, VMON_PROC_STAT_WCHAN);

	/* /proc/$pid/exe */
	if ((*store)->cmdline.len) /* kernel threads have no cmdline, and always fail readlinkf() on exe, skip readlinking the exe for them using this heuristic */
		readlinkf(vmon, &(*store)->exe, vmon->proc_dir, "%i/exe", proc->pid);

	/* XXX TODO: there's a race between discovering comm_len from /proc/$pid/comm and applying it in the parsing of /proc/$pid/stat, detect the race
	 * scenario and retry the sample when detected by goto _retry (see commented _retry label above) */

	/* read in stat and parse it assigning the stat members accordingly */
	while ((len = try_pread((*store)->stat_fd, vmon->buf, sizeof(vmon->buf), total)) > 0) {
		total += len;

		for (i = 0; i < len; i++) {
			/* parse the fields from the file, stepping through... */
			_p.input = vmon->buf[i];
			switch (state) {
#define VMON_PARSER_DELIM ' '	/* TODO XXX eliminate the need for this, I want the .def's to include all the data format knowledge */
#define VMON_IMPLEMENT_PARSER
#include "defs/proc_stat.def"
				default:
					/* we're finished parsing once we've fallen off the end of the symbols */
					goto _out; /* this saves us the EOF read syscall */
			}
		}
	}

_out:
	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;
}




/* helper for maintaining reference counted global objects table */
static vmon_fobject_t * fobject_lookup_hinted(vmon_t *vmon, const char *path, vmon_fobject_t *hint)
{
	vmon_fobject_t	*fobject = NULL, *tmp = NULL;
	uint64_t	inum;

	assert(vmon);

	/* TODO maintain the fobject hash tables, they should probably be isolated/scoped based on the string before the : */
	/* in reality there needs to be a list somewhere of the  valid prefixes we wish to support, and that list should associate
	 * the prefixes with the kind of contextual information details we monitor for each of those types.  Then when new
	 * fobject types are added to libvmon, the rest of the code automatically reflects the additions due to the mechanization.
	 */
	if (!path || strncmp(path, "pipe:", 5))
		return NULL; /* XXX TODO: for now we're only dealing with pipes */

	sscanf(&path[6], "%" SCNu64 "]", &inum);

	if (hint && hint->inum == inum)
		return hint; /* the hint matches, skip the search */

	/* search for the inode, if we can't find it, allocate a new fobject XXX this needs optimizing but silly until we have all object types handled... */
	list_for_each_entry(tmp, &vmon->fobjects, bucket) {
		if (tmp->inum == inum) {
			fobject = tmp;
			break;
		}
	}

	if (!fobject) {
		/* create a new fobject */
		fobject = calloc(1, sizeof(vmon_fobject_t));

		fobject->type = VMON_FOBJECT_TYPE_PIPE;
		fobject->inum = inum;
		INIT_LIST_HEAD(&fobject->ref_fds);
		INIT_LIST_HEAD(&fobject->bucket);
		list_add_tail(&fobject->bucket, &vmon->fobjects);
		vmon->fobjects_nr++;

		if (vmon->fobject_ctor_cb)
			vmon->fobject_ctor_cb(vmon, fobject);
	}

	return fobject;
}


/* add a reference to an fobject, fd_ref is optional (should be present if the reference is due to an open fd, which may not be the case) */
static void fobject_ref(vmon_t *vmon, vmon_fobject_t *fobject, vmon_proc_fd_t *fd_ref)
{
	assert(vmon);
	assert(fobject);

	fobject->refcnt++;

	if (fd_ref)
		list_add_tail(&fd_ref->ref_fds, &fobject->ref_fds);
}


/* fobject is required and is the object being unrefereced,  fd_ref is optional but represents the per-process fd reference being used to access this fobject via */
static int fobject_unref(vmon_t *vmon, vmon_fobject_t *fobject, vmon_proc_fd_t *fd_ref)
{
	assert(vmon);
	assert(fobject);

	if (fd_ref)
		list_del(&fd_ref->ref_fds);

	fobject->refcnt--;

	if (!fobject->refcnt) {
		/* after the refcnt drops to zero we discard the fobject */
		list_del(&fobject->bucket);
		vmon->fobjects_nr--;

		if (vmon->fobject_dtor_cb)
			vmon->fobject_dtor_cb(vmon, fobject);

		free(fobject);
		return 1;
	}

	return 0;
}


/* helper for deleting an fd from the per-process fds list list */
static void del_fd(vmon_t *vmon, vmon_proc_fd_t *fd)
{
	assert(vmon);
	assert(fd);

	list_del(&fd->fds);
	if (fd->object)
		fobject_unref(vmon, fd->object, fd); /* note we supply both the fobject ptr and proc_fd ptr (fd) */
	try_free((void **)&fd->object_path.array);
	free(fd);
}


/* implements the open file descriptors (/proc/$pid/fd/...) sampling */
static sample_ret_t proc_sample_files(vmon_t *vmon, vmon_proc_t *proc, vmon_proc_files_t **store)
{
	int		changes = 0;
	struct dirent	*dentry;
	int		fdnum;
	list_head_t	*cur, *start;
	vmon_proc_fd_t	*fd, *_fd;
	vmon_fobject_t	*cur_object = NULL;

	assert(vmon);
	assert(store);

	if (!proc) {
		/* dtor implementation */
		if (--((*store)->refcnt)) /* suppress dtor while references exist, the store is shared */
			return DTOR_NOFREE;

		try_closedir(&(*store)->fd_dir);

		list_for_each_entry_safe(fd, _fd, &(*store)->fds, fds)
			del_fd(vmon, fd);

		return DTOR_FREE;
	}

	if (proc->is_thread) {
		/* don't monitor the open files of threads, instead, share the files store of the parent process by adding a reference to it */
		/* XXX TODO: use kcmp CMP_FILES in the future instead of assuming all threads share open files */
		if (!(*store) && ((*store) = proc->parent->stores[VMON_STORE_PROC_FILES]))
			(*store)->refcnt++;

		return SAMPLE_UNCHANGED;	/* XXX TODO: we actually want to reflect/copy the activity state of the parent */
	}

	if (!(*store)) { /* implicit ctor on first sample */
		*store = calloc(1, sizeof(vmon_proc_files_t));

		(*store)->refcnt = 1;
		(*store)->fd_dir = opendirf(vmon, vmon->proc_dir, "%i/fd", proc->pid);

		INIT_LIST_HEAD(&(*store)->fds);
	} else if ((*store)->fd_dir) {
		/* we have a directory handle, and we're reentering the function with the directory positioned at the end */
		seekdir((*store)->fd_dir, 0);
	}

	if (!(*store)->fd_dir)
		/* we have no directory handle on reentry */
		/* XXX TODO: this happens when we don't have permission, this hsould be handled more elegantly/cleanly, and we don't
		 * make any attempt to ever reopen the directory because the ctor has already run. */
		goto _fail;

	start = &(*store)->fds;
	while ((dentry = readdir((*store)->fd_dir))) {
		if (dentry->d_name[0] == '.' && (dentry->d_name[1] == '\0' || (dentry->d_name[1] == '.' && dentry->d_name[2] == '\0')))
			continue; /* skip . and .. */

		fdnum = atoi(dentry->d_name);

		/* search the process' files list for this fdnum */
		fd = NULL;
		list_for_each(cur, start) {
			if (cur == &(*store)->fds)
				continue; /* skip the head node as it's not boxed */

			if (list_entry(cur, vmon_proc_fd_t, fds)->fdnum == fdnum) {
				fd = list_entry(cur, vmon_proc_fd_t, fds);
				break;
			}
		}

		if (!fd) {
			fd = calloc(1, sizeof(vmon_proc_fd_t));
			if (!fd)
				goto _fail; /* TODO: errors */

			fd->fdnum = fdnum;
			fd->process = proc;
			INIT_LIST_HEAD(&fd->ref_fds);
			list_add_tail(&fd->fds, start);
		} else {
			list_move_tail(&fd->fds, start);
		}

		fd->generation = vmon->generation;

		/* Always readlink the fd path, since we have no way of knowing if it's a repurposed fdnum even when we've found a match.
		 * It would be nice if say.. the mtime of the /proc/$pid/fd directory changed if there's been any fd activity so we could skip the entire
		 * readdir+readlink loop in idle times, worth investigating. */
		readlinkf(vmon, &fd->object_path, vmon->proc_dir, "%i/fd/%s", proc->pid, dentry->d_name);

		cur_object = fd->object; /* stow the current object reference before we potentially replace it so we may unreference it if needed  */
		fd->object = fobject_lookup_hinted(vmon, fd->object_path.array, cur_object);
		if (cur_object != fd->object) {
			if (cur_object)
				fobject_unref(vmon, cur_object, fd);

			if (fd->object)
				fobject_ref(vmon, fd->object, fd);
		} /* else { lookup returned the same object as before (or NULL), is there anything to do? } */

		start = &fd->fds; /* update the search start to this fd */
	}

	/* search for stale (closed) fds, remove references for any we find */
	list_for_each_entry_safe(fd, _fd, &(*store)->fds, fds) {
		if (fd->generation == vmon->generation)
			break;

		del_fd(vmon, fd);
	}

	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;

_fail:
	return SAMPLE_ERROR;
}


/* implements the vm utilization sampling */
typedef enum _vmon_proc_vm_fsm_t {
#define VMON_ENUM_PARSER_STATES
#include "defs/proc_vm.def"
} vmon_proc_vm_fsm_t;

static sample_ret_t proc_sample_vm(vmon_t *vmon, vmon_proc_t *proc, vmon_proc_vm_t **store)
{
	int			i, len, total = 0;
	int			changes = 0;
	vmon_proc_vm_fsm_t	state = VMON_PARSER_STATE_PROC_VM_SIZE_PAGES;
#define VMON_PREPARE_PARSER
#include "defs/proc_vm.def"

	assert(vmon);
	assert(store);

	if (!proc) { /* dtor */
		try_close(&(*store)->statm_fd);

		return DTOR_FREE;
	}

	if (!(*store)) { /* ctor */
		(*store) = calloc(1, sizeof(vmon_proc_vm_t));
		if (proc->is_thread) {
			(*store)->statm_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/task/%i/statm", proc->pid, proc->pid);
		} else {
			(*store)->statm_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/statm", proc->pid);
		}

		/* initially everything is considered changed */
		memset((*store)->changed, 0xff, sizeof((*store)->changed));
	} else {
		/* clear the entire changed bitmap */
		memset((*store)->changed, 0, sizeof((*store)->changed));
	}

	/* read in statm and parse it assigning the vm members accordingly */
	while ((len = try_pread((*store)->statm_fd, vmon->buf, sizeof(vmon->buf), total)) > 0) {
		total += len;

		for (i = 0; i < len; i++) {
			/* parse the fields from the file, stepping through... */
			_p.input = vmon->buf[i];
			switch (state) {
#define VMON_IMPLEMENT_PARSER
#include "defs/proc_vm.def"
				default:
					/* we're finished parsing once we've fallen off the end of the symbols */
					goto _out; /* this saves us the EOF read syscall */
			}
		}
	}

_out:
	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;
}


/* implements the io utilization sampling */
typedef enum _vmon_proc_io_fsm_t {
#define VMON_ENUM_PARSER_STATES
#include "defs/proc_io.def"
} vmon_proc_io_fsm_t;

static sample_ret_t proc_sample_io(vmon_t *vmon, vmon_proc_t *proc, vmon_proc_io_t **store)
{
	int			i, len, total = 0;
	int			changes = 0;
	vmon_proc_io_fsm_t	state = VMON_PARSER_STATE_PROC_IO_RCHAR_LABEL;
#define VMON_PREPARE_PARSER
#include "defs/proc_io.def"

	assert(vmon);
	assert(store);

	if (!proc) { /* dtor */
		try_close(&(*store)->io_fd);

		return DTOR_FREE;
	}

	if (!(*store)) { /* ctor */
		(*store) = calloc(1, sizeof(vmon_proc_io_t));
		if (proc->is_thread) {
			(*store)->io_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/task/%i/io", proc->pid, proc->pid);
		} else {
			(*store)->io_fd = openf(vmon, O_RDONLY, vmon->proc_dir, "%i/io", proc->pid);
		}

		/* initially everything is considered changed */
		memset((*store)->changed, 0xff, sizeof((*store)->changed));
	} else {
		/* clear the entire changed bitmap */
		memset((*store)->changed, 0, sizeof((*store)->changed));
	}

	/* read in io and parse it assigning the io members accordingly */
	while ((len = try_pread((*store)->io_fd, vmon->buf, sizeof(vmon->buf), total)) > 0) {
		total += len;

		for (i = 0; i < len; i++) {
			/* parse the fields from the file, stepping through... */
			_p.input = vmon->buf[i];
			switch (state) {
#define VMON_IMPLEMENT_PARSER
#include "defs/proc_io.def"
				default:
					/* we're finished parsing once we've fallen off the end of the symbols */
					goto _out; /* this saves us the EOF read syscall */
			}
		}
	}

_out:
	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;
}


/* here starts the private system-wide samplers */

typedef enum _vmon_sys_stat_fsm_t {
#define VMON_ENUM_PARSER_STATES
#include "defs/sys_stat.def"
} vmon_sys_stat_fsm_t;

/* system-wide stat sampling, things like CPU usages, stuff in /proc/stat */
static sample_ret_t sys_sample_stat(vmon_t *vmon, vmon_sys_stat_t **store)
{
	int				i, len, total = 0;
	int				changes = 0;
	vmon_sys_stat_fsm_t		state = VMON_PARSER_STATE_SYS_STAT_CPU_PREFIX;	/* this could be defined as the "VMON_PARSER_INITIAL_STATE" */
	struct timespec			ts;
	typeof((*store)->boottime)	boottime;
#define VMON_PREPARE_PARSER
#include "defs/sys_stat.def"

	assert(store);

	if (!vmon) { /* dtor */
		try_close(&(*store)->stat_fd);
		return DTOR_FREE;
	}

	if (!(*store)) { /* ctor */
		(*store) = calloc(1, sizeof(vmon_sys_stat_t));
		(*store)->stat_fd = openat(dirfd(vmon->proc_dir), "stat", O_RDONLY);
	}

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC	1000000000
#endif

	/* VMON_SYS_STAT_BOOTTIME */
	clock_gettime(CLOCK_BOOTTIME, &ts);
	boottime = ts.tv_sec * vmon->ticks_per_sec;
	boottime += ts.tv_nsec * vmon->ticks_per_sec / NSEC_PER_SEC;
	if ((*store)->boottime != boottime) {
		(*store)->boottime = boottime;
		BITSET((*store)->changed, VMON_SYS_STAT_BOOTTIME);
		changes++;
	}

	while ((len = try_pread((*store)->stat_fd, vmon->buf, sizeof(vmon->buf), total)) > 0) {
		total += len;

		for (i = 0; i < len; i++) {
			_p.input = vmon->buf[i];
			switch (state) {
#define VMON_PARSER_DELIM ' ' /* TODO XXX eliminate the need for this, I want the .def's to include all the data format knowledge */
#define VMON_IMPLEMENT_PARSER
#include "defs/sys_stat.def"
				default:
					/* we're finished parsing once we've fallen off the end of the symbols */
					goto _out; /* this saves us the EOF read syscall */
			}
		}
	}

_out:
	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;
}


/* system-wide vm sampling, /proc/meminfo & /proc/vmstat */
typedef enum _vmon_sys_vm_fsm_t {
#define VMON_ENUM_PARSER_STATES
#include "defs/sys_vm.def"
} vmon_sys_vm_fsm_t;

static sample_ret_t sys_sample_vm(vmon_t *vmon, vmon_sys_vm_t **store)
{
	int			i, len, total = 0;
	int			changes = 0;
	vmon_sys_vm_fsm_t	state = VMON_PARSER_STATE_SYS_VM_TOTAL_KB_LABEL; /* this could be defined as the "VMON_PARSER_INITIAL_STATE" */
#define VMON_PREPARE_PARSER
#include "defs/sys_vm.def"

	assert(store);

	if (!vmon) { /* dtor */
		try_close(&(*store)->meminfo_fd);
		return DTOR_FREE;
	}

	if (!(*store)) { /* ctor */
		(*store) = calloc(1, sizeof(vmon_sys_vm_t));
		(*store)->meminfo_fd = openat(dirfd(vmon->proc_dir), "meminfo", O_RDONLY);

		/* initially everything is considered changed */
		memset((*store)->changed, 0xff, sizeof((*store)->changed));
	} else {
		memset((*store)->changed, 0, sizeof((*store)->changed));
	}

	while ((len = try_pread((*store)->meminfo_fd, vmon->buf, sizeof(vmon->buf), total)) > 0) {
		total += len;

		for (i = 0; i < len; i++) {
			_p.input = vmon->buf[i];
			switch (state) {
#define VMON_PARSER_DELIM ' ' /* TODO XXX eliminate the need for this, I want the .def's to include all the data format knowledge */
#define VMON_IMPLEMENT_PARSER
#include "defs/sys_vm.def"
				default:
					/* we're finished parsing once we've fallen off the end of the symbols */
					goto _out; /* this saves us the EOF read syscall */
			}
		}
	}

_out:
	return changes ? SAMPLE_CHANGED : SAMPLE_UNCHANGED;
}


/* here begins the public interface */

/* initialize a vmon instance, proc_wants is a default wants mask, optionally inherited vmon_proc_monitor() calls */
int vmon_init(vmon_t *vmon, vmon_flags_t flags, vmon_sys_wants_t sys_wants, vmon_proc_wants_t proc_wants)
{
	int	i;

	assert(vmon);

	if ((flags & VMON_FLAG_PROC_ALL) && (proc_wants & VMON_WANT_PROC_FOLLOW_CHILDREN))
		return 0;

	if (!(vmon->proc_dir = opendir("/proc")))
		return 0;

	INIT_LIST_HEAD(&vmon->processes);
	INIT_LIST_HEAD(&vmon->orphans);

	for (i = 0; i < VMON_HTAB_SIZE; i++)
		INIT_LIST_HEAD(&vmon->htab[i]);

	memset(vmon->stores, 0, sizeof(vmon->stores));

	/* TODO XXX: rename to something processes-specific! see vmon.h */
	vmon->array = NULL;
	vmon->array_allocated_nr = vmon->array_active_nr = vmon->array_hint_free = 0;

	/* TODO XXX: this is temporary, see vmon.h */
	INIT_LIST_HEAD(&vmon->fobjects);
	vmon->fobjects_nr = 0;

	vmon->flags = flags;
	vmon->sys_wants = sys_wants;
	vmon->proc_wants = proc_wants;
	vmon->ticks_per_sec = sysconf(_SC_CLK_TCK);
	vmon->num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (vmon->num_cpus <= 0)
		vmon->num_cpus = 1; /* default to 1 cpu */

	/* here we populate the sys and proc function tables */
#define vmon_want(_sym, _name, _func) \
	vmon->sys_funcs[VMON_STORE_ ## _sym] = (int(*)(vmon_t *, void **))_func;
#include "defs/sys_wants.def"

#define vmon_want(_sym, _name, _func) \
	vmon->proc_funcs[VMON_STORE_ ## _sym] = (int(*)(vmon_t *, vmon_proc_t *, void **))_func;
#include "defs/proc_wants.def"

	vmon->sample_cb = NULL;
	vmon->proc_ctor_cb = NULL;
	vmon->proc_dtor_cb = NULL;
	vmon->fobject_ctor_cb = NULL;
	vmon->fobject_dtor_cb = NULL;

	return 1;
}


/* destroy vmon instance */
void vmon_destroy(vmon_t *vmon)
{
	/* TODO: do we want to forcibly unmonitor everything being monitored still, or require the caller to have done that beforehand? */
	/* TODO: cleanup other shit, like closedir(vmon->proc_dir), etc */
}


/* monitor a process under a given vmon instance, the public interface.
 * XXX note it's impossible to say "none" for wants per-process, just "inherit", if vmon_init() was told a proc_wants of "inherit" then it's like having "none"
 * proc_wants for all proceses, perhaps improve this if there's a pressure to support this use case */
vmon_proc_t * vmon_proc_monitor(vmon_t *vmon, int pid, vmon_proc_wants_t wants, void (*sample_cb)(vmon_t *, void *, vmon_proc_t *, void *), void *sample_cb_arg)
{
	return proc_monitor(vmon, NULL, pid, wants, sample_cb, sample_cb_arg);
}


/* stop monitoring a process under a given vmon instance, a caller who supplied a sample_cb & sample_cb_arg pair @ monitor() must also supply it @ unmonitor! */
void vmon_proc_unmonitor(vmon_t *vmon, vmon_proc_t *proc, void (*sample_cb)(vmon_t *, void *, vmon_proc_t *, void *), void *sample_cb_arg)
{
	vmon_proc_t	*child, *_child;
	int		i;

	assert(vmon);
	assert(proc);
	assert(!sample_cb_arg || sample_cb);

	if (sample_cb) { /* uninstall callback */
		vmon_proc_callback_t	*cb, *_cb;

		list_for_each_entry_safe(cb, _cb, &proc->sample_callbacks, callbacks) {
			if (cb->func == sample_cb && cb->arg == sample_cb_arg) {
				list_del(&cb->callbacks);
				free(cb);
				break;
			}
		}
	}

	if (--(proc->refcnt)) /* if the process still has references, don't free it */
		return;

	/* recursively unmonitor any children processes being monitored */
	list_for_each_entry_safe(child, _child, &proc->children, siblings) {
		if (child->parent == proc) {
			child->parent = NULL;					/* children we're currently a parent of, we're going bye bye, so make them orphans */
			list_add_tail(&child->siblings, &vmon->orphans);	/* if the child becomes actually dtor'd in the nxt step it immediately will be discarded from the orphans list ... */
		}
		vmon_proc_unmonitor(vmon, child, NULL, NULL);
	}

	/* unmonitor all threads being monitored, suppressed if this process is a thread itself, as threads don't have children */
	if (!proc->is_thread) {
		list_for_each_entry_safe(child, _child, &proc->threads, threads)
			vmon_proc_unmonitor(vmon, child, NULL, NULL);
	}

	/* if maintaining a process array NULL out the entry */
	if ((vmon->flags & VMON_FLAG_PROC_ARRAY) && (i = find_proc_in_array(vmon, proc, proc->array_hint_pos)) != -1) {
		vmon->array[i] = NULL;
		/* store its index if the current free hint isn't free, or it's lower than the current free hint, making the hint useful */
		if (vmon->array[vmon->array_hint_free] || i < vmon->array_hint_free)
			vmon->array_hint_free = i;
	}

	list_del(&proc->siblings);
	if (proc->is_thread)
		list_del(&proc->threads);
	list_del(&proc->bucket);

	if (proc->parent) {	/* XXX TODO: verify this works ok for unmonitored orphans */
		/* set the children changed flag in the parent */
		if (proc->is_thread) {
			proc->parent->threads_changed = 1;
		} else {
			proc->parent->children_changed = 1; /* XXX TODO: for now I modify children_changed for both when the child dtor occurs and is_stale gets set */
		}
	} else {
		/* or the toplevel processes changed flag if parentless/toplevel */
		vmon->processes_changed = 1;
	}

	for (i = 0; i < sizeof(vmon->proc_funcs) / sizeof(vmon->proc_funcs[VMON_STORE_PROC_STAT]); i++) {
		if (proc->stores[i] != NULL) {	/* any non-NULL stores must have a function installed and must have been sampled, invoke the dtor branch */
			sample_ret_t	r;

			r = vmon->proc_funcs[i](vmon, NULL, &proc->stores[i]);
			switch (r) {
			case DTOR_FREE:
				try_free((void **)&proc->stores[i]);
				break;
			case DTOR_NOFREE:
				break;
			default:
				assert(0);
			}
		}
	}

	/* invoke the dtor if set, note it only happens when the vmon_proc_t instance is about to be freed, not when it transitions to proc.is_stale=1  */
	if (vmon->proc_dtor_cb)
		vmon->proc_dtor_cb(vmon, proc);

	free(proc);
}


/* internal sampling helper, perform sampling for a given process */
static void sample(vmon_t *vmon, vmon_proc_t *proc)
{
	int	i, wants, cur;

	assert(vmon);
	assert(proc);

	proc->children_changed = proc->threads_changed = 0;

	/* load this process monitors wants, or inherit the default */
	wants = proc->wants ? proc->wants : vmon->proc_wants;

	proc->activity = 0;
	for (i = 0, cur = 1; wants; cur <<= 1, i++) {
		if (wants & cur) {
			if (vmon->proc_funcs[i](vmon, proc, &proc->stores[i]) == SAMPLE_CHANGED)
				proc->activity |= cur;

			wants &= ~cur;
		}
	}
}


/* internal sampling helper, perform sampling for all sibling processes in the provided siblings list */
static int sample_threads(vmon_t *vmon, list_head_t *threads)
{
	vmon_proc_t	*proc;

	assert(vmon);
	assert(threads);

	list_for_each_entry(proc, threads, threads) {
		sample(vmon, proc);

#if 0
		/* callbacks can't be installed currently on threads */
		vmon_proc_callback_t	*cb;

		list_for_each_entry(cb, &proc->sample_callbacks, callbacks)
			cb->func(vmon, vmon->sample_cb_arg, proc, cb->arg);
#endif
	}

	return 1;
}


/* internal single-pass sampling helper, recursively perform sampling and callbacks for all sibling processes in the provided siblings list */
static int sample_siblings_unipass(vmon_t *vmon, list_head_t *siblings)
{
	vmon_proc_t	*proc, *_proc;

	assert(vmon);
	assert(siblings);

	list_for_each_entry_safe(proc, _proc, siblings, siblings) {
		vmon_proc_callback_t	*cb;
		int			entered_new = 0;

		/* prepare to transition top-level processes out of the is_new state, there's no sampler acting on behalf of top-level processes */
		if (proc->is_new)
			entered_new = 1;

		sample(vmon, proc);			/* invoke samplers for this node */
		sample_threads(vmon, &proc->threads);	/* invoke samplers for this node's threads */
		sample_siblings_unipass(vmon, &proc->children); /* invoke samplers for this node's children, and their callbacks, by recursing into this function */
							/* XXX TODO: error returns */

		/* if this is the top-level processes list, and proc has found a parent through the above sampling, migrate it to the parent's children list */
		if (siblings == &vmon->processes && proc->parent) {
			list_del(&proc->siblings);
			list_add_tail(&proc->siblings, &proc->parent->children);
			continue;
		}

		/* XXX note that sample_callbacks are called only after all the descendants have had their sampling performed (and their potential callbacks invoked)
		 * this enables the installation of a callback at a specific node in the process hierarchy which can also perform duties on behalf of the children
		 * being monitored, handy when automatically following children, an immediately relevant use case (vwm)
		 */
		list_for_each_entry(cb, &proc->sample_callbacks, callbacks)
			cb->func(vmon, vmon->sample_cb_arg, proc, cb->arg);

		/* transition new to non-new processes where we're responsible, this is a slight problem */
		if (!proc->parent && entered_new)
			proc->is_new = 0;
	}

	/* if we've just finished the top-level processes, and we have some orphans, we want to make the orphans top-level processes and sample them as such */
	if (siblings == &vmon->processes && !list_empty(&vmon->orphans))
		/* XXX TODO: do I need to get these orphans sampled immediately for this sample?  Are they getting skipped this sample?
		 * TODO: instrument the samplers to see if generation is > 1 behind to detect drops... */
		list_splice_init(&vmon->orphans, vmon->processes.prev);

	return 1;
}


/* 2pass version of the internal hierarchical sampling helper */
static int sample_siblings_pass1(vmon_t *vmon, list_head_t *siblings)
{
	vmon_proc_t	*proc, *_proc;

	assert(vmon);
	assert(siblings);

	/* invoke samplers */
	list_for_each_entry_safe(proc, _proc, siblings, siblings) {
		sample(vmon, proc);			/* invoke samplers for this node */
		sample_threads(vmon, &proc->threads);	/* invoke samplers for this node's threads */
		sample_siblings_pass1(vmon, &proc->children); /* invoke samplers for this node's children, by recursing into this function */
							/* XXX TODO: error returns */

		/* if this is the top-level processes list, and proc has found a parent through the above sampling, migrate it to the parent's children list */
		/* XXX: clarification; a process may have been monitored explicitly by an external caller which then becomes monitored as the followed child of
		 * another process, here we detect that and move the process out of the top-level processes list and down to the children list it belongs to. */
		if (siblings == &vmon->processes) {
			/* XXX: nobody maintains the generation numbers for the top-level processes, so it's done here...
			 * TODO: detect exited top-level processes and do something useful? */
			proc->generation = vmon->generation;

			if (proc->parent) {
				list_del(&proc->siblings);
				list_add_tail(&proc->siblings, &proc->parent->children);
			}
		}
	}

	if (siblings == &vmon->processes && !list_empty(&vmon->orphans))
		/* XXX TODO: do I need to get these orphans sampled immediately for this sample?  Are they getting skipped this sample?
		 * TODO: instrument the samplers to see if generation is > 1 behind to detect drops and log as bugs... */
		list_splice_init(&vmon->orphans, vmon->processes.prev);

	return 1;
}


static int sample_siblings_pass2(vmon_t *vmon, list_head_t *siblings)
{
	vmon_proc_t	*proc;

	assert(vmon);
	assert(siblings);

	/* invoke callbacks */
	list_for_each_entry(proc, siblings, siblings) {
		vmon_proc_callback_t	*cb;

		sample_siblings_pass2(vmon, &proc->children);	/* recurse into children, we invoke callbacks as encountered on nodes from the leaves up */

		list_for_each_entry(cb, &proc->sample_callbacks, callbacks)
			cb->func(vmon, vmon->sample_cb_arg, proc, cb->arg);

		if (!proc->parent && proc->is_new)		/* top-level processes aren't managed by a follower/sampler, so we need to clear their is_new flag, this approach is slightly deviant from the managed case,
								 * as the managed case will actually allow the process to retain its is_new flag across a vmon_sample() cycle, where we're clearing it @ exit.  It makes
								 * no difference to the callbacks, but if someone walks the tree after vmon_sample() relying on is_new, they'll be confused. XXX TODO FIXME
								 * May be able to leverage the generation number and turn the top-level sampler into more of a follower analog???
								 */
			proc->is_new = 0;
	}

	return 1;
}


/* collect information for all monitored processes, this is the interesting part, call it periodically at a regular interval */
int vmon_sample(vmon_t *vmon)
{
	int	i, wants, cur, ret = 1;

	assert(vmon);

	vmon->generation++;

	/* first manage the "all processes monitored" use case, this doesn't do any sampling, it just maintains the top-level list of processes being monitored */
	/* note this doesn't cover threads, as linux doesn't expose threads in the readdir of /proc, even though you can directly look them up at /proc/$tid */
	if ((vmon->flags & VMON_FLAG_PROC_ALL)) {
		struct dirent	*dentry;
		list_head_t	*tmp, *_tmp = &vmon->processes;

		/* if VMON_FLAG_PROC_ALL flag is set, quite a different code path is used which simply readdir()'s /proc, treating every numeric directory found
		 * as a process to monitor.  The list of toplevel processes being monitored is kept in sync with these, automatically monitoring
		 * new processes found, and unmonitoring processes now absent, in a fashion very similar to the proc_sample_files() code. */
		seekdir(vmon->proc_dir, 0);
		while ((dentry = readdir(vmon->proc_dir))) {
			int	pid, found;

			if (dentry->d_type != DT_DIR || dentry->d_name[0] < '0' || dentry->d_name[0] > '9')
				continue; /* skip non-directories and non-numeric directories */

			pid = atoi(dentry->d_name);

			found = 0;
			list_for_each(tmp, _tmp) { /* note how we use _tmp as the head for the processes iteration, resuming from the last process' node */
				if (tmp == &vmon->processes)
					continue; /* must take care to skip the processes list_head_t, as it's not a vmon_proc_t */

				if (list_entry(tmp, vmon_proc_t, siblings)->pid == pid) {
					found = 1;
					break;
				}
			}

			if (!found) {
				/* monitor the process */
				vmon_proc_t	*proc = proc_monitor(vmon, NULL, pid, vmon->proc_wants, NULL, NULL);

				if (!proc)
					continue; /* TODO error */

				tmp = &proc->siblings;
			}

			list_move_tail(tmp, _tmp); /* place this process next to the current start (XXX there are cases where it's already there) */

			list_entry(tmp, vmon_proc_t, siblings)->generation = vmon->generation; /* for identifying stale nodes */

			_tmp = tmp; /* set this node as the head to start the next search from */
		}

		list_for_each_safe(tmp, _tmp, &vmon->processes) { /* XXX safe version needed since vmon_proc_unmonitor() deletes from the processes list */
			if (list_entry(tmp, vmon_proc_t, siblings)->generation != vmon->generation) {
				vmon_proc_unmonitor(vmon, list_entry(tmp, vmon_proc_t, siblings), NULL, NULL);
			} else {
				/* we can stop looking for stale processes upon finding a non-stale one, since they've been sorted by the above algorithm */
				break;
			}
		}
	}

	/* now for actual sampling */

	/* first the sys-wide samplers */
	wants = vmon->sys_wants; /* the caller-requested sys-wide wants */
	vmon->activity = 0;
	for (i = 0, cur = 1; wants; cur <<= 1, i++) {
		if (wants & cur) {
			if (vmon->sys_funcs[i](vmon, &vmon->stores[i]) == SAMPLE_CHANGED)
				vmon->activity |= cur;

			wants &= ~cur;
		}
	}

	if (vmon->sample_cb)
		vmon->sample_cb(vmon, vmon->sample_cb_arg);

	/* then the per-process samplers */
	if ((vmon->flags & VMON_FLAG_PROC_ARRAY)) {
		int	j;
		/* TODO: determine if this really makes sense, if we always maintain a hierarchy even in array mode, then we
		 * should probably always sample in the hierarchical order, or maybe make it caller-specified.
		 * There is a benefit to invoking the callbacks in hierarchical order, the callbacks can make assumptions about the children
		 * having the callbacks invoked prior to the current node, if done in depth-first order....
		 * XXX this is a problem, figure out what to do with this, for now we don't even maintain the hierarchy in VMON_FLAG_PROC_ALL
		 * mode, only FOLLOW_CHILDREN mode, and it's likely PROC_ARRAY will generally be used together with PROC_ALL, so no hierarchy
		 * is available to traverse even if we wanted to.
		 */

		/* flat process-array ordered sampling, in this mode threads and processes are all placed flatly in the array,
		 * so this does the sampling for all monitored in no particular order */
		for (j = 0; j < vmon->array_allocated_nr; j++) {
			vmon_proc_t	*proc;

			if ((proc = vmon->array[j])) {
				vmon_proc_callback_t    *cb;

				sample(vmon, proc);

				list_for_each_entry(cb, &proc->sample_callbacks, callbacks)
					cb->func(vmon, vmon->sample_cb_arg, proc, cb->arg);

				/* age process, we use the presence of a parent as a flag indicating if the process is managed ala follow children/threads
				 * XXX TODO: should probably change the public _monitor() api to not contain a parent param then */
				if (!proc->parent && proc->is_new)
					proc->is_new = 0;
			}
		}
	} else if ((vmon->flags & VMON_FLAG_2PASS)) {
		/* recursive hierarchical depth-first processes tree sampling, at each node threads come before children, done in two passes:
		 * Pass 1. samplers
		 * Pass 2. callbacks
		  * XXX this is the path vwm utilizes, everything else is for other uses, like implementing top-like programs.
		 */
		ret = sample_siblings_pass1(vmon, &vmon->processes);	/* XXX TODO: errors */
		ret = sample_siblings_pass2(vmon, &vmon->processes);
	} else {
		/* recursive hierarchical depth-first processes tree sampling, at each node threads come before children, done in a single pass:
		 * Pass 1. samplers; callbacks (for every node)
		 */
		ret = sample_siblings_unipass(vmon, &vmon->processes);
	}

	return ret;
}


void vmon_dump_procs(vmon_t *vmon, FILE *out)
{
	assert(vmon);
	assert(out);

	fprintf(out, "generation=%i\n", vmon->generation);
	for (int i = 0; i < VMON_HTAB_SIZE; i++) {
		vmon_proc_t	*proc;

		list_for_each_entry(proc, &vmon->htab[i], bucket) {
			fprintf(out, "[%i] proc=%p parent=%p gen=%i pid=%i rc=%i is_threaded=%i is_thread=%i is_new=%u is_stale=%u\n",
				i, proc, proc->parent, proc->generation, proc->pid, proc->refcnt, (unsigned)proc->is_threaded, (unsigned)proc->is_thread, (unsigned)proc->is_new, (unsigned)proc->is_stale);

		}
	}
}
