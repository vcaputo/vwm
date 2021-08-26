/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2018  Vito Caputo - <vcaputo@pengaru.com>
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 3 as published
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

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>

#include "charts.h"
#include "composite.h"
#include "libvmon/vmon.h"
#include "list.h"
#include "vwm.h"
#include "xwindow.h"

#define CHART_MASK_DEPTH	8					/* XXX: 1 would save memory, but Xorg isn't good at it */
#define CHART_FIXED_FONT	"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1"
#define CHART_ROW_HEIGHT	15					/* this should always be larger than the font height */
#define CHART_GRAPH_MIN_WIDTH	200					/* always create graphs at least this large */
#define CHART_GRAPH_MIN_HEIGHT	(4 * CHART_ROW_HEIGHT)
#define CHART_ISTHREAD_ARGV	"~"					/* use this string to mark threads in the argv field */
#define CHART_NOCOMM_ARGV	"#missed it!"				/* use this string to substitute the command when missing in argv field */
#define CHART_MAX_ARGC		64					/* this is a huge amount */
#define CHART_VMON_PROC_WANTS	(VMON_WANT_PROC_STAT | VMON_WANT_PROC_FOLLOW_CHILDREN | VMON_WANT_PROC_FOLLOW_THREADS)
#define CHART_VMON_SYS_WANTS	(VMON_WANT_SYS_STAT)

/* the global charts state, supplied to vwm_chart_create() which keeps a reference for future use. */
typedef struct _vwm_charts_t {
	vwm_xserver_t				*xserver;	/* xserver supplied to vwm_charts_init() */

	/* libvmon */
	struct timeval				maybe_sample, last_sample, this_sample;
	typeof(((vmon_sys_stat_t *)0)->user)	last_user_cpu;
	typeof(((vmon_sys_stat_t *)0)->system)	last_system_cpu;
	unsigned long long			last_total, this_total, total_delta;
	unsigned long long			last_idle, last_iowait, idle_delta, iowait_delta;
	vmon_t					vmon;
	float					prev_sampling_interval, sampling_interval;
	int					sampling_paused, contiguous_drops;

	/* X */
	XFontStruct				*chart_font;
	GC					text_gc;
	Picture					shadow_fill,
						text_fill,
						bg_fill,
						snowflakes_text_fill,
						grapha_fill,
						graphb_fill,
						finish_fill;
} vwm_charts_t;

/* everything needed by the per-window chart's context */
typedef struct _vwm_chart_t {
	vmon_proc_t	*monitor;		/* vmon process monitor handle */
	Pixmap		text_pixmap;		/* pixmap for charted text (kept around for XDrawText usage) */
	Picture		text_picture;		/* picture representation of text_pixmap */
	Picture		shadow_picture;		/* text shadow layer */
	Picture		grapha_picture;		/* graph A layer */
	Picture		graphb_picture;		/* graph B layer */
	Picture		tmp_picture;		/* 1 row worth of temporary picture space */
	Picture		picture;		/* chart picture derived from the pixmap, for render compositing */
	int		width;			/* current width of the chart */
	int		height;			/* current height of the chart */
	int		visible_width;		/* currently visible width of the chart */
	int		visible_height;		/* currently visible height of the chart */
	int		phase;			/* current position within the (horizontally scrolling) graphs */
	int		heirarchy_end;		/* row where the process heirarchy currently ends */
	int		snowflakes_cnt;		/* count of snowflaked rows (reset to zero to truncate snowflakes display) */
	int		gen_last_composed;	/* the last composed vmon generation */
	int		redraw_needed;		/* if a redraw is required (like when the window is resized...) */
	char		*name;			/* name if provided, included in chart by the \/\/\ */
} vwm_chart_t;

/* space we need for every process being monitored */
typedef struct _vwm_perproc_ctxt_t {
	typeof(((vmon_t *)0)->generation)	generation;
	typeof(((vmon_proc_stat_t *)0)->utime)	last_utime;
	typeof(((vmon_proc_stat_t *)0)->stime)	last_stime;
	typeof(((vmon_proc_stat_t *)0)->utime)	utime_delta;
	typeof(((vmon_proc_stat_t *)0)->stime)	stime_delta;
} vwm_perproc_ctxt_t;


static float			sampling_intervals[] = {
						INFINITY,	/* STOPPED */
						1,		/* ~1Hz */
						.1,		/* ~10Hz */
						.05,		/* ~20Hz */
						.025,		/* ~40Hz */
						.01666};	/* ~60Hz */

static XRenderColor		chart_visible_color = { 0xffff, 0xffff, 0xffff, 0xffff },
				chart_shadow_color = { 0x0000, 0x0000, 0x0000, 0x8800},
				chart_bg_color = { 0x0, 0x1000, 0x0, 0x9000},
				chart_div_color = { 0x2000, 0x3000, 0x2000, 0x9000},
				chart_snowflakes_visible_color = { 0xd000, 0xd000, 0xd000, 0x8000 },
				chart_trans_color = {0x00, 0x00, 0x00, 0x00},
				chart_grapha_color = { 0xff00, 0x0000, 0x0000, 0x3000 },	/* ~red */
				chart_graphb_color = { 0x0000, 0xffff, 0xffff, 0x3000 };	/* ~cyan */
static XRenderPictureAttributes	pa_repeat = { .repeat = 1 };
static XRenderPictureAttributes	pa_no_repeat = { .repeat = 0 };


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


/* convenience helper for creating a pixmap */
static Pixmap create_pixmap(vwm_charts_t *charts, unsigned width, unsigned height, unsigned depth)
{
	vwm_xserver_t	*xserver = charts->xserver;

	return XCreatePixmap(xserver->display, XSERVER_XROOT(xserver), width, height, depth);
}


