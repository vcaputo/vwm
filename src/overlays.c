/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2017  Vito Caputo - <vcaputo@gnugeneration.com>
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
#include <stdlib.h>
#include <sys/time.h>

#include "composite.h"
#include "libvmon/vmon.h"
#include "list.h"
#include "overlays.h"
#include "vwm.h"
#include "xwindow.h"

#define OVERLAY_MASK_DEPTH		8					/* XXX: 1 would save memory, but Xorg isn't good at it */
#define OVERLAY_FIXED_FONT		"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1"
#define OVERLAY_ROW_HEIGHT		15					/* this should always be larger than the font height */
#define OVERLAY_GRAPH_MIN_WIDTH		200					/* always create graphs at least this large */
#define OVERLAY_GRAPH_MIN_HEIGHT	(4 * OVERLAY_ROW_HEIGHT)
#define OVERLAY_ISTHREAD_ARGV		"~"					/* use this string to mark threads in the argv field */
#define OVERLAY_NOCOMM_ARGV		"#missed it!"				/* use this string to substitute the command when missing in argv field */
#define OVERLAY_MAX_ARGC		512					/* this is a huge amount */
#define OVERLAY_VMON_PROC_WANTS		(VMON_WANT_PROC_STAT | VMON_WANT_PROC_FOLLOW_CHILDREN | VMON_WANT_PROC_FOLLOW_THREADS)
#define OVERLAY_VMON_SYS_WANTS		(VMON_WANT_SYS_STAT)

/* the global overlays state, supplied to vwm_overlay_create() which keeps a reference for future use. */
typedef struct _vwm_overlays_t {
	vwm_xserver_t				*xserver;	/* xserver supplied to vwm_overlays_init() */

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
	XFontStruct				*overlay_font;
	GC					text_gc;
	Picture					shadow_fill,
						text_fill,
						bg_fill,
						snowflakes_text_fill,
						grapha_fill,
						graphb_fill,
						finish_fill;
} vwm_overlays_t;

/* everything needed by the per-window overlay's context */
typedef struct _vwm_overlay_t {
	vmon_proc_t	*monitor;		/* vmon process monitor handle */
	Pixmap		text_pixmap;		/* pixmap for overlayed text (kept around for XDrawText usage) */
	Picture		text_picture;		/* picture representation of text_pixmap */
	Picture		shadow_picture;		/* text shadow layer */
	Picture		grapha_picture;		/* graph A layer */
	Picture		graphb_picture;		/* graph B layer */
	Picture		tmp_picture;		/* 1 row worth of temporary picture space */
	Picture		picture;		/* overlay picture derived from the pixmap, for render compositing */
	int		width;			/* current width of the overlay */
	int		height;			/* current height of the overlay */
	int		visible_width;		/* currently visible width of the overlay */
	int		visible_height;		/* currently visible height of the overlay */
	int		phase;			/* current position within the (horizontally scrolling) graphs */
	int		heirarchy_end;		/* row where the process heirarchy currently ends */
	int		snowflakes_cnt;		/* count of snowflaked rows (reset to zero to truncate snowflakes display) */
	int		gen_last_composed;	/* the last composed vmon generation */
	int		redraw_needed;		/* if a redraw is required (like when the window is resized...) */
} vwm_overlay_t;

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

static XRenderColor		overlay_visible_color = { 0xffff, 0xffff, 0xffff, 0xffff },
				overlay_shadow_color = { 0x0000, 0x0000, 0x0000, 0x8800},
				overlay_bg_color = { 0x0, 0x1000, 0x0, 0x9000},
				overlay_div_color = { 0x2000, 0x3000, 0x2000, 0x9000},
				overlay_snowflakes_visible_color = { 0xd000, 0xd000, 0xd000, 0x8000 },
				overlay_trans_color = {0x00, 0x00, 0x00, 0x00},
				overlay_grapha_color = { 0xff00, 0x0000, 0x0000, 0x3000 },	/* ~red */
				overlay_graphb_color = { 0x0000, 0xffff, 0xffff, 0x3000 };	/* ~cyan */
static XRenderPictureAttributes	pa_repeat = { .repeat = 1 };
static XRenderPictureAttributes	pa_no_repeat = { .repeat = 0 };


