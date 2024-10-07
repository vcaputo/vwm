/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2024  Vito Caputo - <vcaputo@pengaru.com>
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* libvmon integration, warning: this gets a little crazy especially in the rendering. */

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>

#ifdef USE_XLIB
#include <X11/extensions/Xfixes.h>
#endif

#include "charts.h"
#include "libvmon/vmon.h"
#include "list.h"
#include "util.h"
#include "vcr.h"

#ifdef USE_XLIB
#include "composite.h"
#include "xwindow.h"
#include "vwm.h"
#endif

#define CHART_ISTHREAD_ARGV		"~"				/* use this string to mark threads in the argv field */
#define CHART_NOCOMM_ARGV		"# missed it!"			/* use this string to substitute the command when missing in argv field */
#define CHART_MAX_ARGC			64				/* this is a huge amount */
#define CHART_VMON_PROC_WANTS		(VMON_WANT_PROC_STAT | VMON_WANT_PROC_FOLLOW_CHILDREN | VMON_WANT_PROC_FOLLOW_THREADS)
#define CHART_VMON_SYS_WANTS		(VMON_WANT_SYS_STAT)
#define CHART_MAX_COLUMNS		16
#define CHART_DELTA_SECONDS_EPSILON	.001f				/* adherence errors smaller than this are treated as zero */
#define CHART_NUM_FIXED_HEADER_ROWS	1				/* number of rows @ top before the hierarchy */

/* the global charts state, supplied to vwm_chart_create() which keeps a reference for future use. */
typedef struct _vwm_charts_t {
	vcr_backend_t				*vcr_backend;	/* supplied to vwm_charts_create() */

	/* libvmon */
	struct timeval				maybe_sample, last_sample, this_sample;
	unsigned				this_sample_duration;
	float					this_sample_adherence;	/* 0 = on time, (+) behind schedule, (-) ahead of schedule(TODO), units is fraction of .sampling_interval_secs */
	typeof(((vmon_sys_stat_t *)0)->user)	last_user_cpu;
	typeof(((vmon_sys_stat_t *)0)->system)	last_system_cpu;
	unsigned long long			last_total, this_total, total_delta;
	unsigned long long			last_idle, last_iowait, idle_delta, iowait_delta;
	vmon_t					vmon;
	float					prev_sampling_interval_secs, sampling_interval_secs;
	int					sampling_paused, contiguous_drops, primed;
	unsigned				defer_maintenance:1;
} vwm_charts_t;

typedef enum _vwm_column_type_t {
	VWM_COLUMN_VWM,
	VWM_COLUMN_ROW,
	VWM_COLUMN_PROC_USER,
	VWM_COLUMN_PROC_SYS,
	VWM_COLUMN_PROC_WALL,
	VWM_COLUMN_PROC_TREE,
	VWM_COLUMN_PROC_ARGV,
	VWM_COLUMN_PROC_PID,
	VWM_COLUMN_PROC_WCHAN,
	VWM_COLUMN_PROC_STATE,
	VWM_COLUMN_CNT
} vwm_column_type_t;

/* which side to pack the column onto */
typedef enum _vwm_side_t {
	VWM_SIDE_LEFT,
	VWM_SIDE_RIGHT,
	VWM_SIDE_CNT
} vwm_side_t;

/* how to horizontally justify contents within a given column's area */
typedef enum _vwm_justify_t {
	VWM_JUSTIFY_LEFT,
	VWM_JUSTIFY_RIGHT,
	VWM_JUSTIFY_CENTER,
	VWM_JUSTIFY_CNT
} vwm_justify_t;

typedef struct _vwm_column_t {
	/* TODO: make the columns configurable and add more description/toggled state here */
	unsigned		enabled:1;
	vwm_column_type_t	type;
	vwm_side_t		side;
	int			width;
} vwm_column_t;

/* everything needed by the per-window chart's context */
typedef struct _vwm_chart_t {
	vmon_proc_t	*proc;					/* vmon process monitor handle */
	vcr_t		*vcr;

	int		hierarchy_end;				/* row where the process hierarchy currently ends */

	/* FIXME TODO: this is redundant with the same things in vcr_t now, dedupe them */
	int		visible_width;				/* currently visible width of the chart */
	int		visible_height;				/* currently visible height of the chart */

	int		snowflakes_cnt;				/* count of snowflaked rows (reset to zero to truncate snowflakes display) */
	int		gen_last_composed;			/* the last composed vmon generation */
	int		redraw_needed;				/* if a redraw is required (like when the window is resized...) */
	char		*name;					/* name if provided, included in chart by the \/\/\ */
	vwm_column_t	columns[CHART_MAX_COLUMNS];		/* columns in the chart TODO, for now just stowing the real/user/sys width here */
	vwm_column_t	snowflake_columns[CHART_MAX_COLUMNS];	/* columns in the snowflaked rows */
} vwm_chart_t;

/* space we need for every process being monitored */
typedef struct _vwm_perproc_ctxt_t {
	typeof(((vmon_t *)0)->generation)	generation;
	typeof(((vmon_proc_stat_t *)0)->utime)	last_utime;
	typeof(((vmon_proc_stat_t *)0)->stime)	last_stime;
	typeof(((vmon_proc_stat_t *)0)->utime)	utime_delta;
	typeof(((vmon_proc_stat_t *)0)->stime)	stime_delta;
	int					row;
} vwm_perproc_ctxt_t;


static float	sampling_intervals[] = {
			1,	/* ~1Hz */
			.1,	/* ~10Hz */
			.05,	/* ~20Hz */
			.025,	/* ~40Hz */
			.01666,	/* ~60Hz */
		};


/* wrapper around snprintf always returning the length of what's in the buf */
static int snpf(char *str, size_t size, const char *format, ...)
{
	va_list	ap;
	int	ret;

	va_start(ap, format);
	ret = vsnprintf(str, size, format, ap);
	va_end(ap);

	return MIN(ret, size - 1);
}


/* this callback gets invoked at sample time once "per sys" */
static void sample_callback(vmon_t *vmon, void *arg)
{
	vwm_charts_t	*charts = arg;
	vmon_sys_stat_t	*sys_stat = vmon->stores[VMON_STORE_SYS_STAT];

	charts->this_total =	sys_stat->user + sys_stat->nice + sys_stat->system +
					sys_stat->idle + sys_stat->iowait + sys_stat->irq +
					sys_stat->softirq + sys_stat->steal + sys_stat->guest;

	charts->total_delta = charts->this_total - charts->last_total;
	charts->idle_delta = sys_stat->idle - charts->last_idle;
	charts->iowait_delta = sys_stat->iowait - charts->last_iowait;
}


/* these callbacks are invoked by the vmon library when process instances become monitored/unmonitored */
static void vmon_ctor_cb(vmon_t *vmon, vmon_proc_t *proc)
{
	VWM_TRACE("proc->pid=%i", proc->pid);
	proc->foo = calloc(1, sizeof(vwm_perproc_ctxt_t));
}


static void vmon_dtor_cb(vmon_t *vmon, vmon_proc_t *proc)
{
	VWM_TRACE("proc->pid=%i", proc->pid);
	if (proc->foo) {
		free(proc->foo);
		proc->foo = NULL;
	}
}