/* convenience helper for creating a picture, supply res_pixmap to keep a reference to the pixmap drawable. */
static Picture create_picture(vwm_charts_t *charts, unsigned width, unsigned height, unsigned depth, unsigned long attr_mask, XRenderPictureAttributes *attr, Pixmap *res_pixmap)
{
	vwm_xserver_t	*xserver = charts->xserver;
	Pixmap		pixmap;
	Picture		picture;
	int		format;

	/* FIXME this pixmap->picture dance seems silly, investigate further. TODO */
	switch (depth) {
		case 8:
			format = PictStandardA8;
			break;
		case 32:
			format = PictStandardARGB32;
			break;
		default:
			assert(0);
	}

	pixmap = create_pixmap(charts, width, height, depth);
	picture = XRenderCreatePicture(xserver->display, pixmap, XRenderFindStandardFormat(xserver->display, format), attr_mask, attr);

	if (res_pixmap) {
		*res_pixmap = pixmap;
	} else {
		XFreePixmap(xserver->display, pixmap);
	}

	return picture;
}


/* convenience helper for creating a filled picture, supply res_pixmap to keep a reference to the pixmap drawable. */
static Picture create_picture_fill(vwm_charts_t *charts, unsigned width, unsigned height, unsigned depth, unsigned long attrs_mask, XRenderPictureAttributes *attrs, const XRenderColor *color, Pixmap *res_pixmap)
{
	vwm_xserver_t	*xserver = charts->xserver;
	Picture		picture;

	picture = create_picture(charts, width, height, depth, attrs_mask, attrs, res_pixmap);
	XRenderFillRectangle(xserver->display, PictOpSrc, picture, color, 0, 0, width, height);

	return picture;
}