/* this callback gets invoked at sample time once "per sys" */
static void sample_callback(vmon_t *vmon, void *arg)
{
	vwm_overlays_t	*overlays = arg;
	vmon_sys_stat_t	*sys_stat = vmon->stores[VMON_STORE_SYS_STAT];

	overlays->this_total =	sys_stat->user + sys_stat->nice + sys_stat->system +
					sys_stat->idle + sys_stat->iowait + sys_stat->irq +
					sys_stat->softirq + sys_stat->steal + sys_stat->guest;

	overlays->total_delta = overlays->this_total - overlays->last_total;
	overlays->idle_delta = sys_stat->idle - overlays->last_idle;
	overlays->iowait_delta = sys_stat->iowait - overlays->last_iowait;
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
static Pixmap create_pixmap(vwm_overlays_t *overlays, unsigned width, unsigned height, unsigned depth)
{
	vwm_xserver_t	*xserver = overlays->xserver;

	return XCreatePixmap(xserver->display, XSERVER_XROOT(xserver), width, height, depth);
}


/* convenience helper for creating a picture, supply res_pixmap to keep a reference to the pixmap drawable. */
static Picture create_picture(vwm_overlays_t *overlays, unsigned width, unsigned height, unsigned depth, unsigned long attr_mask, XRenderPictureAttributes *attr, Pixmap *res_pixmap)
{
	vwm_xserver_t	*xserver = overlays->xserver;
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

	pixmap = create_pixmap(overlays, width, height, depth);
	picture = XRenderCreatePicture(xserver->display, pixmap, XRenderFindStandardFormat(xserver->display, format), attr_mask, attr);

	if (res_pixmap) {
		*res_pixmap = pixmap;
	} else {
		XFreePixmap(xserver->display, pixmap);
	}

	return picture;
}


/* convenience helper for creating a filled picture, supply res_pixmap to keep a reference to the pixmap drawable. */
static Picture create_picture_fill(vwm_overlays_t *overlays, unsigned width, unsigned height, unsigned depth, unsigned long attrs_mask, XRenderPictureAttributes *attrs, XRenderColor *color, Pixmap *res_pixmap)
{
	vwm_xserver_t	*xserver = overlays->xserver;
	Picture		picture;

	picture = create_picture(overlays, width, height, depth, attrs_mask, attrs, res_pixmap);
	XRenderFillRectangle(xserver->display, PictOpSrc, picture, color, 0, 0, width, height);

	return picture;
}


/* initialize overlays system */
vwm_overlays_t * vwm_overlays_create(vwm_xserver_t *xserver)
{
	vwm_overlays_t	*overlays;
	Pixmap		bitmask;

	overlays = calloc(1, sizeof(vwm_overlays_t));
	if (!overlays) {
		VWM_PERROR("unable to allocate vwm_overlays_t");
		goto _err;
	}

	overlays->xserver = xserver;
	overlays->prev_sampling_interval = overlays->sampling_interval = 0.1f;	/* default to 10Hz */

	if (!vmon_init(&overlays->vmon, VMON_FLAG_2PASS, OVERLAY_VMON_SYS_WANTS, OVERLAY_VMON_PROC_WANTS)) {
		VWM_ERROR("unable to initialize libvmon");
		goto _err_overlays;
	}

	overlays->vmon.proc_ctor_cb = vmon_ctor_cb;
	overlays->vmon.proc_dtor_cb = vmon_dtor_cb;
	overlays->vmon.sample_cb = sample_callback;
	overlays->vmon.sample_cb_arg = overlays;
	gettimeofday(&overlays->this_sample, NULL);

	/* get all the text and graphics stuff setup for overlays */
	overlays->overlay_font = XLoadQueryFont(xserver->display, OVERLAY_FIXED_FONT);
	if (!overlays->overlay_font) {
		VWM_ERROR("unable to load overlay font \"%s\"", OVERLAY_FIXED_FONT);
		goto _err_vmon;
	}

	/* create a GC for rendering the text using Xlib into the text overlay stencils */
	bitmask = create_pixmap(overlays, 1, 1, OVERLAY_MASK_DEPTH);
	overlays->text_gc = XCreateGC(xserver->display, bitmask, 0, NULL);
	XSetForeground(xserver->display, overlays->text_gc, WhitePixel(xserver->display, xserver->screen_num));
	XFreePixmap(xserver->display, bitmask);

	/* create some repeating source fill pictures for drawing through the text and graph stencils */
	overlays->text_fill = create_picture_fill(overlays, 1, 1, 32, CPRepeat, &pa_repeat, &overlay_visible_color, NULL);
	overlays->shadow_fill = create_picture_fill(overlays, 1, 1, 32, CPRepeat, &pa_repeat, &overlay_shadow_color, NULL);

	overlays->bg_fill = create_picture(overlays, 1, OVERLAY_ROW_HEIGHT, 32, CPRepeat, &pa_repeat, NULL);
	XRenderFillRectangle(xserver->display, PictOpSrc, overlays->bg_fill, &overlay_bg_color, 0, 0, 1, OVERLAY_ROW_HEIGHT);
	XRenderFillRectangle(xserver->display, PictOpSrc, overlays->bg_fill, &overlay_div_color, 0, OVERLAY_ROW_HEIGHT - 1, 1, 1);

	overlays->snowflakes_text_fill = create_picture_fill(overlays, 1, 1, 32, CPRepeat, &pa_repeat, &overlay_snowflakes_visible_color, NULL);
	overlays->grapha_fill = create_picture_fill(overlays, 1, 1, 32, CPRepeat, &pa_repeat, &overlay_grapha_color, NULL);
	overlays->graphb_fill = create_picture_fill(overlays, 1, 1, 32, CPRepeat, &pa_repeat, &overlay_graphb_color, NULL);

	overlays->finish_fill = create_picture(overlays, 1, 2, 32, CPRepeat, &pa_repeat, NULL);
	XRenderFillRectangle(xserver->display, PictOpSrc, overlays->finish_fill, &overlay_visible_color, 0, 0, 1, 1);
	XRenderFillRectangle(xserver->display, PictOpSrc, overlays->finish_fill, &overlay_trans_color, 0, 1, 1, 1);

	return overlays;

_err_vmon:
	vmon_destroy(&overlays->vmon);

_err_overlays:
	free(overlays);

_err:
	return NULL;
}


/* teardown overlays system */
void vwm_overlays_destroy(vwm_overlays_t *overlays)
{
	/* TODO: free rest of stuff.. */
	free(overlays);
}


/* copies a row from src to dest */
static void copy_row(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int src_row, Picture src, int dest_row, Picture dest)
{
	XRenderComposite(overlays->xserver->display, PictOpSrc, src, None, dest,
		0, src_row * OVERLAY_ROW_HEIGHT,	/* src */
		0, 0,					/* mask */
		0, dest_row * OVERLAY_ROW_HEIGHT,	/* dest */
		overlay->width, OVERLAY_ROW_HEIGHT);	/* dimensions */
}


/* fills a row with the specified color */
static void fill_row(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int row, Picture pic, XRenderColor *color)
{
	XRenderFillRectangle(overlays->xserver->display, PictOpSrc, pic, color,
		0, row * OVERLAY_ROW_HEIGHT,		/* dest */
		overlay->width, OVERLAY_ROW_HEIGHT);	/* dimensions */
}


/* copy what's below a given row up the specified amount within the same picture */
static void shift_below_row_up(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int row, Picture pic, int rows)
{
	vwm_xserver_t	*xserver = overlays->xserver;

	XRenderChangePicture(xserver->display, pic, CPRepeat, &pa_no_repeat);
	XRenderComposite(xserver->display, PictOpSrc, pic, None, pic,
		0, (rows + row) * OVERLAY_ROW_HEIGHT,										/* src */
		0, 0,														/* mask */
		0, row * OVERLAY_ROW_HEIGHT,											/* dest */
		overlay->width, (rows + overlay->heirarchy_end) * OVERLAY_ROW_HEIGHT - (rows + row) * OVERLAY_ROW_HEIGHT);	/* dimensions */
	XRenderChangePicture(xserver->display, pic, CPRepeat, &pa_repeat);
}


/* moves what's below a given row up above it if specified, the row becoming discarded */
static void snowflake_row(vwm_overlays_t *overlays, vwm_overlay_t *overlay, Picture pic, int copy, int row)
{
	VWM_TRACE("pid=%i overlay=%p row=%i copy=%i heirarhcy_end=%i", overlay->monitor->pid, overlay, row, copy, overlay->heirarchy_end);

	if (copy)
		copy_row(overlays, overlay, row, pic, 0, overlay->tmp_picture);

	shift_below_row_up(overlays, overlay, row, pic, 1);

	if (copy) {
		copy_row(overlays, overlay, 0, overlay->tmp_picture, overlay->heirarchy_end, pic);
	} else {
		fill_row(overlays, overlay, overlay->heirarchy_end, pic, &overlay_trans_color);
	}
}

/* XXX TODO libvmon automagic children following races with explicit X client pid monitoring with different outcomes, it should be irrelevant which wins,
 *     currently the only visible difference is the snowflakes gap (heirarchy_end) varies, which is why I haven't bothered to fix it, I barely even notice.
 */


static void shift_below_row_down(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int row, Picture pic, int rows)
{
	XRenderComposite(overlays->xserver->display, PictOpSrc, pic, None, pic,
		0, row * OVERLAY_ROW_HEIGHT,						/* src */
		0, 0,									/* mask */
		0, (row + rows) * OVERLAY_ROW_HEIGHT,					/* dest */
		overlay->width, overlay->height - (rows + row) * OVERLAY_ROW_HEIGHT);	/* dimensions */
}


/* shifts what's below a given row down a row, and clears the row, preparing it for populating */
static void allocate_row(vwm_overlays_t *overlays, vwm_overlay_t *overlay, Picture pic, int row)
{
	VWM_TRACE("pid=%i overlay=%p row=%i", overlay->monitor->pid, overlay, row);

	shift_below_row_down(overlays, overlay, row, pic, 1);
	fill_row(overlays, overlay, row, pic, &overlay_trans_color);
}


/* shadow a row from the text layer in the shadow layer */
static void shadow_row(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int row)
{
	vwm_xserver_t *xserver = overlays->xserver;

	/* the current technique for creating the shadow is to simply render the text at +1/-1 pixel offsets on both axis in translucent black */
	XRenderComposite(xserver->display, PictOpSrc, overlays->shadow_fill, overlay->text_picture, overlay->shadow_picture,
		0, 0,
		-1, row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		overlay->visible_width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(xserver->display, PictOpOver, overlays->shadow_fill, overlay->text_picture, overlay->shadow_picture,
		0, 0,
		0, -1 + row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		overlay->visible_width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(xserver->display, PictOpOver, overlays->shadow_fill, overlay->text_picture, overlay->shadow_picture,
		0, 0,
		1, row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		overlay->visible_width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(xserver->display, PictOpOver, overlays->shadow_fill, overlay->text_picture, overlay->shadow_picture,
		0, 0,
		0, 1 + row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		overlay->visible_width, OVERLAY_ROW_HEIGHT);
}


/* simple helper to map the vmon per-proc argv array into an XTextItem array, deals with threads vs. processes and the possibility of the comm field not getting read in before the process exited... */
static void argv2xtext(vmon_proc_t *proc, XTextItem *items, int max_items, int *nr_items)
{
	int	i;
	int	nr = 0;

	if (proc->is_thread) {	/* stick the thread marker at the start of threads */
		items[0].nchars = sizeof(OVERLAY_ISTHREAD_ARGV) - 1;
		items[0].chars = OVERLAY_ISTHREAD_ARGV;
		items[0].delta = 4;
		items[0].font = None;
		nr++;
	}

	if (((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len) {
		items[nr].nchars = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len - 1;
		items[nr].chars = ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.array;
	} else {
		/* sometimes a process is so ephemeral we don't manage to sample its comm, XXX TODO: we always have a pid, stringify it? */
		items[nr].nchars = sizeof(OVERLAY_NOCOMM_ARGV) - 1;
		items[nr].chars = OVERLAY_NOCOMM_ARGV;
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
static void draw_bars(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int row, double a_fraction, double a_total, double b_fraction, double b_total)
{
	vwm_xserver_t	*xserver = overlays->xserver;
	int		a_height, b_height;

	/* compute the bar heights for this sample */
	a_height = (a_fraction / a_total * (double)(OVERLAY_ROW_HEIGHT - 1)); /* give up 1 pixel for the div */
	b_height = (b_fraction / b_total * (double)(OVERLAY_ROW_HEIGHT - 1));

	/* round up to 1 pixel when the scaled result is a fraction less than 1,
	 * I want to at least see 1 pixel blips for the slightest cpu utilization */
	if (a_fraction && !a_height)
		a_height = 1;

	if (b_fraction && !b_height)
		b_height = 1;

	/* draw the two bars for this sample at the current phase in the graphs, note the first is ceiling-based, second floor-based */
	XRenderFillRectangle(xserver->display, PictOpSrc, overlay->grapha_picture, &overlay_visible_color,
		overlay->phase, row * OVERLAY_ROW_HEIGHT,	/* dst x, y */
		1, a_height);					/* dst w, h */
	XRenderFillRectangle(xserver->display, PictOpSrc, overlay->graphb_picture, &overlay_visible_color,
		overlay->phase, row * OVERLAY_ROW_HEIGHT + (OVERLAY_ROW_HEIGHT - b_height) - 1,	/* dst x, y */
		1, b_height);									/* dst w, h */
}


/* helper for marking a finish line at the current phase for the specified row */
static void mark_finish(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int row)
{
	vwm_xserver_t	*xserver = overlays->xserver;

	XRenderComposite(xserver->display, PictOpSrc, overlays->finish_fill, None, overlay->grapha_picture,
			 0, 0,						/* src x, y */
			 0, 0,						/* mask x, y */
			 overlay->phase, row * OVERLAY_ROW_HEIGHT,	/* dst x, y */
			 1, OVERLAY_ROW_HEIGHT - 1);
	XRenderComposite(xserver->display, PictOpSrc, overlays->finish_fill, None, overlay->graphb_picture,
			 0, 0,						/* src x, y */
			 0, 0,						/* mask x, y */
			 overlay->phase, row * OVERLAY_ROW_HEIGHT,	/* dst x, y */
			 1, OVERLAY_ROW_HEIGHT - 1);
}


/* helper for drawing a proc's argv @ specified x offset and row on the overlay */
static void print_argv(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int x, int row, vmon_proc_t *proc)
{
	vwm_xserver_t	*xserver = overlays->xserver;
	XTextItem	items[OVERLAY_MAX_ARGC];
	int		nr_items;

	argv2xtext(proc, items, NELEMS(items), &nr_items);
	XDrawText(xserver->display, overlay->text_pixmap, overlays->text_gc,
		  x, (row + 1) * OVERLAY_ROW_HEIGHT - 3,		/* dst x, y */
		  items, nr_items);
}


/* draws proc in a row of the process heirarchy */
static void draw_heirarchy_row(vwm_overlays_t *overlays, vwm_overlay_t *overlay, vmon_proc_t *proc, int depth, int row, int heirarchy_changed)
{
	vwm_xserver_t		*xserver = overlays->xserver;
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vmon_proc_t		*child;
	char			str[256];
	int			str_len, str_width;

/* process heirarchy text and accompanying per-process details like wchan/pid/state... */

	/* skip if obviously unnecessary (this can be further improved, but this makes a big difference as-is) */
	if (!overlay->redraw_needed &&
	    !heirarchy_changed &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_WCHAN) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_PID) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_STATE) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_ARGV))
		return;

	/* TODO: make the columns interactively configurable @ runtime */
	if (!proc->is_new)
	/* XXX for now always clear the row, this should be capable of being optimized in the future (if the datums driving the text haven't changed...) */
		XRenderFillRectangle(xserver->display, PictOpSrc, overlay->text_picture, &overlay_trans_color,
			0, row * OVERLAY_ROW_HEIGHT,		/* dst x, y */
			overlay->width, OVERLAY_ROW_HEIGHT);	/* dst w, h */

	/* put the process' wchan, state, and PID columns @ the far right */
	if (proc->is_thread || list_empty(&proc->threads)) {	/* only threads or non-threaded processes include the wchan and state */
		snprintf(str, sizeof(str), "   %.*s %5i %c %n",
			proc_stat->wchan.len,
			proc_stat->wchan.len == 1 && proc_stat->wchan.array[0] == '0' ? "-" : proc_stat->wchan.array,
			proc->pid,
			proc_stat->state,
			&str_len);
	} else { /* we're a process having threads, suppress the wchan and state, as they will be displayed for the thread of same pid */
		snprintf(str, sizeof(str), "  %5i   %n", proc->pid, &str_len);
	}
	str_width = XTextWidth(overlays->overlay_font, str, str_len);

	/* the process' comm label indented according to depth, followed with their respective argv's */
	print_argv(overlays, overlay, depth * (OVERLAY_ROW_HEIGHT / 2), row, proc);

	/* ensure the area for the rest of the stuff is cleared, we don't put much text into thread rows so skip it for those. */
	if (!proc->is_thread)
		XRenderFillRectangle(xserver->display, PictOpSrc, overlay->text_picture, &overlay_trans_color,
				     overlay->visible_width - str_width, row * OVERLAY_ROW_HEIGHT,			/* dst x,y */
				     overlay->width - (overlay->visible_width - str_width), OVERLAY_ROW_HEIGHT);	/* dst w,h */

	XDrawString(xserver->display, overlay->text_pixmap, overlays->text_gc,
		    overlay->visible_width - str_width, (row + 1) * OVERLAY_ROW_HEIGHT - 3,		/* dst x, y */
		    str, str_len);

	/* only if this process isn't the root process @ the window shall we consider all relational drawing conditions */
	if (proc != overlay->monitor) {
		vmon_proc_t		*ancestor, *sibling, *last_sibling = NULL;
		struct list_head	*rem;
		int			needs_tee = 0;
		int			bar_x = 0, bar_y = (row + 1) * OVERLAY_ROW_HEIGHT;
		int			sub;

		/* XXX: everything done in this code block only dirties _this_ process' row in the rendered overlay output */

		/* walk up the ancestors until reaching overlay->monitor, any ancestors we encounter which have more siblings we draw a vertical bar for */
		/* this draws the |'s in something like:  | |   |    | comm */
		for (sub = 1, ancestor = proc->parent; ancestor && ancestor != overlay->monitor; ancestor = ancestor->parent) {
			sub++;
			bar_x = (depth - sub) * (OVERLAY_ROW_HEIGHT / 2) + 4;

			/* determine if the ancestor has remaining siblings which are not stale, if so, draw a connecting bar at its depth */
			for (rem = ancestor->siblings.next; rem != &ancestor->parent->children; rem = rem->next) {
				if (!(list_entry(rem, vmon_proc_t, siblings)->is_stale)) {
					XDrawLine(xserver->display, overlay->text_pixmap, overlays->text_gc,
						  bar_x, bar_y - OVERLAY_ROW_HEIGHT,	/* dst x1, y1 */
						  bar_x, bar_y);			/* dst x2, y2 (vertical line) */
					break; /* stop looking for more siblings at this ancestor when we find one that isn't stale */
				}
			}
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
				bar_x = (depth - 1) * (OVERLAY_ROW_HEIGHT / 2) + 4;

				/* if we're the last sibling, corner the tee by shortening the vbar */
				if (proc == last_sibling) {
					XDrawLine(xserver->display, overlay->text_pixmap, overlays->text_gc,
						  bar_x, bar_y - OVERLAY_ROW_HEIGHT,	/* dst x1, y1 */
						  bar_x, bar_y - 4);			/* dst x2, y2 (vertical bar) */
				} else {
					XDrawLine(xserver->display, overlay->text_pixmap, overlays->text_gc,
						  bar_x, bar_y - OVERLAY_ROW_HEIGHT,	/* dst x1, y1 */
						  bar_x, bar_y);			/* dst x2, y2 (vertical bar) */
				}

				XDrawLine(xserver->display, overlay->text_pixmap, overlays->text_gc,
					  bar_x, bar_y - 4,				/* dst x1, y1 */
					  bar_x + 2, bar_y - 4);			/* dst x2, y2 (horizontal bar) */

				/* terminate the outer sibling loop upon drawing the tee... */
				break;
			}
		}
	}

	shadow_row(overlays, overlay, row);
}


/* recursive draw function for "rest" of overlay: the per-process rows (heirarchy, argv, state, wchan, pid...) */
static void draw_overlay_rest(vwm_overlays_t *overlays, vwm_overlay_t *overlay, vmon_proc_t *proc, int *depth, int *row, int heirarchy_changed)
{
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;
	vmon_proc_t		*child;
	double			utime_delta, stime_delta;

	/* Some parts of this we must do on every sample to maintain coherence in the graphs, since they're incrementally kept
	 * in sync with the process heirarchy, allocating and shifting the rows as processes are created and destroyed.  Everything
	 * else we should be able to skip doing unless overlay.redraw_needed or their contents changed.
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
			VWM_TRACE("entered stale at overlay=%p depth=%i row=%i", overlay, *depth, *row);
			in_stale_entrypoint = in_stale = 1;
			(*row) += count_rows(proc) - 1;
		}

		(*depth)++;
		list_for_each_entry_prev(child, &proc->children, siblings) {
			draw_overlay_rest(overlays, overlay, child, depth, row, heirarchy_changed);
			(*row)--;
		}

		if (!proc->is_thread) {
			list_for_each_entry_prev(child, &proc->threads, threads) {
				draw_overlay_rest(overlays, overlay, child, depth, row, heirarchy_changed);
				(*row)--;
			}
		}
		(*depth)--;

		VWM_TRACE("%i (%.*s) is stale @ depth %i row %i is_thread=%i", proc->pid,
			((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len - 1,
			((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.array,
			(*depth), (*row), proc->is_thread);

		mark_finish(overlays, overlay, (*row));

		/* extract the row from the various layers */
		snowflake_row(overlays, overlay, overlay->grapha_picture, 1, (*row));
		snowflake_row(overlays, overlay, overlay->graphb_picture, 1, (*row));
		snowflake_row(overlays, overlay, overlay->text_picture, 0, (*row));
		snowflake_row(overlays, overlay, overlay->shadow_picture, 0, (*row));
		overlay->snowflakes_cnt++;

		/* stamp the name (and whatever else we include) into overlay.text_picture */
		print_argv(overlays, overlay, 5, overlay->heirarchy_end, proc);
		shadow_row(overlays, overlay, overlay->heirarchy_end);

		overlay->heirarchy_end--;

		if (in_stale_entrypoint) {
			VWM_TRACE("exited stale at overlay=%p depth=%i row=%i", overlay, *depth, *row);
			in_stale = 0;
		}

		return;
	} else if (proc->is_new) {
		/* what to do when a process has been introduced */
		VWM_TRACE("%i is new", proc->pid);

		allocate_row(overlays, overlay, overlay->grapha_picture, (*row));
		allocate_row(overlays, overlay, overlay->graphb_picture, (*row));
		allocate_row(overlays, overlay, overlay->text_picture, (*row));
		allocate_row(overlays, overlay, overlay->shadow_picture, (*row));

		overlay->heirarchy_end++;
	}

/* CPU utilization graphs */
	/* use the generation number to avoid recomputing this stuff for callbacks recurring on the same process in the same sample */
	if (proc_ctxt->generation != overlays->vmon.generation) {
		proc_ctxt->stime_delta = proc_stat->stime - proc_ctxt->last_stime;
		proc_ctxt->utime_delta = proc_stat->utime - proc_ctxt->last_utime;
		proc_ctxt->last_utime = proc_stat->utime;
		proc_ctxt->last_stime = proc_stat->stime;

		proc_ctxt->generation = overlays->vmon.generation;
	}

	if (proc->is_new) {
		/* we need a minimum of two samples before we can compute a delta to plot,
		 * so we suppress that and instead mark the start of monitoring with an impossible 100% of both graph contexts, a starting line. */
		stime_delta = utime_delta = overlays->total_delta;
	} else {
		stime_delta = proc_ctxt->stime_delta;
		utime_delta = proc_ctxt->utime_delta;
	}

	draw_bars(overlays, overlay, *row, stime_delta, overlays->total_delta, utime_delta, overlays->total_delta);

	draw_heirarchy_row(overlays, overlay, proc, *depth, *row, heirarchy_changed);

	(*row)++;

	/* recur any threads first, then any children processes */
	(*depth)++;
	if (!proc->is_thread) {	/* XXX: the threads member serves as the list head only when not a thread */
		list_for_each_entry(child, &proc->threads, threads) {
			draw_overlay_rest(overlays, overlay, child, depth, row, heirarchy_changed);
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		draw_overlay_rest(overlays, overlay, child, depth, row, heirarchy_changed);
	}
	(*depth)--;
}


/* recursive draw function entrypoint, draws the IOWait/Idle/HZ row, then enters draw_overlay_rest() */
static void draw_overlay(vwm_overlays_t *overlays, vwm_overlay_t *overlay, vmon_proc_t *proc, int *depth, int *row)
{
	vwm_xserver_t		*xserver = overlays->xserver;
	int			heirarchy_changed = 0;
	int			str_len, str_width;
	char			str[256];

/* CPU utilization graphs */
	/* IOWait and Idle % @ row 0 */
	draw_bars(overlays, overlay, *row, overlays->iowait_delta, overlays->total_delta, overlays->idle_delta, overlays->total_delta);

	/* only draw the \/\/\ and HZ if necessary */
	if (overlay->redraw_needed || overlays->prev_sampling_interval != overlays->sampling_interval) {
		snprintf(str, sizeof(str), "\\/\\/\\    %2iHz %n", (int)(overlays->sampling_interval == INFINITY ? 0 : 1 / overlays->sampling_interval), &str_len);
		XRenderFillRectangle(xserver->display, PictOpSrc, overlay->text_picture, &overlay_trans_color,
			0, 0,						/* dst x, y */
			overlay->visible_width, OVERLAY_ROW_HEIGHT);	/* dst w, h */
		str_width = XTextWidth(overlays->overlay_font, str, str_len);
		XDrawString(xserver->display, overlay->text_pixmap, overlays->text_gc,
			    overlay->visible_width - str_width, OVERLAY_ROW_HEIGHT - 3,		/* dst x, y */
			    str, str_len);
		shadow_row(overlays, overlay, 0);
	}
	(*row)++;

	if (!overlay->redraw_needed)
		heirarchy_changed = proc_heirarchy_changed(proc);


	draw_overlay_rest(overlays, overlay, proc, depth, row, heirarchy_changed);

	overlay->redraw_needed = 0;

	return;
}


/* consolidated version of overlay text and graph rendering, makes snowflakes integration cleaner, this always gets called regardless of the overlays mode */
static void maintain_overlay(vwm_overlays_t *overlays, vwm_overlay_t *overlay)
{
	vwm_xserver_t	*xserver = overlays->xserver;
	int		row = 0, depth = 0;

	if (!overlay->monitor || !overlay->monitor->stores[VMON_STORE_PROC_STAT])
		return;

	/* TODO:
	 * A side effect of responding to window resizes in this function is there's a latency proportional to the current sample_interval.
	 * Something to fix is to resize the overlays when the window resizes.
	 * However, simply resizing the overlays is insufficient.  Their contents need to be redrawn in the new dimensions, this is where it
	 * gets annoying.  The current maintain/draw_overlay makes assumptions about being run from the periodic vmon per-process callback.
	 * There needs to be a redraw mode added where draw_overlay is just reconstructing the current state, which requires that we suppress
	 * the phase advance and in maintain_overlay() and just enter draw_overlay() to redraw everything for the same generation.
	 * So this probably requires some tweaking of draw_overlay() as well as maintain_overlay().  I want to be able tocall mainta_overlays()
	 * from anywhere, and have it detect if it's being called on the same generation or if the generation has advanced.
	 * For now, the monitors will just be a little latent in window resizes which is pretty harmless artifact.
	 */

	overlay->phase += (overlay->width - 1);  /* simply change this to .phase++ to scroll the other direction */
	overlay->phase %= overlay->width;
	XRenderFillRectangle(xserver->display, PictOpSrc, overlay->grapha_picture, &overlay_trans_color, overlay->phase, 0, 1, overlay->height);
	XRenderFillRectangle(xserver->display, PictOpSrc, overlay->graphb_picture, &overlay_trans_color, overlay->phase, 0, 1, overlay->height);

	/* recursively draw the monitored processes to the overlay */
	draw_overlay(overlays, overlay, overlay->monitor, &depth, &row);
}


/* this callback gets invoked at sample time for every process we've explicitly monitored (not autofollowed children/threads)
 * It's where we update the cumulative data for all windows, including the graph masks, regardless of their visibility
 * It's also where we compose the graphs and text for visible windows into a picture ready for compositing with the window contents */
static void proc_sample_callback(vmon_t *vmon, void *sys_cb_arg, vmon_proc_t *proc, void *proc_cb_arg)
{
	vwm_overlays_t	*overlays = sys_cb_arg;
	vwm_overlay_t	*overlay = proc_cb_arg;

	VWM_TRACE("proc=%p overlay=%p", proc, overlay);

	/* render the various always-updated overlays, this is the component we do regardless of the overlays mode and window visibility,
	 * essentially the incrementally rendered/historic components */
	maintain_overlay(overlays, overlay);

	/* XXX TODO: we used to mark repaint as being needed if this overlay's window was mapped, but
	 * since extricating overlays from windows that's no longer convenient, and repaint is
	 * always performed after a sample.  Make sure the repainting isn't costly when nothing
	 * overlayed is mapped (the case that code optimized)
	 */
}


/* return the composed height of the overlay */
static int vwm_overlay_composed_height(vwm_overlays_t *overlays, vwm_overlay_t *overlay)
{
	int	snowflakes = overlay->snowflakes_cnt ? 1 + overlay->snowflakes_cnt : 0; /* don't include the separator row if there are no snowflakes */

	return MIN((overlay->heirarchy_end + snowflakes) * OVERLAY_ROW_HEIGHT, overlay->visible_height);
}


/* reset snowflakes on the specified overlay */
void vwm_overlay_reset_snowflakes(vwm_overlays_t *overlays, vwm_overlay_t *overlay)
{
	if (overlay->snowflakes_cnt) {
		overlay->snowflakes_cnt = 0;
		overlay->redraw_needed = 1;
	}
}


static void free_overlay_pictures(vwm_overlays_t *overlays, vwm_overlay_t *overlay)
{
	vwm_xserver_t	*xserver = overlays->xserver;

	XRenderFreePicture(xserver->display, overlay->grapha_picture);
	XRenderFreePicture(xserver->display, overlay->graphb_picture);
	XRenderFreePicture(xserver->display, overlay->tmp_picture);
	XRenderFreePicture(xserver->display, overlay->text_picture);
	XFreePixmap(xserver->display, overlay->text_pixmap);
	XRenderFreePicture(xserver->display, overlay->shadow_picture);
	XRenderFreePicture(xserver->display, overlay->picture);

}


static void copy_overlay_pictures(vwm_overlays_t *overlays, vwm_overlay_t *src, vwm_overlay_t *dest)
{
	vwm_xserver_t	*xserver = overlays->xserver;

	if (!src->width)
		return;

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


/* (re)size the specified overlay's visible dimensions */
int vwm_overlay_set_visible_size(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int width, int height)
{
	if (width != overlay->visible_width || height != overlay->visible_height)
		overlay->redraw_needed = 1;

	/* TODO error handling: if a create failed but we had an overlay, free whatever we created and leave it be, succeed.
	 * if none existed it's a hard error and we must propagate it. */

	/* if larger than the overlays currently are, enlarge them */
	if (width > overlay->width || height > overlay->height) {
		vwm_overlay_t	existing = *overlay;

		overlay->width = MAX(overlay->width, MAX(width, OVERLAY_GRAPH_MIN_WIDTH));
		overlay->height = MAX(overlay->height, MAX(height, OVERLAY_GRAPH_MIN_HEIGHT));

		overlay->grapha_picture = create_picture_fill(overlays, overlay->width, overlay->height, OVERLAY_MASK_DEPTH, CPRepeat, &pa_repeat, &overlay_trans_color, NULL);
		overlay->graphb_picture = create_picture_fill(overlays, overlay->width, overlay->height, OVERLAY_MASK_DEPTH, CPRepeat, &pa_repeat, &overlay_trans_color, NULL);
		overlay->tmp_picture = create_picture(overlays, overlay->width, OVERLAY_ROW_HEIGHT, OVERLAY_MASK_DEPTH, 0, NULL, NULL);

		/* keep the text_pixmap reference around for XDrawText usage */
		overlay->text_picture = create_picture_fill(overlays, overlay->width, overlay->height, OVERLAY_MASK_DEPTH, 0, NULL, &overlay_trans_color, &overlay->text_pixmap);

		overlay->shadow_picture = create_picture_fill(overlays, overlay->width, overlay->height, OVERLAY_MASK_DEPTH, 0, NULL, &overlay_trans_color, NULL);
		overlay->picture = create_picture(overlays, overlay->width, overlay->height, 32, 0, NULL, NULL);

		copy_overlay_pictures(overlays, &existing, overlay);
		free_overlay_pictures(overlays, &existing);
	}

	overlay->visible_width = width;
	overlay->visible_height = height;

	return 1;
}


/* create an overlay and start monitoring for the supplied pid */
vwm_overlay_t * vwm_overlay_create(vwm_overlays_t *overlays, int pid, int width, int height)
{
	vwm_overlay_t	*overlay;

	overlay = calloc(1, sizeof(vwm_overlay_t));
	if (!overlay) {
		VWM_PERROR("Unable to allocate vwm_overlay_t");
		goto _err;
	}

	/* add the client process to the monitoring heirarchy */
	/* XXX note libvmon here maintains a unique callback for each unique callback+xwin pair, so multi-window processes work */
	overlay->monitor = vmon_proc_monitor(&overlays->vmon, NULL, pid, VMON_WANT_PROC_INHERIT, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, overlay);
	if (!overlay->monitor) {
		VWM_ERROR("Unable to establish proc monitor");
		goto _err_free;
	}

	 /* FIXME: count_rows() isn't returning the right count sometimes (off by ~1), it seems to be related to racing with the automatic child monitoring */
	 /* the result is an extra row sometimes appearing below the process heirarchy */
	overlay->heirarchy_end = 1 + count_rows(overlay->monitor);
	overlay->gen_last_composed = -1;

	if (!vwm_overlay_set_visible_size(overlays, overlay, width, height)) {
		VWM_ERROR("Unable to set initial overlay size");
		goto _err_unmonitor;
	}

	return overlay;

_err_unmonitor:
	vmon_proc_unmonitor(&overlays->vmon, overlay->monitor, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, overlay);

_err_free:
	free(overlay);
_err:
	return NULL;
}


/* stop monitoring and destroy the supplied overlay */
void vwm_overlay_destroy(vwm_overlays_t *overlays, vwm_overlay_t *overlay)
{
	vmon_proc_unmonitor(&overlays->vmon, overlay->monitor, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, overlay);
	free_overlay_pictures(overlays, overlay);
	free(overlay);
}


/* this composes the maintained overlay into the base overlay picture, this gets called from paint_all() on every repaint of xwin */
/* we noop the call if the gen_last_composed and monitor->proc.generation numbers match, indicating there's nothing new to compose. */
void vwm_overlay_compose(vwm_overlays_t *overlays, vwm_overlay_t *overlay, XserverRegion *res_damaged_region)
{
	vwm_xserver_t	*xserver = overlays->xserver;
	int		height;

	if (!overlay->width || !overlay->height)
		return;

	if (overlay->gen_last_composed == overlay->monitor->generation)
		return; /* noop if no sampling occurred since last compose */

	overlay->gen_last_composed = overlay->monitor->generation; /* remember this generation */

	//VWM_TRACE("composing %p", overlay);

	height = vwm_overlay_composed_height(overlays, overlay);

	/* fill the overlay picture with the background */
	XRenderComposite(xserver->display, PictOpSrc, overlays->bg_fill, None, overlay->picture,
		0, 0,
		0, 0,
		0, 0,
		overlay->visible_width, height);

	/* draw the graphs into the overlay through the stencils being maintained by the sample callbacks */
	XRenderComposite(xserver->display, PictOpOver, overlays->grapha_fill, overlay->grapha_picture, overlay->picture,
		0, 0,
		overlay->phase, 0,
		0, 0,
		overlay->visible_width, height);
	XRenderComposite(xserver->display, PictOpOver, overlays->graphb_fill, overlay->graphb_picture, overlay->picture,
		0, 0,
		overlay->phase, 0,
		0, 0,
		overlay->visible_width, height);

	/* draw the shadow into the overlay picture using a translucent black source drawn through the shadow mask */
	XRenderComposite(xserver->display, PictOpOver, overlays->shadow_fill, overlay->shadow_picture, overlay->picture,
		0, 0,
		0, 0,
		0, 0,
		overlay->visible_width, height);

	/* render overlay text into the overlay picture using a white source drawn through the overlay text as a mask, on top of everything */
	XRenderComposite(xserver->display, PictOpOver, overlays->text_fill, overlay->text_picture, overlay->picture,
		0, 0,
		0, 0,
		0, 0,
		overlay->visible_width, (overlay->heirarchy_end * OVERLAY_ROW_HEIGHT));

	XRenderComposite(xserver->display, PictOpOver, overlays->snowflakes_text_fill, overlay->text_picture, overlay->picture,
		0, 0,
		0, overlay->heirarchy_end * OVERLAY_ROW_HEIGHT,
		0, overlay->heirarchy_end * OVERLAY_ROW_HEIGHT,
		overlay->visible_width, height - (overlay->heirarchy_end * OVERLAY_ROW_HEIGHT));

	/* damage the window to ensure the updated overlay is drawn (TODO: this can be done more selectively/efficiently) */
	if (res_damaged_region) {
		XRectangle	damage = {};

		damage.width = overlay->visible_width;
		damage.height = overlay->visible_height;

		*res_damaged_region = XFixesCreateRegion(xserver->display, &damage, 1);
	}
}


/* render the overlay into a picture at the specified coordinates and dimensions */
void vwm_overlay_render(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int op, Picture dest, int x, int y, int width, int height)
{
	vwm_xserver_t	*xserver = overlays->xserver;

	if (!overlay->width || !overlay->height)
		return;

	/* draw the monitoring overlay atop dest, note we stay within the window borders here. */
	XRenderComposite(xserver->display, op, overlay->picture, None, dest,
			 0, 0, 0, 0,										/* src x,y, maxk x, y */
			 x,											/* dst x */
			 y,											/* dst y */
			 width, MIN(vwm_overlay_composed_height(overlays, overlay), height) /* FIXME */);	/* w, h */
}


/* increase the sample rate relative to current using the table of intervals */
void vwm_overlays_rate_increase(vwm_overlays_t *overlays)
{
	int	i;

	assert(overlays);

	for (i = 0; i < NELEMS(sampling_intervals); i++) {
		if (sampling_intervals[i] < overlays->sampling_interval) {
			overlays->sampling_interval = sampling_intervals[i];
			break;
		}
	}
}


/* decrease the sample rate relative to current using the table of intervals */
void vwm_overlays_rate_decrease(vwm_overlays_t *overlays)
{
	int	i;

	assert(overlays);

	for (i = NELEMS(sampling_intervals) - 1; i >= 0; i--) {
		if (sampling_intervals[i] > overlays->sampling_interval) {
			overlays->sampling_interval = sampling_intervals[i];
			break;
		}
	}
}


/* set an arbitrary sample rate rather than using one of the presets, 0 to pause */
void vwm_overlays_rate_set(vwm_overlays_t *overlays, unsigned hertz)
{
	assert(overlays);

	/* XXX: note floating point divide by 0 simply results in infinity */
	overlays->sampling_interval = 1.0f / (float)hertz;
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


/* update the overlays if necessary, return if updating occurred, and duration before another update needed in *desired_delay */
int vwm_overlays_update(vwm_overlays_t *overlays, int *desired_delay)
{
	float	this_delta = 0.0f;
	int	ret = 0;

	gettimeofday(&overlays->maybe_sample, NULL);
	if ((overlays->sampling_interval == INFINITY && !overlays->sampling_paused) || /* XXX this is kind of a kludge to get the 0 Hz indicator drawn before pausing */
	    (overlays->sampling_interval != INFINITY && ((this_delta = delta(&overlays->maybe_sample, &overlays->this_sample)) >= overlays->sampling_interval))) {
		vmon_sys_stat_t	*sys_stat;

		/* automatically lower the sample rate if we can't keep up with the current sample rate */
		if (overlays->sampling_interval < INFINITY &&
		    overlays->sampling_interval <= overlays->prev_sampling_interval &&
		    this_delta >= (overlays->sampling_interval * 1.5)) {

			/* require > 1 contiguous drops before lowering the rate, tolerates spurious one-off stalls */
			if (++overlays->contiguous_drops > 2)
				vwm_overlays_rate_decrease(overlays);
		} else {
			overlays->contiguous_drops = 0;
		}

		/* age the sys-wide sample data into "last" variables, before the new sample overwrites them. */
		overlays->last_sample = overlays->this_sample;
		overlays->this_sample = overlays->maybe_sample;
		if ((sys_stat = overlays->vmon.stores[VMON_STORE_SYS_STAT])) {
			overlays->last_user_cpu = sys_stat->user;
			overlays->last_system_cpu = sys_stat->system;
			overlays->last_total =	sys_stat->user +
					sys_stat->nice +
					sys_stat->system +
					sys_stat->idle +
					sys_stat->iowait +
					sys_stat->irq +
					sys_stat->softirq +
					sys_stat->steal +
					sys_stat->guest;

			overlays->last_idle = sys_stat->idle;
			overlays->last_iowait = sys_stat->iowait;
		}

		ret = vmon_sample(&overlays->vmon);	/* XXX: calls proc_sample_callback() for explicitly monitored processes after sampling their descendants */
						/* XXX: also calls sample_callback() per invocation after sampling the sys wants */

		overlays->sampling_paused = (overlays->sampling_interval == INFINITY);
		overlays->prev_sampling_interval = overlays->sampling_interval;
	}

	/* TODO: make some effort to compute how long to sleep, but this is perfectly fine for now. */
	*desired_delay = overlays->sampling_interval == INFINITY ? -1 : overlays->sampling_interval * 300.0;

	return ret;
}