/* initialize charts system */
vwm_charts_t * vwm_charts_create(vcr_backend_t *vbe, unsigned flags)
{
	vwm_charts_t	*charts;

	charts = calloc(1, sizeof(vwm_charts_t));
	if (!charts) {
		VWM_PERROR("unable to allocate vwm_charts_t");
		goto _err;
	}

	charts->vcr_backend = vbe;

	if (flags & VWM_CHARTS_FLAG_DEFER_MAINTENANCE)
		charts->defer_maintenance = 1;

	charts->prev_sampling_interval_secs = charts->sampling_interval_secs = 0.1f;	/* default to 10Hz */

	if (!vmon_init(&charts->vmon, VMON_FLAG_2PASS, CHART_VMON_SYS_WANTS, CHART_VMON_PROC_WANTS)) {
		VWM_ERROR("unable to initialize libvmon");
		goto _err_charts;
	}

	charts->vmon.proc_ctor_cb = vmon_ctor_cb;
	charts->vmon.proc_dtor_cb = vmon_dtor_cb;
	charts->vmon.sample_cb = sample_callback;
	charts->vmon.sample_cb_arg = charts;
	gettimeofday(&charts->this_sample, NULL);

	return charts;

_err_charts:
	free(charts);

_err:
	return NULL;
}


/* teardown charts system */
void vwm_charts_destroy(vwm_charts_t *charts)
{
	/* TODO: free rest of stuff.. */
	free(charts);
}


/* moves what's below a given row up above it, preserve the lost row's graphs @ hierarchy end */
static void snowflake_row(vwm_charts_t *charts, vwm_chart_t *chart, int row)
{
	VWM_TRACE("pid=%i chart=%p row=%i heirarhcy_end=%i", chart->proc->pid, chart, row, chart->hierarchy_end);

	/* stash the graph rows */
	vcr_stash_row(chart->vcr, VCR_LAYER_GRAPHA, row);
	vcr_stash_row(chart->vcr, VCR_LAYER_GRAPHB, row);

	/* shift _all_ the layers up by 1 row */
	vcr_shift_below_row_up_one(chart->vcr, row);

	/* unstash the graph rows @ hierarchy end so we have them in the snowflakes */
	vcr_unstash_row(chart->vcr, VCR_LAYER_GRAPHA, chart->hierarchy_end);
	vcr_unstash_row(chart->vcr, VCR_LAYER_GRAPHB, chart->hierarchy_end);

	/* clear the others @ hierarchy end, new argv will get stamped over it */
	vcr_clear_row(chart->vcr, VCR_LAYER_TEXT, chart->hierarchy_end, -1, -1);
	vcr_clear_row(chart->vcr, VCR_LAYER_SHADOW, chart->hierarchy_end, -1, -1);
}


/* XXX TODO libvmon automagic children following races with explicit X client pid monitoring with different outcomes, it should be irrelevant which wins,
 *     currently the only visible difference is the snowflakes gap (hierarchy_end) varies, which is why I haven't bothered to fix it, I barely even notice.
 */


/* shifts what's below a given row down a row, and clears the row, preparing it for populating */
static void allocate_row(vwm_charts_t *charts, vwm_chart_t *chart, int row)
{
	VWM_TRACE("pid=%i chart=%p row=%i", chart->proc->pid, chart, row);

	vcr_shift_below_row_down_one(chart->vcr, row);
	/* FIXME TODO: the vcr layers api needs to just support bitmasks for which layers the operation
	 * applies to.
	 */
	vcr_clear_row(chart->vcr, VCR_LAYER_GRAPHA, row, -1, -1);
	vcr_clear_row(chart->vcr, VCR_LAYER_GRAPHB, row, -1, -1);
	vcr_clear_row(chart->vcr, VCR_LAYER_TEXT, row, -1, -1);
	vcr_clear_row(chart->vcr, VCR_LAYER_SHADOW, row, -1, -1);
}


/* shadow a row from the text layer in the shadow layer */
static void shadow_row(vwm_charts_t *charts, vwm_chart_t *chart, int row)
{
	vcr_shadow_row(chart->vcr, VCR_LAYER_TEXT, row);
}