/* initialize charts system */
vwm_charts_t * vwm_charts_create(vwm_xserver_t *xserver)
{
	vwm_charts_t	*charts;
	Pixmap		bitmask;

	charts = calloc(1, sizeof(vwm_charts_t));
	if (!charts) {
		VWM_PERROR("unable to allocate vwm_charts_t");
		goto _err;
	}

	charts->xserver = xserver;
	charts->prev_sampling_interval = charts->sampling_interval = 0.1f;	/* default to 10Hz */

	if (!vmon_init(&charts->vmon, VMON_FLAG_2PASS, CHART_VMON_SYS_WANTS, CHART_VMON_PROC_WANTS)) {
		VWM_ERROR("unable to initialize libvmon");
		goto _err_charts;
	}

	charts->vmon.proc_ctor_cb = vmon_ctor_cb;
	charts->vmon.proc_dtor_cb = vmon_dtor_cb;
	charts->vmon.sample_cb = sample_callback;
	charts->vmon.sample_cb_arg = charts;
	gettimeofday(&charts->this_sample, NULL);

	/* get all the text and graphics stuff setup for charts */
	charts->chart_font = XLoadQueryFont(xserver->display, CHART_FIXED_FONT);
	if (!charts->chart_font) {
		VWM_ERROR("unable to load chart font \"%s\"", CHART_FIXED_FONT);
		goto _err_vmon;
	}

	/* create a GC for rendering the text using Xlib into the text chart stencils */
	bitmask = create_pixmap(charts, 1, 1, CHART_MASK_DEPTH);
	charts->text_gc = XCreateGC(xserver->display, bitmask, 0, NULL);
	XSetForeground(xserver->display, charts->text_gc, WhitePixel(xserver->display, xserver->screen_num));
	XFreePixmap(xserver->display, bitmask);

	/* create some repeating source fill pictures for drawing through the text and graph stencils */
	charts->text_fill = create_picture_fill(charts, 1, 1, 32, CPRepeat, &pa_repeat, &chart_visible_color, NULL);
	charts->shadow_fill = create_picture_fill(charts, 1, 1, 32, CPRepeat, &pa_repeat, &chart_shadow_color, NULL);

	charts->bg_fill = create_picture(charts, 1, CHART_ROW_HEIGHT, 32, CPRepeat, &pa_repeat, NULL);
	XRenderFillRectangle(xserver->display, PictOpSrc, charts->bg_fill, &chart_bg_color, 0, 0, 1, CHART_ROW_HEIGHT);
	XRenderFillRectangle(xserver->display, PictOpSrc, charts->bg_fill, &chart_div_color, 0, CHART_ROW_HEIGHT - 1, 1, 1);

	charts->snowflakes_text_fill = create_picture_fill(charts, 1, 1, 32, CPRepeat, &pa_repeat, &chart_snowflakes_visible_color, NULL);
	charts->grapha_fill = create_picture_fill(charts, 1, 1, 32, CPRepeat, &pa_repeat, &chart_grapha_color, NULL);
	charts->graphb_fill = create_picture_fill(charts, 1, 1, 32, CPRepeat, &pa_repeat, &chart_graphb_color, NULL);

	charts->finish_fill = create_picture(charts, 1, 2, 32, CPRepeat, &pa_repeat, NULL);
	XRenderFillRectangle(xserver->display, PictOpSrc, charts->finish_fill, &chart_visible_color, 0, 0, 1, 1);
	XRenderFillRectangle(xserver->display, PictOpSrc, charts->finish_fill, &chart_trans_color, 0, 1, 1, 1);

	return charts;

_err_vmon:
	vmon_destroy(&charts->vmon);

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


/* copies a row from src to dest */
static void copy_row(vwm_charts_t *charts, vwm_chart_t *chart, int src_row, Picture src, int dest_row, Picture dest)
{
	XRenderComposite(charts->xserver->display, PictOpSrc, src, None, dest,
		0, src_row * CHART_ROW_HEIGHT,		/* src */
		0, 0,					/* mask */
		0, dest_row * CHART_ROW_HEIGHT,		/* dest */
		chart->width, CHART_ROW_HEIGHT);	/* dimensions */
}


/* fills a row with the specified color */
static void fill_row(vwm_charts_t *charts, vwm_chart_t *chart, int row, Picture pic, XRenderColor *color)
{
	XRenderFillRectangle(charts->xserver->display, PictOpSrc, pic, color,
		0, row * CHART_ROW_HEIGHT,		/* dest */
		chart->width, CHART_ROW_HEIGHT);	/* dimensions */
}


/* copy what's below a given row up the specified amount within the same picture */
static void shift_below_row_up(vwm_charts_t *charts, vwm_chart_t *chart, int row, Picture pic, int rows)
{
	vwm_xserver_t	*xserver = charts->xserver;

	XRenderChangePicture(xserver->display, pic, CPRepeat, &pa_no_repeat);
	XRenderComposite(xserver->display, PictOpSrc, pic, None, pic,
		0, (rows + row) * CHART_ROW_HEIGHT,									/* src */
		0, 0,													/* mask */
		0, row * CHART_ROW_HEIGHT,										/* dest */
		chart->width, (rows + chart->heirarchy_end) * CHART_ROW_HEIGHT - (rows + row) * CHART_ROW_HEIGHT);	/* dimensions */
	XRenderChangePicture(xserver->display, pic, CPRepeat, &pa_repeat);
}


/* moves what's below a given row up above it if specified, the row becoming discarded */
static void snowflake_row(vwm_charts_t *charts, vwm_chart_t *chart, Picture pic, int copy, int row)
{
	VWM_TRACE("pid=%i chart=%p row=%i copy=%i heirarhcy_end=%i", chart->monitor->pid, chart, row, copy, chart->heirarchy_end);

	if (copy)
		copy_row(charts, chart, row, pic, 0, chart->tmp_picture);

	shift_below_row_up(charts, chart, row, pic, 1);

	if (copy) {
		copy_row(charts, chart, 0, chart->tmp_picture, chart->heirarchy_end, pic);
	} else {
		fill_row(charts, chart, chart->heirarchy_end, pic, &chart_trans_color);
	}
}

/* XXX TODO libvmon automagic children following races with explicit X client pid monitoring with different outcomes, it should be irrelevant which wins,
 *     currently the only visible difference is the snowflakes gap (heirarchy_end) varies, which is why I haven't bothered to fix it, I barely even notice.
 */


static void shift_below_row_down(vwm_charts_t *charts, vwm_chart_t *chart, int row, Picture pic, int rows)
{
	XRenderComposite(charts->xserver->display, PictOpSrc, pic, None, pic,
		0, row * CHART_ROW_HEIGHT,					/* src */
		0, 0,								/* mask */
		0, (row + rows) * CHART_ROW_HEIGHT,				/* dest */
		chart->width, chart->height - (rows + row) * CHART_ROW_HEIGHT);	/* dimensions */
}


/* shifts what's below a given row down a row, and clears the row, preparing it for populating */
static void allocate_row(vwm_charts_t *charts, vwm_chart_t *chart, Picture pic, int row)
{
	VWM_TRACE("pid=%i chart=%p row=%i", chart->monitor->pid, chart, row);

	shift_below_row_down(charts, chart, row, pic, 1);
	fill_row(charts, chart, row, pic, &chart_trans_color);
}


/* shadow a row from the text layer in the shadow layer */
static void shadow_row(vwm_charts_t *charts, vwm_chart_t *chart, int row)
{
	vwm_xserver_t *xserver = charts->xserver;

	/* the current technique for creating the shadow is to simply render the text at +1/-1 pixel offsets on both axis in translucent black */
	XRenderComposite(xserver->display, PictOpSrc, charts->shadow_fill, chart->text_picture, chart->shadow_picture,
		0, 0,
		-1, row * CHART_ROW_HEIGHT,
		0, row * CHART_ROW_HEIGHT,
		chart->visible_width, CHART_ROW_HEIGHT);

	XRenderComposite(xserver->display, PictOpOver, charts->shadow_fill, chart->text_picture, chart->shadow_picture,
		0, 0,
		0, -1 + row * CHART_ROW_HEIGHT,
		0, row * CHART_ROW_HEIGHT,
		chart->visible_width, CHART_ROW_HEIGHT);

	XRenderComposite(xserver->display, PictOpOver, charts->shadow_fill, chart->text_picture, chart->shadow_picture,
		0, 0,
		1, row * CHART_ROW_HEIGHT,
		0, row * CHART_ROW_HEIGHT,
		chart->visible_width, CHART_ROW_HEIGHT);

	XRenderComposite(xserver->display, PictOpOver, charts->shadow_fill, chart->text_picture, chart->shadow_picture,
		0, 0,
		0, 1 + row * CHART_ROW_HEIGHT,
		0, row * CHART_ROW_HEIGHT,
		chart->visible_width, CHART_ROW_HEIGHT);
}


/* simple helper to map the vmon per-proc argv array into an XTextItem array, deals with threads vs. processes and the possibility of the comm field not getting read in before the process exited... */
static void argv2xtext(vmon_proc_t *proc, XTextItem *items, int max_items, int *nr_items)
{
	int	i;
	int	nr = 0;

	if (proc->is_thread) {	/* stick the thread marker at the start of threads */
		items[0].nchars = sizeof(CHART_ISTHREAD_ARGV) - 1;
		items[0].chars = CHART_ISTHREAD_ARGV;
		items[0].delta = 4;
		items[0].font = None;
		nr++;
	}

	if (((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len) {
		items[nr].nchars = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len - 1;
		items[nr].chars = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.array;
	} else {
		/* sometimes a process is so ephemeral we don't manage to sample its comm, XXX TODO: we always have a pid, stringify it? */
		items[nr].nchars = sizeof(CHART_NOCOMM_ARGV) - 1;
		items[nr].chars = CHART_NOCOMM_ARGV;
	}
	items[nr].delta = 4;
	items[nr].font = None;
	nr++;

	for (i = 1; nr < max_items && i < ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->argc; nr++, i++) {
		items[nr].chars = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->argv[i];
		items[nr].nchars = strlen(((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->argv[i]);	/* TODO: libvmon should inform us of the length */
		items[nr].delta = 4;
		items[nr].font = None;
	}

	(*nr_items) = nr;
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


/* helper for detecting if any children/threads in the process heirarchy rooted @ proc are new/stale this sample */
static int proc_heirarchy_changed(vmon_proc_t *proc)
{
	vmon_proc_t	*child;

	if (proc->children_changed || proc->threads_changed)
		return 1;

	if (!proc->is_thread) {
		list_for_each_entry(child, &proc->threads, threads) {
			if (proc_heirarchy_changed(child))
				return 1;
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		if (proc_heirarchy_changed(child))
			return 1;
	}

	return 0;
}


/* helper for drawing the vertical bars in the graph layers */
static void draw_bars(vwm_charts_t *charts, vwm_chart_t *chart, int row, double a_fraction, double a_total, double b_fraction, double b_total)
{
	vwm_xserver_t	*xserver = charts->xserver;
	int		a_height, b_height;

	/* compute the bar heights for this sample */
	a_height = (a_fraction / a_total * (double)(CHART_ROW_HEIGHT - 1)); /* give up 1 pixel for the div */
	b_height = (b_fraction / b_total * (double)(CHART_ROW_HEIGHT - 1));

	/* round up to 1 pixel when the scaled result is a fraction less than 1,
	 * I want to at least see 1 pixel blips for the slightest cpu utilization */
	if (a_fraction && !a_height)
		a_height = 1;

	if (b_fraction && !b_height)
		b_height = 1;

	/* draw the two bars for this sample at the current phase in the graphs, note the first is ceiling-based, second floor-based */
	XRenderFillRectangle(xserver->display, PictOpSrc, chart->grapha_picture, &chart_visible_color,
		chart->phase, row * CHART_ROW_HEIGHT,	/* dst x, y */
		1, a_height);				/* dst w, h */
	XRenderFillRectangle(xserver->display, PictOpSrc, chart->graphb_picture, &chart_visible_color,
		chart->phase, row * CHART_ROW_HEIGHT + (CHART_ROW_HEIGHT - b_height) - 1,	/* dst x, y */
		1, b_height);									/* dst w, h */
}


/* helper for marking a finish line at the current phase for the specified row */
static void mark_finish(vwm_charts_t *charts, vwm_chart_t *chart, int row)
{
	vwm_xserver_t	*xserver = charts->xserver;

	XRenderComposite(xserver->display, PictOpSrc, charts->finish_fill, None, chart->grapha_picture,
			 0, 0,					/* src x, y */
			 0, 0,					/* mask x, y */
			 chart->phase, row * CHART_ROW_HEIGHT,	/* dst x, y */
			 1, CHART_ROW_HEIGHT - 1);
	XRenderComposite(xserver->display, PictOpSrc, charts->finish_fill, None, chart->graphb_picture,
			 0, 0,					/* src x, y */
			 0, 0,					/* mask x, y */
			 chart->phase, row * CHART_ROW_HEIGHT,	/* dst x, y */
			 1, CHART_ROW_HEIGHT - 1);
}


/* helper for drawing a proc's argv @ specified x offset and row on the chart */
static void print_argv(vwm_charts_t *charts, vwm_chart_t *chart, int x, int row, vmon_proc_t *proc)
{
	vwm_xserver_t	*xserver = charts->xserver;
	XTextItem	items[CHART_MAX_ARGC];
	int		nr_items;

	argv2xtext(proc, items, NELEMS(items), &nr_items);
	XDrawText(xserver->display, chart->text_pixmap, charts->text_gc,
		  x, (row + 1) * CHART_ROW_HEIGHT - 3,		/* dst x, y */
		  items, nr_items);
}


/* determine if a given process has subsequent siblings in the heirarchy */
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


/* draws proc in a row of the process heirarchy */
static void draw_heirarchy_row(vwm_charts_t *charts, vwm_chart_t *chart, vmon_proc_t *proc, int depth, int row, int heirarchy_changed)
{
	vwm_xserver_t		*xserver = charts->xserver;
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vmon_proc_t		*child;
	char			str[256];
	int			str_len, str_width;

/* process heirarchy text and accompanying per-process details like wchan/pid/state... */

	/* skip if obviously unnecessary (this can be further improved, but this makes a big difference as-is) */
	if (!chart->redraw_needed &&
	    !heirarchy_changed &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_WCHAN) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_PID) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_STATE) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_ARGV))
		return;

	/* TODO: make the columns interactively configurable @ runtime */
	if (!proc->is_new)
	/* XXX for now always clear the row, this should be capable of being optimized in the future (if the datums driving the text haven't changed...) */
		XRenderFillRectangle(xserver->display, PictOpSrc, chart->text_picture, &chart_trans_color,
			0, row * CHART_ROW_HEIGHT,		/* dst x, y */
			chart->width, CHART_ROW_HEIGHT);	/* dst w, h */

	/* put the process' wchan, state, and PID columns @ the far right */
	if (proc->is_thread || list_empty(&proc->threads)) {	/* only threads or non-threaded processes include the wchan and state */
		str_len = snpf(str, sizeof(str), "   %.*s %5i %c ",
				proc_stat->wchan.len,
				proc_stat->wchan.len == 1 && proc_stat->wchan.array[0] == '0' ? "-" : proc_stat->wchan.array,
				proc->pid,
				proc_stat->state);
	} else { /* we're a process having threads, suppress the wchan and state, as they will be displayed for the thread of same pid */
		str_len = snpf(str, sizeof(str), "  %5i   ", proc->pid);
	}

	str_width = XTextWidth(charts->chart_font, str, str_len);

	/* the process' comm label indented according to depth, followed with their respective argv's */
	print_argv(charts, chart, depth * (CHART_ROW_HEIGHT / 2), row, proc);

	/* ensure the area for the rest of the stuff is cleared, we don't put much text into thread rows so skip it for those. */
	if (!proc->is_thread)
		XRenderFillRectangle(xserver->display, PictOpSrc, chart->text_picture, &chart_trans_color,
				     chart->visible_width - str_width, row * CHART_ROW_HEIGHT,			/* dst x,y */
				     chart->width - (chart->visible_width - str_width), CHART_ROW_HEIGHT);	/* dst w,h */

	XDrawString(xserver->display, chart->text_pixmap, charts->text_gc,
		    chart->visible_width - str_width, (row + 1) * CHART_ROW_HEIGHT - 3,		/* dst x, y */
		    str, str_len);

	/* only if this process isn't the root process @ the window shall we consider all relational drawing conditions */
	if (proc != chart->monitor) {
		vmon_proc_t		*ancestor, *sibling, *last_sibling = NULL;
		int			needs_tee = 0;
		int			bar_x = 0, bar_y = (row + 1) * CHART_ROW_HEIGHT;
		int			sub;

		/* XXX: everything done in this code block only dirties _this_ process' row in the rendered chart output */

		/* walk up the ancestors until reaching chart->monitor, any ancestors we encounter which have more siblings we draw a vertical bar for */
		/* this draws the |'s in something like:  | |   |    | comm */
		for (sub = 1, ancestor = proc->parent; ancestor && ancestor != chart->monitor; ancestor = ancestor->parent) {
			sub++;
			bar_x = (depth - sub) * (CHART_ROW_HEIGHT / 2) + 4;

			/* determine if the ancestor has remaining siblings which are not stale, if so, draw a connecting bar at its depth */
			if (proc_has_subsequent_siblings(&charts->vmon, ancestor))
				XDrawLine(xserver->display, chart->text_pixmap, charts->text_gc,
					  bar_x, bar_y - CHART_ROW_HEIGHT,	/* dst x1, y1 */
					  bar_x, bar_y);			/* dst x2, y2 (vertical line) */
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
				bar_x = (depth - 1) * (CHART_ROW_HEIGHT / 2) + 4;

				/* if we're the last sibling, corner the tee by shortening the vbar */
				if (proc == last_sibling) {
					XDrawLine(xserver->display, chart->text_pixmap, charts->text_gc,
						  bar_x, bar_y - CHART_ROW_HEIGHT,	/* dst x1, y1 */
						  bar_x, bar_y - 4);			/* dst x2, y2 (vertical bar) */
				} else {
					XDrawLine(xserver->display, chart->text_pixmap, charts->text_gc,
						  bar_x, bar_y - CHART_ROW_HEIGHT,	/* dst x1, y1 */
						  bar_x, bar_y);			/* dst x2, y2 (vertical bar) */
				}

				XDrawLine(xserver->display, chart->text_pixmap, charts->text_gc,
					  bar_x, bar_y - 4,				/* dst x1, y1 */
					  bar_x + 2, bar_y - 4);			/* dst x2, y2 (horizontal bar) */

				/* terminate the outer sibling loop upon drawing the tee... */
				break;
			}
		}
	}

	shadow_row(charts, chart, row);
}


/* recursive draw function for "rest" of chart: the per-process rows (heirarchy, argv, state, wchan, pid...) */
static void draw_chart_rest(vwm_charts_t *charts, vwm_chart_t *chart, vmon_proc_t *proc, int *depth, int *row, int heirarchy_changed)
{
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;
	vmon_proc_t		*child;
	double			utime_delta, stime_delta;

	/* Some parts of this we must do on every sample to maintain coherence in the graphs, since they're incrementally kept
	 * in sync with the process heirarchy, allocating and shifting the rows as processes are created and destroyed.  Everything
	 * else we should be able to skip doing unless chart.redraw_needed or their contents changed.
	 */

	if (proc->is_stale) {
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
			draw_chart_rest(charts, chart, child, depth, row, heirarchy_changed);
			(*row)--;
		}

		if (!proc->is_thread) {
			list_for_each_entry_prev(child, &proc->threads, threads) {
				draw_chart_rest(charts, chart, child, depth, row, heirarchy_changed);
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
		snowflake_row(charts, chart, chart->grapha_picture, 1, (*row));
		snowflake_row(charts, chart, chart->graphb_picture, 1, (*row));
		snowflake_row(charts, chart, chart->text_picture, 0, (*row));
		snowflake_row(charts, chart, chart->shadow_picture, 0, (*row));
		chart->snowflakes_cnt++;

		/* stamp the name (and whatever else we include) into chart.text_picture */
		print_argv(charts, chart, 5, chart->heirarchy_end, proc);
		shadow_row(charts, chart, chart->heirarchy_end);

		chart->heirarchy_end--;

		if (in_stale_entrypoint) {
			VWM_TRACE("exited stale at chart=%p depth=%i row=%i", chart, *depth, *row);
			in_stale = 0;
		}

		return;
	} else if (proc->is_new) {
		/* what to do when a process has been introduced */
		VWM_TRACE("%i is new", proc->pid);

		allocate_row(charts, chart, chart->grapha_picture, (*row));
		allocate_row(charts, chart, chart->graphb_picture, (*row));
		allocate_row(charts, chart, chart->text_picture, (*row));
		allocate_row(charts, chart, chart->shadow_picture, (*row));

		chart->heirarchy_end++;
	}

/* CPU utilization graphs */
	/* use the generation number to avoid recomputing this stuff for callbacks recurring on the same process in the same sample */
	if (proc_ctxt->generation != charts->vmon.generation) {
		proc_ctxt->stime_delta = proc_stat->stime - proc_ctxt->last_stime;
		proc_ctxt->utime_delta = proc_stat->utime - proc_ctxt->last_utime;
		proc_ctxt->last_utime = proc_stat->utime;
		proc_ctxt->last_stime = proc_stat->stime;

		proc_ctxt->generation = charts->vmon.generation;
	}

	if (proc->is_new) {
		/* we need a minimum of two samples before we can compute a delta to plot,
		 * so we suppress that and instead mark the start of monitoring with an impossible 100% of both graph contexts, a starting line. */
		stime_delta = utime_delta = charts->total_delta;
	} else {
		stime_delta = proc_ctxt->stime_delta;
		utime_delta = proc_ctxt->utime_delta;
	}

	draw_bars(charts, chart, *row, stime_delta, charts->total_delta, utime_delta, charts->total_delta);

	draw_heirarchy_row(charts, chart, proc, *depth, *row, heirarchy_changed);

	(*row)++;

	/* recur any threads first, then any children processes */
	(*depth)++;
	if (!proc->is_thread) {	/* XXX: the threads member serves as the list head only when not a thread */
		list_for_each_entry(child, &proc->threads, threads) {
			draw_chart_rest(charts, chart, child, depth, row, heirarchy_changed);
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		draw_chart_rest(charts, chart, child, depth, row, heirarchy_changed);
	}
	(*depth)--;
}


/* convert chart sampling interval back into an integral hertz value, basically
 * open-coded ceilf(1.f / charts->sampling_interval) to avoid needing -lm.
 */
static unsigned interval_as_hz(vwm_charts_t *charts)
{
	return (1.f / charts->sampling_interval + .5f);
}


/* recursive draw function entrypoint, draws the IOWait/Idle/HZ row, then enters draw_chart_rest() */
static void draw_chart(vwm_charts_t *charts, vwm_chart_t *chart, vmon_proc_t *proc, int *depth, int *row)
{
	vwm_xserver_t		*xserver = charts->xserver;
	int			heirarchy_changed = 0;
	int			str_len, str_width;
	char			str[256];

/* CPU utilization graphs */
	/* IOWait and Idle % @ row 0 */
	draw_bars(charts, chart, *row, charts->iowait_delta, charts->total_delta, charts->idle_delta, charts->total_delta);

	/* only draw the \/\/\ and HZ if necessary */
	if (chart->redraw_needed || charts->prev_sampling_interval != charts->sampling_interval) {
		str_len = snpf(str, sizeof(str), "\\/\\/\\%s%s @ %2uHz ",
			chart->name ? " # " : "",
			chart->name ? chart->name : "",
			interval_as_hz(charts));
		XRenderFillRectangle(xserver->display, PictOpSrc, chart->text_picture, &chart_trans_color,
			0, 0,						/* dst x, y */
			chart->visible_width, CHART_ROW_HEIGHT);	/* dst w, h */
		str_width = XTextWidth(charts->chart_font, str, str_len);
		XDrawString(xserver->display, chart->text_pixmap, charts->text_gc,
			    chart->visible_width - str_width, CHART_ROW_HEIGHT - 3,		/* dst x, y */
			    str, str_len);
		shadow_row(charts, chart, 0);
	}
	(*row)++;

	if (!chart->redraw_needed)
		heirarchy_changed = proc_heirarchy_changed(proc);


	draw_chart_rest(charts, chart, proc, depth, row, heirarchy_changed);

	chart->redraw_needed = 0;

	return;
}


/* consolidated version of chart text and graph rendering, makes snowflakes integration cleaner, this always gets called regardless of the charts mode */
static void maintain_chart(vwm_charts_t *charts, vwm_chart_t *chart)
{
	vwm_xserver_t	*xserver = charts->xserver;
	int		row = 0, depth = 0;

	if (!chart->monitor || !chart->monitor->stores[VMON_STORE_PROC_STAT])
		return;

	/* TODO:
	 * A side effect of responding to window resizes in this function is there's a latency proportional to the current sample_interval.
	 * Something to fix is to resize the charts when the window resizes.
	 * However, simply resizing the charts is insufficient.  Their contents need to be redrawn in the new dimensions, this is where it
	 * gets annoying.  The current maintain/draw_chart makes assumptions about being run from the periodic vmon per-process callback.
	 * There needs to be a redraw mode added where draw_chart is just reconstructing the current state, which requires that we suppress
	 * the phase advance and in maintain_chart() and just enter draw_chart() to redraw everything for the same generation.
	 * So this probably requires some tweaking of draw_chart() as well as maintain_chart().  I want to be able tocall mainta_charts()
	 * from anywhere, and have it detect if it's being called on the same generation or if the generation has advanced.
	 * For now, the monitors will just be a little latent in window resizes which is pretty harmless artifact.
	 */

	chart->phase += (chart->width - 1);  /* simply change this to .phase++ to scroll the other direction */
	chart->phase %= chart->width;
	XRenderFillRectangle(xserver->display, PictOpSrc, chart->grapha_picture, &chart_trans_color, chart->phase, 0, 1, chart->height);
	XRenderFillRectangle(xserver->display, PictOpSrc, chart->graphb_picture, &chart_trans_color, chart->phase, 0, 1, chart->height);

	/* recursively draw the monitored processes to the chart */
	draw_chart(charts, chart, chart->monitor, &depth, &row);
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
	maintain_chart(charts, chart);

	/* XXX TODO: we used to mark repaint as being needed if this chart's window was mapped, but
	 * since extricating charts from windows that's no longer convenient, and repaint is
	 * always performed after a sample.  Make sure the repainting isn't costly when nothing
	 * charted is mapped (the case that code optimized)
	 */
}


/* return the composed height of the chart */
static int vwm_chart_composed_height(vwm_charts_t *charts, vwm_chart_t *chart)
{
	int	snowflakes = chart->snowflakes_cnt ? 1 + chart->snowflakes_cnt : 0; /* don't include the separator row if there are no snowflakes */

	return MIN((chart->heirarchy_end + snowflakes) * CHART_ROW_HEIGHT, chart->visible_height);
}


/* reset snowflakes on the specified chart */
void vwm_chart_reset_snowflakes(vwm_charts_t *charts, vwm_chart_t *chart)
{
	if (chart->snowflakes_cnt) {
		chart->snowflakes_cnt = 0;
		chart->redraw_needed = 1;
	}
}


static void free_chart_pictures(vwm_charts_t *charts, vwm_chart_t *chart)
{
	vwm_xserver_t	*xserver = charts->xserver;

	XRenderFreePicture(xserver->display, chart->grapha_picture);
	XRenderFreePicture(xserver->display, chart->graphb_picture);
	XRenderFreePicture(xserver->display, chart->tmp_picture);
	XRenderFreePicture(xserver->display, chart->text_picture);
	XFreePixmap(xserver->display, chart->text_pixmap);
	XRenderFreePicture(xserver->display, chart->shadow_picture);
	XRenderFreePicture(xserver->display, chart->picture);

}


static void copy_chart_pictures(vwm_charts_t *charts, vwm_chart_t *src, vwm_chart_t *dest)
{
	vwm_xserver_t	*xserver = charts->xserver;

	/* XXX: note the graph pictures are copied from their current phase in the x dimension */
	XRenderComposite(xserver->display, PictOpSrc, src->grapha_picture, None, dest->grapha_picture,
		src->phase, 0,		/* src x, y */
		0, 0,			/* mask x, y */
		dest->phase, 0,		/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->graphb_picture, None, dest->graphb_picture,
		src->phase, 0,		/* src x, y */
		0, 0,			/* mask x, y */
		dest->phase, 0,		/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->text_picture, None, dest->text_picture,
		0, 0,			/* src x, y */
		0, 0,			/* mask x, y */
		0, 0,			/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->shadow_picture, None, dest->shadow_picture,
		0, 0,			/* src x, y */
		0, 0,			/* mask x, y */
		0, 0,			/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->picture, None, dest->picture,
		0, 0,			/* src x, y */
		0, 0,			/* mask x, y */
		0, 0,			/* dest x, y */
		src->width, src->height);
}


/* (re)size the specified chart's visible dimensions */
int vwm_chart_set_visible_size(vwm_charts_t *charts, vwm_chart_t *chart, int width, int height)
{
	if (width != chart->visible_width || height != chart->visible_height)
		chart->redraw_needed = 1;

	/* TODO error handling: if a create failed but we had an chart, free whatever we created and leave it be, succeed.
	 * if none existed it's a hard error and we must propagate it. */

	/* if larger than the charts currently are, enlarge them */
	if (width > chart->width || height > chart->height) {
		vwm_chart_t	existing = *chart;

		chart->width = MAX(chart->width, MAX(width, CHART_GRAPH_MIN_WIDTH));
		chart->height = MAX(chart->height, MAX(height, CHART_GRAPH_MIN_HEIGHT));

		chart->grapha_picture = create_picture_fill(charts, chart->width, chart->height, CHART_MASK_DEPTH, CPRepeat, &pa_repeat, &chart_trans_color, NULL);
		chart->graphb_picture = create_picture_fill(charts, chart->width, chart->height, CHART_MASK_DEPTH, CPRepeat, &pa_repeat, &chart_trans_color, NULL);
		chart->tmp_picture = create_picture(charts, chart->width, CHART_ROW_HEIGHT, CHART_MASK_DEPTH, 0, NULL, NULL);

		/* keep the text_pixmap reference around for XDrawText usage */
		chart->text_picture = create_picture_fill(charts, chart->width, chart->height, CHART_MASK_DEPTH, 0, NULL, &chart_trans_color, &chart->text_pixmap);

		chart->shadow_picture = create_picture_fill(charts, chart->width, chart->height, CHART_MASK_DEPTH, 0, NULL, &chart_trans_color, NULL);
		chart->picture = create_picture(charts, chart->width, chart->height, 32, 0, NULL, NULL);

		if (existing.width) {
			copy_chart_pictures(charts, &existing, chart);
			free_chart_pictures(charts, &existing);
		}
	}

	chart->visible_width = width;
	chart->visible_height = height;

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

	/* add the client process to the monitoring heirarchy */
	/* XXX note libvmon here maintains a unique callback for each unique callback+xwin pair, so multi-window processes work */
	chart->monitor = vmon_proc_monitor(&charts->vmon, NULL, pid, VMON_WANT_PROC_INHERIT, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, chart);
	if (!chart->monitor) {
		VWM_ERROR("Unable to establish proc monitor");
		goto _err_free;
	}

	 /* FIXME: count_rows() isn't returning the right count sometimes (off by ~1), it seems to be related to racing with the automatic child monitoring */
	 /* the result is an extra row sometimes appearing below the process heirarchy */
	chart->heirarchy_end = 1 + count_rows(chart->monitor);
	chart->gen_last_composed = -1;

	if (!vwm_chart_set_visible_size(charts, chart, width, height)) {
		VWM_ERROR("Unable to set initial chart size");
		goto _err_unmonitor;
	}

	return chart;

_err_unmonitor:
	vmon_proc_unmonitor(&charts->vmon, chart->monitor, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, chart);

_err_free:
	free(chart->name);
	free(chart);
_err:
	return NULL;
}


/* stop monitoring and destroy the supplied chart */
void vwm_chart_destroy(vwm_charts_t *charts, vwm_chart_t *chart)
{
	vmon_proc_unmonitor(&charts->vmon, chart->monitor, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, chart);
	free_chart_pictures(charts, chart);
	free(chart);
}


/* this composes the maintained chart into the base chart picture, this gets called from paint_all() on every repaint of xwin */
/* we noop the call if the gen_last_composed and monitor->proc.generation numbers match, indicating there's nothing new to compose. */
void vwm_chart_compose(vwm_charts_t *charts, vwm_chart_t *chart, XserverRegion *res_damaged_region)
{
	vwm_xserver_t	*xserver = charts->xserver;
	int		height;

	if (!chart->width || !chart->height)
		return;

	if (chart->gen_last_composed == chart->monitor->generation)
		return; /* noop if no sampling occurred since last compose */

	chart->gen_last_composed = chart->monitor->generation; /* remember this generation */

	//VWM_TRACE("composing %p", chart);

	height = vwm_chart_composed_height(charts, chart);

	/* fill the chart picture with the background */
	XRenderComposite(xserver->display, PictOpSrc, charts->bg_fill, None, chart->picture,
		0, 0,
		0, 0,
		0, 0,
		chart->visible_width, height);

	/* draw the graphs into the chart through the stencils being maintained by the sample callbacks */
	XRenderComposite(xserver->display, PictOpOver, charts->grapha_fill, chart->grapha_picture, chart->picture,
		0, 0,
		chart->phase, 0,
		0, 0,
		chart->visible_width, height);
	XRenderComposite(xserver->display, PictOpOver, charts->graphb_fill, chart->graphb_picture, chart->picture,
		0, 0,
		chart->phase, 0,
		0, 0,
		chart->visible_width, height);

	/* draw the shadow into the chart picture using a translucent black source drawn through the shadow mask */
	XRenderComposite(xserver->display, PictOpOver, charts->shadow_fill, chart->shadow_picture, chart->picture,
		0, 0,
		0, 0,
		0, 0,
		chart->visible_width, height);

	/* render chart text into the chart picture using a white source drawn through the chart text as a mask, on top of everything */
	XRenderComposite(xserver->display, PictOpOver, charts->text_fill, chart->text_picture, chart->picture,
		0, 0,
		0, 0,
		0, 0,
		chart->visible_width, (chart->heirarchy_end * CHART_ROW_HEIGHT));

	XRenderComposite(xserver->display, PictOpOver, charts->snowflakes_text_fill, chart->text_picture, chart->picture,
		0, 0,
		0, chart->heirarchy_end * CHART_ROW_HEIGHT,
		0, chart->heirarchy_end * CHART_ROW_HEIGHT,
		chart->visible_width, height - (chart->heirarchy_end * CHART_ROW_HEIGHT));

	/* damage the window to ensure the updated chart is drawn (TODO: this can be done more selectively/efficiently) */
	if (res_damaged_region) {
		XRectangle	damage = {};

		damage.width = chart->visible_width;
		damage.height = chart->visible_height;

		*res_damaged_region = XFixesCreateRegion(xserver->display, &damage, 1);
	}
}


/* render the chart into a picture at the specified coordinates and dimensions */
void vwm_chart_render(vwm_charts_t *charts, vwm_chart_t *chart, int op, Picture dest, int x, int y, int width, int height)
{
	vwm_xserver_t	*xserver = charts->xserver;

	if (!chart->width || !chart->height)
		return;

	/* draw the monitoring chart atop dest, note we stay within the window borders here. */
	XRenderComposite(xserver->display, op, chart->picture, None, dest,
			 0, 0, 0, 0,									/* src x,y, maxk x, y */
			 x,										/* dst x */
			 y,										/* dst y */
			 width, MIN(vwm_chart_composed_height(charts, chart), height) /* FIXME */);	/* w, h */
}


/* render the chart into a newly allocated pixmap, intended for snapshotting purposes */
void vwm_chart_render_as_pixmap(vwm_charts_t *charts, vwm_chart_t *chart, const XRenderColor *bg_color, Pixmap *res_pixmap)
{
	static const XRenderColor	blackness = { 0x0000, 0x0000, 0x0000, 0xFFFF};
	Picture			 	dest;

	assert(charts);
	assert(chart);
	assert(res_pixmap);

	if (!bg_color)
		bg_color = &blackness;

	dest = create_picture_fill(charts, chart->width, chart->height, 32, 0, NULL, bg_color, res_pixmap);
	vwm_chart_render(charts, chart, PictOpOver, dest, 0, 0, chart->width, chart->height);
	XRenderFreePicture(charts->xserver->display, dest);
}


void vwm_chart_render_as_ximage(vwm_charts_t *charts, vwm_chart_t *chart, const XRenderColor *bg_color, XImage **res_ximage)
{
	Pixmap	dest_pixmap;

	assert(charts);
	assert(chart);
	assert(res_ximage);

	vwm_chart_render_as_pixmap(charts, chart, bg_color, &dest_pixmap);
	*res_ximage = XGetImage(charts->xserver->display,
				dest_pixmap,
				0,
				0,
				chart->width,
				chart->height,
				AllPlanes,
				ZPixmap);

	XFreePixmap(charts->xserver->display, dest_pixmap);
}


/* increase the sample rate relative to current using the table of intervals */
void vwm_charts_rate_increase(vwm_charts_t *charts)
{
	int	i;

	assert(charts);

	for (i = 0; i < NELEMS(sampling_intervals); i++) {
		if (sampling_intervals[i] < charts->sampling_interval) {
			charts->sampling_interval = sampling_intervals[i];
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
		if (sampling_intervals[i] > charts->sampling_interval) {
			charts->sampling_interval = sampling_intervals[i];
			break;
		}
	}
}


/* set an arbitrary sample rate rather than using one of the presets, 0 to pause */
void vwm_charts_rate_set(vwm_charts_t *charts, unsigned hertz)
{
	assert(charts);

	/* XXX: note floating point divide by 0 simply results in infinity */
	charts->sampling_interval = 1.0f / (float)hertz;
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


/* update the charts if necessary, return if updating occurred, and duration before another update needed in *desired_delay */
int vwm_charts_update(vwm_charts_t *charts, int *desired_delay)
{
	float	this_delta = 0.0f;
	int	ret = 0;

	gettimeofday(&charts->maybe_sample, NULL);
	if ((charts->sampling_interval == INFINITY && !charts->sampling_paused) || /* XXX this is kind of a kludge to get the 0 Hz indicator drawn before pausing */
	    (charts->sampling_interval != INFINITY && ((this_delta = delta(&charts->maybe_sample, &charts->this_sample)) >= charts->sampling_interval))) {
		vmon_sys_stat_t	*sys_stat;

		/* automatically lower the sample rate if we can't keep up with the current sample rate */
		if (charts->sampling_interval < INFINITY &&
		    charts->sampling_interval <= charts->prev_sampling_interval &&
		    this_delta >= (charts->sampling_interval * 1.5)) {

			/* require > 1 contiguous drops before lowering the rate, tolerates spurious one-off stalls */
			if (++charts->contiguous_drops > 2)
				vwm_charts_rate_decrease(charts);
		} else {
			charts->contiguous_drops = 0;
		}

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

		charts->sampling_paused = (charts->sampling_interval == INFINITY);
		charts->prev_sampling_interval = charts->sampling_interval;
	}

	/* TODO: make some effort to compute how long to sleep, but this is perfectly fine for now. */
	*desired_delay = charts->sampling_interval == INFINITY ? -1 : charts->sampling_interval * 300.0;

	return ret;
}