/* simple helper to map the vmon per-proc argv array into an XTextItem array, deals with threads vs. processes and the possibility of the comm field not getting read in before the process exited... */
static void proc_argv2strs(const vmon_proc_t *proc, vcr_str_t *strs, int max_strs, int *res_n_strs)
{
	int	nr = 0;

	assert(proc);
	assert(strs);
	assert(max_strs > 2);
	assert(res_n_strs);

	if (proc->is_thread) {	/* stick the thread marker at the start of threads */
		strs[0].str = CHART_ISTHREAD_ARGV;
		strs[0].len = sizeof(CHART_ISTHREAD_ARGV) - 1;
		nr++;
	}

	if (((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len) {
		strs[nr].str = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.array;
		strs[nr].len = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len - 1;
	} else {
		/* sometimes a process is so ephemeral we don't manage to sample its comm, XXX TODO: we always have a pid, stringify it? */
		strs[nr].str = CHART_NOCOMM_ARGV;
		strs[nr].len = sizeof(CHART_NOCOMM_ARGV) - 1;
	}
	nr++;

	if (!proc->is_thread) { /* suppress the argv for threads */
		for (int i = 1; nr < max_strs && i < ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->argc; nr++, i++) {
			strs[nr].str = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->argv[i];
			strs[nr].len = strlen(((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->argv[i]);	/* TODO: libvmon should inform us of the length */
		}
	}

	*res_n_strs = nr;
}


/* helper for counting number of existing descendants subtrees */
static int count_rows(vmon_proc_t *proc)
{
	int		count = 1; /* XXX maybe suppress proc->is_new? */
	vmon_proc_t	*child;

	if (!proc->is_thread) {
		list_for_each_entry(child, &proc->threads, threads)
			count += count_rows(child);
	}

	list_for_each_entry(child, &proc->children, siblings)
		count += count_rows(child);

	return count;
}


/* helper for detecting if any children/threads in the process hierarchy rooted @ proc are new/stale this sample */
static int proc_hierarchy_changed(vmon_proc_t *proc)
{
	vmon_proc_t	*child;

	if (proc->children_changed || proc->threads_changed)
		return 1;

	if (!proc->is_thread) {
		list_for_each_entry(child, &proc->threads, threads) {
			if (proc_hierarchy_changed(child))
				return 1;
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		if (proc_hierarchy_changed(child))
			return 1;
	}

	return 0;
}


/* helper for drawing the vertical bars in the graph layers */
static void draw_bars(vwm_charts_t *charts, vwm_chart_t *chart, int row, double mult, double a_fraction, double a_total, double b_fraction, double b_total)
{
	float	a_t, b_t;

	/* compute the bar %ages for this sample */
	a_t = a_fraction / a_total * mult; /* TODO: these divides could be turned into multiplies, since the totals are sys-wide uniforms throughout the sample */
	b_t = b_fraction / b_total * mult;

	/* ensure at least 1 pixel when the scaled result is a fraction less than 1,
	 * I want to at least see 1 pixel blips for the slightest cpu utilization */
	vcr_draw_bar(chart->vcr, VCR_LAYER_GRAPHA, row, a_t, a_fraction > 0 ? 1 : 0);
	vcr_draw_bar(chart->vcr, VCR_LAYER_GRAPHB, row, b_t, b_fraction > 0 ? 1 : 0);
}


/* helper for marking a finish line at the current phase for the specified row */
static void mark_finish(vwm_charts_t *charts, vwm_chart_t *chart, int row)
{
	vcr_mark_finish_line(chart->vcr, VCR_LAYER_GRAPHA, row);
	vcr_mark_finish_line(chart->vcr, VCR_LAYER_GRAPHB, row);
}


/* helper for drawing a proc's argv @ specified x offset and row on the chart */
static void print_argv(const vwm_charts_t *charts, const vwm_chart_t *chart, int x, int row, const vmon_proc_t *proc, int *res_width)
{
	vcr_str_t	strs[VCR_DRAW_TEXT_N_STRS_MAX];
	int		n_strs;

	assert(chart);

	proc_argv2strs(proc, strs, NELEMS(strs), &n_strs);
	vcr_draw_text(chart->vcr, VCR_LAYER_TEXT, x, row, strs, n_strs, res_width);
}


/* determine if a given process has subsequent siblings in the hierarchy */
static inline int proc_has_subsequent_siblings(vmon_t *vmon, vmon_proc_t *proc)
{
	struct list_head	*sib, *head = &vmon->processes;

	if (proc->parent)
		head = &proc->parent->children;

	for (sib = proc->siblings.next; sib != head; sib = sib->next) {
		if (!(list_entry(sib, vmon_proc_t, siblings)->is_stale))
			return 1;
	}

	return 0;
}


/* convert chart sampling interval back into an integral hertz value, basically
 * open-coded ceilf(1.f / charts->sampling_interval_secs) to avoid needing -lm.
 */
static unsigned interval_as_hz(vwm_charts_t *charts)
{
	return (1.f / charts->sampling_interval_secs + .5f);
}


/* draw a process' row slice of a process tree */
static void draw_tree_row(vwm_charts_t *charts, vwm_chart_t *chart, int x, int depth, int row, const vmon_proc_t *proc, int *res_width)
{
	/* only if this process isn't the root process @ the window shall we consider all relational drawing conditions */
	if (proc != chart->proc) {
		vmon_proc_t	*child, *ancestor, *sibling, *last_sibling = NULL;
		int		bar_x = 0, bar_y = (row + 1) * VCR_ROW_HEIGHT;
		int		sub;

		/* XXX: everything done in this code block only dirties _this_ process' row in the rendered chart output */

		/* walk up the ancestors until reaching chart->proc, any ancestors we encounter which have more siblings we draw a vertical bar for */
		/* this draws the |'s in something like:  | |   |    | comm */
		for (sub = 1, ancestor = proc->parent; ancestor && ancestor != chart->proc; ancestor = ancestor->parent, sub++) {
			bar_x = ((depth - 1) - sub) * (VCR_ROW_HEIGHT / 2) + 4;

			assert(depth > 0);

			/* determine if the ancestor has remaining siblings which are not stale, if so, draw a connecting bar at its depth */
			if (proc_has_subsequent_siblings(&charts->vmon, ancestor))
				vcr_draw_ortho_line(chart->vcr, VCR_LAYER_TEXT,
					  x + bar_x, bar_y - VCR_ROW_HEIGHT,	/* dst x1, y1 */
					  x + bar_x, bar_y);			/* dst x2, y2 (vertical line) */
		}

		/* determine if _any_ of our siblings have children requiring us to draw a tee immediately before our comm string.
		 * The only sibling which doesn't cause this to happen is the last one in the children list, if it has children it has no impact on its remaining
		 * siblings, as there are none.
		 *
		 * This draws the + in something like:  | |    |  |    +comm
		 */

		/* find the last sibling (this has to be done due to the potential for stale siblings at the tail, and we'd rather not repeatedly check for it) */
		list_for_each_entry(sibling, &proc->parent->children, siblings) {
			if (!sibling->is_stale)
				last_sibling = sibling;
		}

		/* now look for siblings with non-stale children to determine if a tee is needed, ignoring the last sibling */
		list_for_each_entry(sibling, &proc->parent->children, siblings) {
			int	needs_tee = 0;

			/* skip stale siblings, they aren't interesting as they're invisible, and the last sibling has no bearing on wether we tee or not. */
			if (sibling->is_stale || sibling == last_sibling)
				continue;

			/* if any of the other siblings have children which are not stale, put a tee in front of our name, but ignore stale children */
			list_for_each_entry(child, &sibling->children, siblings) {
				if (!child->is_stale) {
					needs_tee = 1;
					break;
				}
			}

			/* if we still don't think we need a tee, check if there are threads */
			if (!needs_tee) {
				list_for_each_entry(child, &sibling->threads, threads) {
					if (!child->is_stale) {
						needs_tee = 1;
						break;
					}
				}
			}

			/* found a tee is necessary, all that's left is to determine if the tee is a corner and draw it accordingly, stopping the search. */
			if (needs_tee) {
				bar_x = (depth - 1) * (VCR_ROW_HEIGHT / 2) + 4;

				/* if we're the last sibling, corner the tee by shortening the vbar */
				if (proc == last_sibling) {
					vcr_draw_ortho_line(chart->vcr, VCR_LAYER_TEXT,
						  x + bar_x, bar_y - VCR_ROW_HEIGHT,	/* dst x1, y1 */
						  x + bar_x, bar_y - 4);			/* dst x2, y2 (vertical bar) */
				} else {
					vcr_draw_ortho_line(chart->vcr, VCR_LAYER_TEXT,
						  x + bar_x, bar_y - VCR_ROW_HEIGHT,	/* dst x1, y1 */
						  x + bar_x, bar_y);			/* dst x2, y2 (vertical bar) */
				}

				vcr_draw_ortho_line(chart->vcr, VCR_LAYER_TEXT,
					  x + bar_x, bar_y - 4,				/* dst x1, y1 */
					  x + bar_x + 2, bar_y - 4);			/* dst x2, y2 (horizontal bar) */

				/* terminate the outer sibling loop upon drawing the tee... */
				break;
			}
		}

		if (res_width)
			*res_width = depth * (VCR_ROW_HEIGHT / 2);
	}
}


/* draw a proc row according to the columns configured in columns,
 * row==0 is treated specially as the heading row
 */
static void draw_columns(vwm_charts_t *charts, vwm_chart_t *chart, vwm_column_t *columns, int heading, int depth, int row, const vmon_proc_t *proc)
{
	vmon_sys_stat_t		*sys_stat = charts->vmon.stores[VMON_STORE_SYS_STAT];
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;
	char			str[256];

	for (int i = 0, left = 0, right = 0; i < CHART_MAX_COLUMNS; i++) {
		vwm_column_t	*c = &columns[i];
		vwm_justify_t	str_justify = VWM_JUSTIFY_CENTER;
		int		str_len = 0, uniform = 1, advance = 1;

		if (!c->enabled)
			continue;

		/* XXX FIXME: i don't constrain columns using a clip mask or restrained drawing, so they can scribble
		 * on neighboring cells, this is especially problematic with long ARGVs like when compiling.
		 * As a kludge to work around this for now, clear the column's area immediately before drawing it,
		 * just in case something else scribbled into it.  This doesn't prevent subsequent scribbles, and if
		 * a column's width became too large, the clearing itself can be destructive.  This works fine for
		 * the currently configured columns, but long-term this will have to get fixed properly.
		 */
		if (c->side == VWM_SIDE_LEFT)
			vcr_clear_row(chart->vcr, VCR_LAYER_TEXT, row, left, c->width + VCR_ROW_HEIGHT / 2);
		else
			vcr_clear_row(chart->vcr, VCR_LAYER_TEXT, row, chart->visible_width - (c->width + VCR_ROW_HEIGHT / 2 + right), c->width + VCR_ROW_HEIGHT / 2);

		switch (c->type) {
		case VWM_COLUMN_VWM:
			if (heading) /* "\/\/\ # name @ XXHz" is only relevant to the heading */
				str_len = snpf(str, sizeof(str), "\\/\\/\\%s%s @ %2uHz ",
					chart->name ? " # " : "",
					chart->name ? chart->name : "",
					interval_as_hz(charts));

			uniform = 0; /* XXX this suppresses the c->width assignment so the column can be absent outside the heading */
			str_justify = VWM_JUSTIFY_RIGHT;
			break;

		case VWM_COLUMN_ROW: /* row in the chart */
			if (heading)
				str_len = snpf(str, sizeof(str), "Row");
			else
				str_len = snpf(str, sizeof(str), "%i", row - CHART_NUM_FIXED_HEADER_ROWS);

			str_justify = VWM_JUSTIFY_LEFT;
			/* this is kind of hacky, but libvmon doesn't monitor our row, it's implicitly "sampled" when we draw */
			proc_ctxt->row = row;
			break;

		case VWM_COLUMN_PROC_USER: /* User CPU time */
			if (heading)
				str_len = snpf(str, sizeof(str), "User");
			else
				str_len = snpf(str, sizeof(str), "%.2fs",
						(float)proc_stat->utime / (float)charts->vmon.ticks_per_sec);

			str_justify = VWM_JUSTIFY_RIGHT;
			break;

		case VWM_COLUMN_PROC_SYS: /* Sys CPU time */
			if (heading)
				str_len = snpf(str, sizeof(str), "Sys");
			else
				str_len = snpf(str, sizeof(str), "%.2fs",
						(float)proc_stat->stime / (float)charts->vmon.ticks_per_sec);

			str_justify = VWM_JUSTIFY_RIGHT;
			break;

		case VWM_COLUMN_PROC_WALL: /* User Sys Wall times */
			if (heading)
				str_len = snpf(str, sizeof(str), "Wall");
			else if (!proc_stat->start || proc_stat->start > sys_stat->boottime)
				str_len = snpf(str, sizeof(str), "??s");
			else
				str_len = snpf(str, sizeof(str), "%.2fs",
						(float)(sys_stat->boottime - proc_stat->start) / (float)charts->vmon.ticks_per_sec);

			str_justify = VWM_JUSTIFY_RIGHT;
			break;

		case VWM_COLUMN_PROC_TREE: { /* print a row of the process hierarchy tree */
			int	width = 0;

			advance = 0;	/* tree column manages its own advance; c->width is meaningless */

			if (heading)	/* tree markup needs no heading */
				break;

			assert(c->side == VWM_SIDE_LEFT); /* XXX: technically SIDE_RIGHT could work, but doesn't currently */

			draw_tree_row(charts, chart, left, depth, row, proc, &width);
			left += width;
			break;
		}

		case VWM_COLUMN_PROC_ARGV: { /* print the process' argv */
			if (heading) {
				str_len = snpf(str, sizeof(str), "ArgV/~ThreadName");
				str_justify = VWM_JUSTIFY_LEFT;
			} else {
				int	width;

				print_argv(charts, chart, left /* FIXME: consider c->side */, row, proc, &width);
				if (width > c->width) {
					c->width = width;
					chart->redraw_needed++;
				}
			}
			break;
		}

		case VWM_COLUMN_PROC_PID: /* print the process' PID */
			if (heading)
				str_len = snpf(str, sizeof(str), "PID");
			else
				str_len = snpf(str, sizeof(str), "%5i", proc->pid);

			str_justify = VWM_JUSTIFY_RIGHT;
			break;

		case VWM_COLUMN_PROC_WCHAN: /* print the process' wchan */
			if (heading)
				str_len = snpf(str, sizeof(str), "WChan");
			else {

				/* don't show wchan for processes with threads, since their main thread will show it. */
				if (!proc->is_thread && !list_empty(&proc->threads))
					break;

				str_len = snpf(str, sizeof(str), "%.*s",
						proc_stat->wchan.len,
						proc_stat->wchan.len == 1 && proc_stat->wchan.array[0] == '0' ? "-" : proc_stat->wchan.array);
			}

			str_justify = VWM_JUSTIFY_RIGHT;
			break;

		case VWM_COLUMN_PROC_STATE: /* print the process' state */
			if (heading)
				str_len = snpf(str, sizeof(str), "State");
			else {
				/* don't show process state for processes with threads, since their main thread will show it. */
				if (!proc->is_thread && !list_empty(&proc->threads))
					break;

				str_len = snpf(str, sizeof(str), "%c", proc_stat->state);
			}

			str_justify = VWM_JUSTIFY_CENTER;
			break;

		default:
			assert(0);
		}

		/* for plain string draws, str_len is left non-zero and they all get handled equally here */
		if (str_len) {
			const vcr_str_t	strs[1] = { (vcr_str_t){.str = str, .len = str_len} };
			int		str_width, xpos;

			/* get the width first, so we can place the text, note the -1 to suppress drawings */
			vcr_draw_text(chart->vcr, VCR_LAYER_TEXT, -1 /* x */, -1 /* row */, strs, 1, &str_width);
			if (uniform && str_width > c->width) {
				c->width = str_width;
				chart->redraw_needed++;
			}

			/* get xpos to the left edge of the column WRT c->width and c->side */
			switch (c->side) {
			case VWM_SIDE_LEFT:
				xpos = left;
				break;

			case VWM_SIDE_RIGHT:
				xpos = chart->visible_width - (right + c->width);
				break;

			default:
				assert(0);
			}

			/* adjust xpos according to str_justify and c->width */
			switch (str_justify) {
			case VWM_JUSTIFY_LEFT:
				/* xpos already @ left */
				break;

			case VWM_JUSTIFY_RIGHT:
				xpos += c->width - str_width;
				break;

			case VWM_JUSTIFY_CENTER:
				xpos += (c->width - str_width) / 2;
				break;

			default:
				assert(0);
			}

			vcr_draw_text(chart->vcr, VCR_LAYER_TEXT, xpos, row, strs, 1, NULL);
		}

		if (advance) {
			left += (c->side == VWM_SIDE_LEFT) * (c->width + VCR_ROW_HEIGHT / 2);
			right += (c->side == VWM_SIDE_RIGHT) * (c->width + VCR_ROW_HEIGHT / 2);
		}
	}
}


/* return if any of the enabled columns in columns has changed its contents */
static int columns_changed(const vwm_charts_t *charts, const vwm_chart_t *chart, vwm_column_t *columns, int row, const vmon_proc_t *proc)
{
	vmon_sys_stat_t		*sys_stat = charts->vmon.stores[VMON_STORE_SYS_STAT];
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;

	for (int i = 0; i < CHART_MAX_COLUMNS; i++) {
		const vwm_column_t	*c = &columns[i];

		if (!c->enabled)
			continue;

		switch (c->type) {
		case VWM_COLUMN_VWM:
			/* XXX: meh, maybe we should detect Hz changes here? */
			break;
		case VWM_COLUMN_ROW:
			return (row != proc_ctxt->row);
		case VWM_COLUMN_PROC_USER:
			if (BITTEST(proc_stat->changed, VMON_PROC_STAT_UTIME))
				return 1;
			break;
		case VWM_COLUMN_PROC_SYS:
			if (BITTEST(proc_stat->changed, VMON_PROC_STAT_STIME))
				return 1;
			break;
		case VWM_COLUMN_PROC_WALL:
			if (BITTEST(proc_stat->changed, VMON_PROC_STAT_START) ||
			    BITTEST(sys_stat->changed, VMON_SYS_STAT_BOOTTIME))
				return 1;
			break;
		case VWM_COLUMN_PROC_TREE:
			break;
		case VWM_COLUMN_PROC_ARGV:
			if (BITTEST(proc_stat->changed, VMON_PROC_STAT_ARGV))
				return 1;
			break;
		case VWM_COLUMN_PROC_PID:
			if (BITTEST(proc_stat->changed, VMON_PROC_STAT_PID))
				return 1;
			break;
		case VWM_COLUMN_PROC_WCHAN:
			if (BITTEST(proc_stat->changed, VMON_PROC_STAT_WCHAN))
				return 1;
			break;
		case VWM_COLUMN_PROC_STATE:
			if (BITTEST(proc_stat->changed, VMON_PROC_STAT_STATE))
				return 1;
			break;
		default:
			assert(0);
		}
	}

	return 0;
}


/* draws proc in a row of the process hierarchy */
static void draw_overlay_row(vwm_charts_t *charts, vwm_chart_t *chart, vmon_proc_t *proc, int depth, int row, int deferred_pass)
{
	/* if we're in defer_maintenance mode, don't do any of this until the deferred pass */
	if (charts->defer_maintenance && !deferred_pass)
		return;

	/* skip if obviously unnecessary (this can be further improved, but this makes a big difference as-is) */
	if (!deferred_pass && !chart->redraw_needed && !columns_changed(charts, chart, chart->columns, row, proc))
		return;

	if (!proc->is_new) /* XXX for now always clear the row, this should be capable of being optimized in the future (if the datums driving the text haven't changed...) */
		vcr_clear_row(chart->vcr, VCR_LAYER_TEXT, row, -1, -1);

	draw_columns(charts, chart, chart->columns, 0 /* heading */, depth, row, proc);
	shadow_row(charts, chart, row);
}


/* recursive draw function for "rest" of chart: the per-process rows (hierarchy, argv, state, wchan, pid...) */
static void draw_chart_rest(vwm_charts_t *charts, vwm_chart_t *chart, vmon_proc_t *proc, int *depth, int *row, int deferred_pass, unsigned sample_duration_idx)
{
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;
	vmon_proc_t		*child;
	double			utime_delta, stime_delta;

	/* Some parts of this we must do on every sample to maintain coherence in the graphs, since they're incrementally kept
	 * in sync with the process hierarchy, allocating and shifting the rows as processes are created and destroyed.  Everything
	 * else we should be able to skip doing unless chart.redraw_needed or their contents changed.
	 */

	/* if this is a deferred pass, we need to simply skip stale nodes, since they've already
	 * been snowflaked incrementally in the !deferred_pass branch below.
	 * Handling them in the deferred pass as well would just scribble over the snowflakes with
	 * stale stuff erroneously lingering in the live tree rows.
	 */
	if (deferred_pass && proc->is_stale)
		return;

	if (!deferred_pass) {
		/* These incremental/structural aspects can't be repeated in the final defer_maintenance pass since it's
		 * a repeated pass within the same sample - we can't realize these effects twice.
		 */
		if (sample_duration_idx == 0) { /* some things need to only be done once per sample duration, some at the start, some at the end */

			if (proc->is_stale) { /* we "realize" stale processes only in the first draw within a sample duration */
				/* what to do when a process (subtree) has gone away */
				static int	in_stale = 0;
				int		in_stale_entrypoint = 0;

				/* I snowflake the stale processes from the leaves up for a more intuitive snowflake order...
				 * (I expect the command at the root of the subtree to appear at the top of the snowflakes...) */
				/* This does require that I do a separate forward recursion to determine the number of rows
				 * so I can correctly snowflake in reverse */
				if (!in_stale) {
					VWM_TRACE("entered stale at chart=%p depth=%i row=%i", chart, *depth, *row);
					in_stale_entrypoint = in_stale = 1;
					(*row) += count_rows(proc) - 1;
				}

				(*depth)++;
				list_for_each_entry_prev(child, &proc->children, siblings) {
					draw_chart_rest(charts, chart, child, depth, row, deferred_pass, sample_duration_idx);
					(*row)--;
				}

				if (!proc->is_thread) {
					list_for_each_entry_prev(child, &proc->threads, threads) {
						draw_chart_rest(charts, chart, child, depth, row, deferred_pass, sample_duration_idx);
						(*row)--;
					}
				}
				(*depth)--;

				VWM_TRACE("%i (%.*s) is stale @ depth %i row %i is_thread=%i", proc->pid,
					((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len - 1,
					((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.array,
					(*depth), (*row), proc->is_thread);

				mark_finish(charts, chart, (*row));

				/* extract the row from the various layers */
				snowflake_row(charts, chart, (*row));
				chart->snowflakes_cnt++;

				/* stamp the name (and whatever else we include) into chart.text_picture */
				draw_columns(charts, chart, chart->snowflake_columns, 0 /* heading */, 0 /* depth */, chart->hierarchy_end, proc);
				shadow_row(charts, chart, chart->hierarchy_end);

				chart->hierarchy_end--;

				if (in_stale_entrypoint) {
					VWM_TRACE("exited stale at chart=%p depth=%i row=%i", chart, *depth, *row);
					in_stale = 0;
				}

				return;
			}

			/* use the generation number to avoid recomputing this stuff for callbacks recurring on the same process in the same sample */
			if (proc_ctxt->generation != charts->vmon.generation) {
				proc_ctxt->stime_delta = proc_stat->stime - proc_ctxt->last_stime;
				proc_ctxt->utime_delta = proc_stat->utime - proc_ctxt->last_utime;
				proc_ctxt->last_utime = proc_stat->utime;
				proc_ctxt->last_stime = proc_stat->stime;

				proc_ctxt->generation = charts->vmon.generation;
			}
		}

		if (proc->is_stale)
			return;	/* is_stale is already handled on the first sample_diration_idx */

		/* we "realize" new processes on the last draw within a duration.
		 * FIXME TODO: this could be placed more accurately in time by referencing the process's
		 * PROC_STAT_START time and allocating the row at that point within a duration.
		 * but for now it's still an improvement over losing time to simply place it at the end of
		 * the duration.  We don't have two samples to compute cpu utilizations for it anyways, so
		 * even if we were to place it accurately on the timeline, there wouldn't be data to put
		 * in the intervening space between the start line and the end anyways, which would be less
		 * accurate/potentially misleading - basically the start line would have to be repeated to
		 * fill in the space where we have no data so as to still indicate "hey, the process started
		 * back here, but this filled white region is where we couldn't collect anything about it
		 * since its start point.  This raises an interesting issue in general surrounding start lines
		 * in general; many processes tend to already exist when vmon starts up, and we draw the start
		 * lines when we begin monitoring a given process... and that is misleading if the process was
		 * preexisting.  In such caes, when the start time is way in the past, we should either suppress
		 * the start line, or be willing to place it out of phase - if the graph covers that moment.  If
		 * we were to place it out of phase, we'd have another situation where we can't leave the space
		 * between then and the current sample empty, it would have to all be filled with start line.
		 */
		if (proc->is_new) {
			if (sample_duration_idx != (charts->this_sample_duration - 1))
				return; /* suppress doing anything aboout new processes until the last draw within the duration */

			/* what to do when a process has been introduced */
			VWM_TRACE("%i is new", proc->pid);

			allocate_row(charts, chart, (*row));

			chart->hierarchy_end++;

			/* we need a minimum of two samples before we can compute a delta to plot,
			 * so we suppress that and instead mark the start of monitoring with an impossible 100% of both graph contexts, a starting line. */
			stime_delta = utime_delta = charts->total_delta;
		} else {
			stime_delta = proc_ctxt->stime_delta;
			utime_delta = proc_ctxt->utime_delta;
		}

		draw_bars(charts,
			chart,
			*row,
			(proc->is_thread || !proc->is_threaded) ? charts->vmon.num_cpus : 1.0,
			stime_delta,
			charts->total_delta,
			utime_delta,
			charts->total_delta);
	}

	/* only try draw the overlay on the last draw within a duration */
	if (sample_duration_idx == (charts->this_sample_duration - 1))
		draw_overlay_row(charts, chart, proc, *depth, *row, deferred_pass);
	(*row)++;

	/* recur any threads first, then any children processes */
	(*depth)++;
	if (!proc->is_thread) {	/* XXX: the threads member serves as the list head only when not a thread */
		list_for_each_entry(child, &proc->threads, threads) {
			draw_chart_rest(charts, chart, child, depth, row, deferred_pass, sample_duration_idx);
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		draw_chart_rest(charts, chart, child, depth, row, deferred_pass, sample_duration_idx);
	}
	(*depth)--;
}


/* recursive draw function entrypoint, draws the IOWait/Idle/HZ row, then enters draw_chart_rest() */
static void draw_chart(vwm_charts_t *charts, vwm_chart_t *chart, vmon_proc_t *proc, int deferred_pass, unsigned sample_duration_idx)
{
	int	prev_redraw_needed = chart->redraw_needed;
	int	row = 0, depth = 0;

	/* IOWait and Idle % @ row 0 */
	draw_bars(charts, chart, 0, 1.0, charts->iowait_delta, charts->total_delta, charts->idle_delta, charts->total_delta);

	/* only draw the \/\/\ and HZ if necessary */
	if (sample_duration_idx == (charts->this_sample_duration - 1)) {
		if (deferred_pass || (!charts->defer_maintenance && (chart->redraw_needed || charts->prev_sampling_interval_secs != charts->sampling_interval_secs))) {
			vcr_clear_row(chart->vcr, VCR_LAYER_TEXT, row, -1, -1);
			draw_columns(charts, chart, chart->columns, 1 /* heading */, 0 /* depth */, row, proc);
			shadow_row(charts, chart, row);
		}

		if (!prev_redraw_needed)
			chart->redraw_needed = proc_hierarchy_changed(proc);
	}
	row = CHART_NUM_FIXED_HEADER_ROWS;

	/* now everything else */
	draw_chart_rest(charts, chart, proc, &depth, &row, deferred_pass, sample_duration_idx);
	if (sample_duration_idx == (charts->this_sample_duration - 1)) {
		if (chart->redraw_needed > prev_redraw_needed) {
			/* Drawing bumped redraw_needed (like a layout change from widths changing),
			 * so don't reset the counter to zero forcing the next redraw.  TODO: this does cause
			 * a small delay between width-affecting values showing and column widths adjusting to them,
			 * resulting in a sort of eventually-consistent behavior.
			 * We could trigger a redraw here immediately by basically jumping back to the start of
			 * this function, but there are problems doing that as-is due to the stateful/incremental
			 * relationship between the charts and vmon's sample.  Rather than attacking that refactor
			 * now, I'll leave it like this for now.
			 */
			chart->redraw_needed = 1;
		} else
			chart->redraw_needed = 0;
	}
}


/* consolidated version of chart text and graph rendering, makes snowflakes integration cleaner, this always gets called regardless of the charts mode */
static void maintain_chart(vwm_charts_t *charts, vwm_chart_t *chart, int deferred_pass)
{
	assert(charts);
	assert(chart);
	/* let's make sure nobody's causing a deferred_pass=1 outside of defer_maintenance mode */
	assert(!deferred_pass || charts->defer_maintenance);

	if (!chart->proc || !chart->proc->stores[VMON_STORE_PROC_STAT])
		return;

	/* TODO:
	 * A side effect of responding to window resizes in this function is there's a latency proportional to the current sample_interval.
	 * Something to fix is to resize the charts when the window resizes.
	 * However, simply resizing the charts is insufficient.  Their contents need to be redrawn in the new dimensions, this is where it
	 * gets annoying.  The current maintain/draw_chart makes assumptions about being run from the periodic vmon per-process callback.
	 * There needs to be a redraw mode added where draw_chart is just reconstructing the current state, which requires that we suppress
	 * the phase advance in maintain_chart() and just enter draw_chart() to redraw everything for the same generation.
	 * So this probably requires some tweaking of draw_chart() as well as maintain_chart().  I want to be able tocall mainta_charts()
	 * from anywhere, and have it detect if it's being called on the same generation or if the generation has advanced.
	 * For now, the monitors will just be a little latent in window resizes which is pretty harmless artifact.
	 * XXX: ^^^ this comment is somewhat stale at this point now that deferred maintenance for headless mode happened,
	 *          furthermore this_sample_duration overlaps a bit with what's said above.
	 */

	/* deferred pass updates the arbitrarily reproducible overlays, not incrementally rendered graphs; this_sample_duration is irrelevant */
	if (deferred_pass)
		return draw_chart(charts, chart, chart->proc, deferred_pass, 0 /* sample_duration_idx */);

	for (unsigned i = 0; i < charts->this_sample_duration; i++) {
		vcr_advance_phase(chart->vcr, -1); /* change this to +1 to scroll the other direction */

		/* recursively draw the monitored processes to the chart */
		draw_chart(charts, chart, chart->proc, 0 /* deferred_pass */, i);
	}
}


/* this callback gets invoked at sample time for every process we've explicitly monitored (not autofollowed children/threads)
 * It's where we update the cumulative data for all windows, including the graph masks, regardless of their visibility
 * It's also where we compose the graphs and text for visible windows into a picture ready for compositing with the window contents */
static void proc_sample_callback(vmon_t *vmon, void *sys_cb_arg, vmon_proc_t *proc, void *proc_cb_arg)
{
	vwm_charts_t	*charts = sys_cb_arg;
	vwm_chart_t	*chart = proc_cb_arg;

	VWM_TRACE("proc=%p chart=%p", proc, chart);

	/* render the various always-updated charts, this is the component we do regardless of the charts mode and window visibility,
	 * essentially the incrementally rendered/historic components */
	maintain_chart(charts, chart, 0 /* deferred_pass */);

	/* XXX TODO: we used to mark repaint as being needed if this chart's window was mapped, but
	 * since extricating charts from windows that's no longer convenient, and repaint is
	 * always performed after a sample.  Make sure the repainting isn't costly when nothing
	 * charted is mapped (the case that code optimized)
	 */
}


/* reset snowflakes on the specified chart */
void vwm_chart_reset_snowflakes(vwm_charts_t *charts, vwm_chart_t *chart)
{
	if (chart->snowflakes_cnt) {
		chart->snowflakes_cnt = 0;
		chart->redraw_needed = 1;
	}
}


/* (re)size the specified chart's visible dimensions */
int vwm_chart_set_visible_size(vwm_charts_t *charts, vwm_chart_t *chart, int width, int height)
{
	chart->visible_width = width;
	chart->visible_height = height;

	if (vcr_resize_visible(chart->vcr, width, height) > 0)
		chart->redraw_needed = 1;

	return 1;
}


/* create an chart and start monitoring for the supplied pid */
vwm_chart_t * vwm_chart_create(vwm_charts_t *charts, int pid, int width, int height, const char *name)
{
	vwm_chart_t	*chart;

	chart = calloc(1, sizeof(vwm_chart_t));
	if (!chart) {
		VWM_PERROR("Unable to allocate vwm_chart_t");
		goto _err;
	}

	if (name) {
		chart->name = strdup(name);
		if (!chart->name) {
			VWM_PERROR("Unable to allocate name");
			goto _err_free;
		}
	}

	/* TODO: make the columns interactively configurable @ runtime */
	chart->columns[0] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_ROW, .side = VWM_SIDE_LEFT };
	chart->columns[1] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_USER, .side = VWM_SIDE_LEFT };
	chart->columns[2] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_SYS, .side = VWM_SIDE_LEFT };
	chart->columns[3] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_WALL, .side = VWM_SIDE_LEFT };
	chart->columns[4] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_TREE, .side = VWM_SIDE_LEFT };
	chart->columns[5] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_ARGV, .side = VWM_SIDE_LEFT };
	chart->columns[6] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_STATE, .side = VWM_SIDE_RIGHT };
	chart->columns[7] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_PID, .side = VWM_SIDE_RIGHT };
	chart->columns[8] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_WCHAN, .side = VWM_SIDE_RIGHT };
	chart->columns[9] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_VWM, .side = VWM_SIDE_RIGHT };

	chart->snowflake_columns[0] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_PID, .side = VWM_SIDE_LEFT };
	chart->snowflake_columns[1] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_USER, .side = VWM_SIDE_LEFT };
	chart->snowflake_columns[2] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_SYS, .side = VWM_SIDE_LEFT };
	chart->snowflake_columns[3] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_WALL, .side = VWM_SIDE_LEFT };
	chart->snowflake_columns[4] = (vwm_column_t){ .enabled = 1, .type = VWM_COLUMN_PROC_ARGV, .side = VWM_SIDE_LEFT };

	/* add the client process to the monitoring hierarchy */
	/* XXX note libvmon here maintains a unique callback for each unique callback+xwin pair, so multi-window processes work */
	chart->proc = vmon_proc_monitor(&charts->vmon, NULL, pid, VMON_WANT_PROC_INHERIT, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, chart);
	if (!chart->proc) {
		VWM_ERROR("Unable to establish proc monitor");
		goto _err_free;
	}

	 /* FIXME: count_rows() isn't returning the right count sometimes (off by ~1), it seems to be related to racing with the automatic child monitoring */
	 /* the result is an extra row sometimes appearing below the process hierarchy */
	chart->hierarchy_end = CHART_NUM_FIXED_HEADER_ROWS + count_rows(chart->proc);
	chart->gen_last_composed = -1;

	chart->vcr = vcr_new(charts->vcr_backend, &chart->hierarchy_end, &chart->snowflakes_cnt);

	if (!vwm_chart_set_visible_size(charts, chart, width, height)) {
		VWM_ERROR("Unable to set initial chart size");
		goto _err_unmonitor;
	}

	return chart;

_err_unmonitor:
	vmon_proc_unmonitor(&charts->vmon, chart->proc, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, chart);

_err_free:
	free(chart->name);
	free(chart);
_err:
	return NULL;
}


/* stop monitoring and destroy the supplied chart */
void vwm_chart_destroy(vwm_charts_t *charts, vwm_chart_t *chart)
{
	vmon_proc_unmonitor(&charts->vmon, chart->proc, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, chart);
	vcr_free(chart->vcr);
	free(chart->name);
	free(chart);
}


/* this composes the maintained chart into the base chart picture, this gets called from paint_all() on every repaint of xwin */
/* we noop the call if the gen_last_composed and proc->generation numbers match, indicating there's nothing new to compose. */
void vwm_chart_compose(vwm_charts_t *charts, vwm_chart_t *chart)
{
	if (!chart->visible_width || !chart->visible_height)
		return;

	if (chart->gen_last_composed == chart->proc->generation)
		return; /* noop if no sampling occurred since last compose */

	/* In deferred maintenance mode, we skip maintaining a bunch of layers until the compose happens.
	 * Normally the layers are maintained incrementally with sampling so real-time visualized use cases
	 * like vwm/vmon-xlib can always have a readily composed set of current layers to flatten/compose
	 * into the output picture sourced during window rendering/compositing.
	 *
	 * But in offline viewing situations (vmon-png), especially for lower end embedded devices, the
	 * libvmon sample rate tends to be orders of mangitude higher than the visualization rate. e.g.
	 * sampling occurs @ 1Hz, with vmon snapshotting a png every half hour.  In such situations it's
	 * wasteful to be maintaining all layers on every libvmon sample.  This is when deferred maintenance
	 * should be used - it puts off maintaining layers which can be reproduced at any time, while still
	 * maintaining the bare minimum needed for achieving correctness by compose time.  All the layers
	 * deferred between compose calls must still be maintained for compose to produce complete results,
	 * so we do one last maintain_chart() call with deferred_pass=1, forcing maintenance of all layers.
	 */
	if (charts->defer_maintenance)
		maintain_chart(charts, chart, 1 /* deferred_pass */);

	chart->gen_last_composed = chart->proc->generation; /* remember this generation */

	/* FIXME TODO: errors */ (void) vcr_compose(chart->vcr);
}


#ifdef USE_XLIB
/* xdamage producing variant of the above for vwm composited WM use */
void vwm_chart_compose_xdamage(vwm_charts_t *charts, vwm_chart_t *chart, XserverRegion *res_damaged_region)
{
	assert(charts);
	assert(chart);
	assert(res_damaged_region);

	vwm_chart_compose(charts, chart);
	/* damage the window to ensure the updated chart is drawn (TODO: this can be done more selectively/efficiently) */
	/* TODO errors: */ (void) vcr_get_composed_xdamage(chart->vcr, res_damaged_region);
}
#endif


/* render the chart into a picture at the specified coordinates and dimensions */
void vwm_chart_render(vwm_charts_t *charts, vwm_chart_t *chart, vcr_present_op_t op, vcr_dest_t *dest, int x, int y, int width, int height)
{
	if (!chart->visible_width || !chart->visible_height)
		return;

	vcr_present(chart->vcr, op, dest, x, y, width, height);
}


/* increase the sample rate relative to current using the table of intervals */
void vwm_charts_rate_increase(vwm_charts_t *charts)
{
	int	i;

	assert(charts);

	for (i = 0; i < NELEMS(sampling_intervals); i++) {
		if (sampling_intervals[i] < charts->sampling_interval_secs) {
			charts->sampling_interval_secs = sampling_intervals[i];
			break;
		}
	}
}


/* decrease the sample rate relative to current using the table of intervals */
void vwm_charts_rate_decrease(vwm_charts_t *charts)
{
	int	i;

	assert(charts);

	for (i = NELEMS(sampling_intervals) - 1; i >= 0; i--) {
		if (sampling_intervals[i] > charts->sampling_interval_secs) {
			charts->sampling_interval_secs = sampling_intervals[i];
			break;
		}
	}
}


/* set an arbitrary sample rate rather than using one of the presets, 0 to pause */
void vwm_charts_rate_set(vwm_charts_t *charts, unsigned hertz)
{
	assert(charts);

	/* XXX: note floating point divide by 0 simply results in infinity */
	charts->sampling_interval_secs = 1.0f / (float)hertz;
}


/* convenience function for returning the time delta as a seconds.fraction float */
static float delta(struct timeval *cur, struct timeval *prev)
{
	struct timeval	res;
	float		delta;

	/* determine the # of whole.fractional seconds between prev and cur */
	timersub(cur, prev, &res);

	delta = res.tv_sec;
	delta += (float)((float)res.tv_usec) / 1000000.0;

	return delta;
}


static inline int delta_close_enough(vwm_charts_t *charts, float delta)
{
	float	remainder = charts->sampling_interval_secs - delta;

	/* if within .1ms of the scheduled next sample (or behind schedule at all), consider it "close enough" and take the sample. */
	if (remainder <= CHART_DELTA_SECONDS_EPSILON)
		return 1;

	return 0;
}


/* update the charts if necessary, return if updating occurred, and duration before another update needed in *desired_delay_us */
int vwm_charts_update(vwm_charts_t *charts, int *desired_delay_us)
{
	int	ret = 0, sampled = 0;
	float	this_delta = 0.0f;

	gettimeofday(&charts->maybe_sample, NULL);
	this_delta = delta(&charts->maybe_sample, &charts->this_sample);
	if (!charts->primed ||
	    (charts->sampling_interval_secs == INFINITY && !charts->sampling_paused) || /* XXX this is kind of a kludge to get the 0 Hz indicator drawn before pausing */
	    (charts->sampling_interval_secs != INFINITY && delta_close_enough(charts, this_delta))) {
		vmon_sys_stat_t	*sys_stat;

		/* automatically lower the sample rate if we can't keep up with the current sample rate */
		if (charts->sampling_interval_secs < INFINITY &&
		    charts->sampling_interval_secs <= charts->prev_sampling_interval_secs &&
		    this_delta >= (charts->sampling_interval_secs * 1.5)) {

			/* adjust charts->this_sample_duration as needed since we've missed our deadline.
			 * This is more of an issue in headless mode, especially when run on slower/embedded
			 * devices, even worse when periodically snapshoting costly PNGs that may take
			 * several seconds during which no sampling occurs.
			 */
			charts->this_sample_duration = (this_delta / charts->sampling_interval_secs) + .5f /* rounded to int */;

			/* require > 1 contiguous drops before lowering the rate, tolerates spurious one-off stalls */
			if (++charts->contiguous_drops > 2)
				vwm_charts_rate_decrease(charts);
		} else {
			charts->contiguous_drops = 0;
			charts->this_sample_duration = 1; /* ideally always 1, but > 1 when sample deadline missed (repeat sample)  */
		}

		charts->this_sample_adherence = -(charts->sampling_interval_secs - this_delta);
		if (charts->this_sample_adherence < CHART_DELTA_SECONDS_EPSILON && charts->this_sample_adherence > -CHART_DELTA_SECONDS_EPSILON)
			charts->this_sample_adherence = 0;
		charts->this_sample_adherence /= charts->sampling_interval_secs; /* turn adherence into a fraction of the current interval */

		VWM_TRACE("sample_duration=%u sample_adherence=%f",
			charts->this_sample_duration, charts->this_sample_adherence);

		/* age the sys-wide sample data into "last" variables, before the new sample overwrites them. */
		charts->last_sample = charts->this_sample;
		charts->this_sample = charts->maybe_sample;
		if ((sys_stat = charts->vmon.stores[VMON_STORE_SYS_STAT])) {
			charts->last_user_cpu = sys_stat->user;
			charts->last_system_cpu = sys_stat->system;
			charts->last_total =	sys_stat->user +
						sys_stat->nice +
						sys_stat->system +
						sys_stat->idle +
						sys_stat->iowait +
						sys_stat->irq +
						sys_stat->softirq +
						sys_stat->steal +
						sys_stat->guest;

			charts->last_idle = sys_stat->idle;
			charts->last_iowait = sys_stat->iowait;
		}

		ret = vmon_sample(&charts->vmon);	/* XXX: calls proc_sample_callback() for explicitly monitored processes after sampling their descendants */
							/* XXX: also calls sample_callback() per invocation after sampling the sys wants */

		charts->sampling_paused = (charts->sampling_interval_secs == INFINITY);
		charts->prev_sampling_interval_secs = charts->sampling_interval_secs;

		/* "primed" is just a flag to ensure we always perform the first sample */
		if (!charts->primed)
			charts->primed = 1;

		sampled = 1;
	}

	if (charts->sampling_interval_secs == INFINITY) {
		*desired_delay_us = -1; /* sleep forever */
	} else {
		float	remaining_secs;

		if (sampled) {	/* sampling takes time, so let's subtract that from the interval-derived sleep time to try get the next sample started on-time (if possible) */
			struct timeval	post_sampled;

			gettimeofday(&post_sampled, NULL);
			this_delta += delta(&post_sampled, &charts->this_sample);
		}

		remaining_secs = charts->sampling_interval_secs - this_delta;
		if (remaining_secs <= 0) {
			/* always sleep some minimal amount, we don't want to spin */
			*desired_delay_us = CHART_DELTA_SECONDS_EPSILON * 1000000.f;
		} else {
			*desired_delay_us = remaining_secs * 1000000.f;
		}
	}

	return ret;
}
