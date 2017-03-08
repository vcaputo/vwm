/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2016  Vito Caputo - <vcaputo@gnugeneration.com>
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
#include <stdlib.h>
#include <sys/time.h>

#include "composite.h"
#include "libvmon/vmon.h"
#include "list.h"
#include "overlay.h"
#include "vwm.h"
#include "xwindow.h"

/* TODO: move to overlay.h */
#define OVERLAY_MASK_DEPTH		8					/* XXX: 1 would save memory, but Xorg isn't good at it */
#define OVERLAY_MASK_FORMAT		PictStandardA8
#define OVERLAY_FIXED_FONT		"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1"
#define OVERLAY_ROW_HEIGHT		15					/* this should always be larger than the font height */
#define OVERLAY_GRAPH_MIN_WIDTH		200					/* always create graphs at least this large */
#define OVERLAY_GRAPH_MIN_HEIGHT	(4 * OVERLAY_ROW_HEIGHT)
#define OVERLAY_ISTHREAD_ARGV		"~"					/* use this string to mark threads in the argv field */
#define OVERLAY_NOCOMM_ARGV		"#missed it!"				/* use this string to substitute the command when missing in argv field */
#define OVERLAY_MAX_ARGC		512					/* this is a huge amount */


/* libvmon */
static struct timeval				maybe_sample, last_sample, this_sample = {0,0};
static typeof(((vmon_sys_stat_t *)0)->user)	last_user_cpu;
static typeof(((vmon_sys_stat_t *)0)->system)	last_system_cpu;
static unsigned long long			last_total, this_total, total_delta;
static unsigned long long			last_idle, last_iowait, idle_delta, iowait_delta;
static vmon_t					vmon;

static float					sampling_intervals[] = {
							1,		/* ~1Hz */
							.1,		/* ~10Hz */
							.05,		/* ~20Hz */
							.025,		/* ~40Hz */
							.01666};	/* ~60Hz */
static int					prev_sampling_interval = 1, sampling_interval = 1;

/* space we need for every process being monitored */
typedef struct _vwm_perproc_ctxt_t {
	typeof(vmon.generation)			generation;
	typeof(((vmon_proc_stat_t *)0)->utime)	last_utime;
	typeof(((vmon_proc_stat_t *)0)->stime)	last_stime;
	typeof(((vmon_proc_stat_t *)0)->utime)	utime_delta;
	typeof(((vmon_proc_stat_t *)0)->stime)	stime_delta;
} vwm_perproc_ctxt_t;

/* Compositing / Overlays */
static XFontStruct		*overlay_font;
static GC			text_gc;
static XRenderPictureAttributes	pa_repeat = { .repeat = 1 };
static XRenderPictureAttributes	pa_no_repeat = { .repeat = 0 };
static Picture			overlay_shadow_fill,	/* TODO: the repetition here smells like an XMacro waiting to happen */
				overlay_text_fill,
				overlay_bg_fill,
				overlay_snowflakes_text_fill,
				overlay_grapha_fill,
				overlay_graphb_fill,
				overlay_finish_fill;
static XRenderColor		overlay_visible_color = { 0xffff, 0xffff, 0xffff, 0xffff },
				overlay_shadow_color = { 0x0000, 0x0000, 0x0000, 0x8800},
				overlay_bg_color = { 0x0, 0x1000, 0x0, 0x9000},
				overlay_div_color = { 0x2000, 0x3000, 0x2000, 0x9000},
				overlay_snowflakes_visible_color = { 0xd000, 0xd000, 0xd000, 0x8000 },
				overlay_trans_color = {0x00, 0x00, 0x00, 0x00},
				overlay_grapha_color = { 0xff00, 0x0000, 0x0000, 0x3000 },	/* ~red */
				overlay_graphb_color = { 0x0000, 0xffff, 0xffff, 0x3000 };	/* ~cyan */


/* moves what's below a given row up above it if specified, the row becoming discarded */
static void snowflake_row(vwm_t *vwm, vwm_xwindow_t *xwin, Picture pic, int copy, int row)
{
	VWM_TRACE("pid=%i xwin=%p row=%i copy=%i heirarhcy_end=%i", xwin->monitor->pid, xwin, row, copy, xwin->overlay.heirarchy_end);

	if (copy) {
		/* copy row to tmp */
		XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, pic, None, xwin->overlay.tmp_picture,
			0, row * OVERLAY_ROW_HEIGHT,			/* src */
			0, 0,						/* mask */
			0, 0,						/* dest */
			xwin->overlay.width, OVERLAY_ROW_HEIGHT);	/* dimensions */
	}

	/* shift up */
	XRenderChangePicture(VWM_XDISPLAY(vwm), pic, CPRepeat, &pa_no_repeat);
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, pic, None, pic,
		0, (1 + row) * OVERLAY_ROW_HEIGHT,										/* src */
		0, 0,														/* mask */
		0, row * OVERLAY_ROW_HEIGHT,											/* dest */
		xwin->overlay.width, (1 + xwin->overlay.heirarchy_end) * OVERLAY_ROW_HEIGHT - (1 + row) * OVERLAY_ROW_HEIGHT);	/* dimensions */
	XRenderChangePicture(VWM_XDISPLAY(vwm), pic, CPRepeat, &pa_repeat);

	if (copy) {
		/* copy tmp to top of snowflakes */
		XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.tmp_picture, None, pic,
			0, 0,									/* src */
			0, 0,									/* mask */
			0, (xwin->overlay.heirarchy_end) * OVERLAY_ROW_HEIGHT,			/* dest */
			xwin->overlay.width, OVERLAY_ROW_HEIGHT);				/* dimensions */
	} else {
		/* clear the snowflake row */
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, pic, &overlay_trans_color,
			0, (xwin->overlay.heirarchy_end) * OVERLAY_ROW_HEIGHT,			/* dest */
			xwin->overlay.width, OVERLAY_ROW_HEIGHT);				/* dimensions */
	}
}

/* XXX TODO libvmon automagic children following races with explicit X client pid monitoring with different outcomes, it should be irrelevant which wins,
 *     currently the only visible difference is the snowflakes gap (heirarchy_end) varies, which is why I haven't bothered to fix it, I barely even notice.
 */

/* shifts what's below a given row down a row, and clears the row, preparing it for populating */
static void allocate_row(vwm_t *vwm, vwm_xwindow_t *xwin, Picture pic, int row)
{
	VWM_TRACE("pid=%i xwin=%p row=%i", xwin->monitor->pid, xwin, row);

	/* shift everything below the row down */
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, pic, None, pic,
		0, row * OVERLAY_ROW_HEIGHT,							/* src */
		0, 0,										/* mask */
		0, (1 + row) * OVERLAY_ROW_HEIGHT,						/* dest */
		xwin->overlay.width, xwin->overlay.height - (1 + row) * OVERLAY_ROW_HEIGHT);	/* dimensions */
	/* fill the space created with transparent pixels */
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, pic, &overlay_trans_color,
		0, row * OVERLAY_ROW_HEIGHT,							/* dest */
		xwin->overlay.width, OVERLAY_ROW_HEIGHT);					/* dimensions */
}


/* shadow a row from the text layer in the shadow layer */
static void shadow_row(vwm_t *vwm, vwm_xwindow_t *xwin, int row)
{
	/* the current technique for creating the shadow is to simply render the text at +1/-1 pixel offsets on both axis in translucent black */
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		-1, row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		0, -1 + row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		1, row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		0, 1 + row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);
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
static int count_rows(vmon_proc_t *proc) {
	int		count = 1; /* XXX maybe suppress proc->is_new? */
	vmon_proc_t	*child;

	if (!proc->is_thread) {
		list_for_each_entry(child, &proc->threads, threads) {
			count += count_rows(child);
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		count += count_rows(child);
	}

	return count;
}


/* helper for detecting if any children/threads in the process heirarchy rooted @ proc are new/stale this sample */
static int proc_heirarchy_changed(vmon_proc_t *proc) {
	vmon_proc_t	*child;

	if (proc->children_changed || proc->threads_changed) return 1;

	if (!proc->is_thread) {
		list_for_each_entry(child, &proc->threads, threads) {
			if (proc_heirarchy_changed(child)) return 1;
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		if (proc_heirarchy_changed(child)) return 1;
	}

	return 0;
}


/* helper for drawing the vertical bars in the graph layers */
static void draw_bars(vwm_t *vwm, vwm_xwindow_t *xwin, int row, double a_fraction, double a_total, double b_fraction, double b_total)
{
	int	a_height, b_height;

	/* compute the bar heights for this sample */
	a_height = (a_fraction / a_total * (double)(OVERLAY_ROW_HEIGHT - 1)); /* give up 1 pixel for the div */
	b_height = (b_fraction / b_total * (double)(OVERLAY_ROW_HEIGHT - 1));

	/* round up to 1 pixel when the scaled result is a fraction less than 1,
	 * I want to at least see 1 pixel blips for the slightest cpu utilization */
	if (a_fraction && !a_height) a_height = 1;
	if (b_fraction && !b_height) b_height = 1;

	/* draw the two bars for this sample at the current phase in the graphs, note the first is ceiling-based, second floor-based */
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.grapha_picture, &overlay_visible_color,
		xwin->overlay.phase, row * OVERLAY_ROW_HEIGHT,					/* dst x, y */
		1, a_height);										/* dst w, h */
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.graphb_picture, &overlay_visible_color,
		xwin->overlay.phase, row * OVERLAY_ROW_HEIGHT + (OVERLAY_ROW_HEIGHT - b_height) - 1,	/* dst x, y */
		1, b_height);										/* dst w, h */
}


/* draws proc in a row of the process heirarchy */
static void draw_heirarchy_row(vwm_t *vwm, vwm_xwindow_t *xwin, vmon_proc_t *proc, int depth, int row, int heirarchy_changed)
{
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];
	vmon_proc_t		*child;
	char			str[256];
	int			str_len, str_width;
	XTextItem		items[OVERLAY_MAX_ARGC];
	int			nr_items;

/* process heirarchy text and accompanying per-process details like wchan/pid/state... */

	/* skip if obviously unnecessary (this can be further improved, but this makes a big difference as-is) */
	if (!xwin->overlay.redraw_needed &&
	    !heirarchy_changed &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_WCHAN) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_PID) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_STATE) &&
	    !BITTEST(proc_stat->changed, VMON_PROC_STAT_ARGV)) return;

	/* TODO: make the columns interactively configurable @ runtime */
	if (!proc->is_new) {
	/* XXX for now always clear the row, this should be capable of being optimized in the future (if the datums driving the text haven't changed...) */
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color,
			0, row * OVERLAY_ROW_HEIGHT,			/* dst x, y */
			xwin->overlay.width, OVERLAY_ROW_HEIGHT);	/* dst w, h */
	}

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
	str_width = XTextWidth(overlay_font, str, str_len);

	/* the process' comm label indented according to depth, followed with their respective argv's */
	argv2xtext(proc, items, NELEMS(items), &nr_items);
	XDrawText(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
		  depth * (OVERLAY_ROW_HEIGHT / 2), (row + 1) * OVERLAY_ROW_HEIGHT - 3,			/* dst x, y */
		  items, nr_items);

	/* ensure the area for the rest of the stuff is cleared, we don't put much text into thread rows so skip it for those. */
	if (!proc->is_thread) {
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color,
			xwin->attrs.width - str_width, row * OVERLAY_ROW_HEIGHT,			/* dst x,y */
			xwin->overlay.width - (xwin->attrs.width - str_width), OVERLAY_ROW_HEIGHT);	/* dst w,h */
	}

	XDrawString(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
		    xwin->attrs.width - str_width, (row + 1) * OVERLAY_ROW_HEIGHT - 3,		/* dst x, y */
		    str, str_len);

	/* only if this process isn't the root process @ the window shall we consider all relational drawing conditions */
	if (proc != xwin->monitor) {
		vmon_proc_t		*ancestor, *sibling, *last_sibling = NULL;
		struct list_head	*rem;
		int			needs_tee = 0;
		int			bar_x = 0, bar_y = (row + 1) * OVERLAY_ROW_HEIGHT;
		int			sub;

		/* XXX: everything done in this code block only dirties _this_ process' row in the rendered overlay output */

		/* walk up the ancestors until reaching xwin->monitor, any ancestors we encounter which have more siblings we draw a vertical bar for */
		/* this draws the |'s in something like:  | |   |    | comm */
		for (sub = 1, ancestor = proc->parent; ancestor && ancestor != xwin->monitor; ancestor = ancestor->parent) {
			sub++;
			bar_x = (depth - sub) * (OVERLAY_ROW_HEIGHT / 2) + 4;

			/* determine if the ancestor has remaining siblings which are not stale, if so, draw a connecting bar at its depth */
			for (rem = ancestor->siblings.next; rem != &ancestor->parent->children; rem = rem->next) {
				if (!(list_entry(rem, vmon_proc_t, siblings)->is_stale)) {
					XDrawLine(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
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
			if (!sibling->is_stale) last_sibling = sibling;
		}

		/* now look for siblings with non-stale children to determine if a tee is needed, ignoring the last sibling */
		list_for_each_entry(sibling, &proc->parent->children, siblings) {
			/* skip stale siblings, they aren't interesting as they're invisible, and the last sibling has no bearing on wether we tee or not. */
			if (sibling->is_stale || sibling == last_sibling) continue;

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
					XDrawLine(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
						  bar_x, bar_y - OVERLAY_ROW_HEIGHT,	/* dst x1, y1 */
						  bar_x, bar_y - 4);			/* dst x2, y2 (vertical bar) */
				} else {
					XDrawLine(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
						  bar_x, bar_y - OVERLAY_ROW_HEIGHT,	/* dst x1, y1 */
						  bar_x, bar_y);			/* dst x2, y2 (vertical bar) */
				}

				XDrawLine(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
					  bar_x, bar_y - 4,				/* dst x1, y1 */
					  bar_x + 2, bar_y - 4);			/* dst x2, y2 (horizontal bar) */

				/* terminate the outer sibling loop upon drawing the tee... */
				break;
			}
		}
	}

	shadow_row(vwm, xwin, row);
}


/* recursive draw function for "rest" of overlay: the per-process rows (heirarchy, argv, state, wchan, pid...) */
static void draw_overlay_rest(vwm_t *vwm, vwm_xwindow_t *xwin, vmon_proc_t *proc, int *depth, int *row, int heirarchy_changed)
{
	vmon_proc_t		*child;
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];

	/* graph variables */
	double			utime_delta, stime_delta;

	/* text variables */
	XTextItem		items[OVERLAY_MAX_ARGC];
	int			nr_items;

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
			VWM_TRACE("entered stale at xwin=%p depth=%i row=%i", xwin, *depth, *row);
			in_stale_entrypoint = in_stale = 1;
			(*row) += count_rows(proc) - 1;
		}

		(*depth)++;
		list_for_each_entry_prev(child, &proc->children, siblings) {
			draw_overlay_rest(vwm, xwin, child, depth, row, heirarchy_changed);
			(*row)--;
		}

		if (!proc->is_thread) {
			list_for_each_entry_prev(child, &proc->threads, threads) {
				draw_overlay_rest(vwm, xwin, child, depth, row, heirarchy_changed);
				(*row)--;
			}
		}
		(*depth)--;

		VWM_TRACE("%i (%.*s) is stale @ depth %i row %i is_thread=%i", proc->pid,
			((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len - 1,
			((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.array,
			(*depth), (*row), proc->is_thread);

		/* stamp the graphs with the finish line */
		XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, overlay_finish_fill, None, xwin->overlay.grapha_picture,
				 0, 0,							/* src x, y */
				 0, 0,							/* mask x, y */
				 xwin->overlay.phase, (*row) * OVERLAY_ROW_HEIGHT,	/* dst x, y */
				 1, OVERLAY_ROW_HEIGHT - 1);
		XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, overlay_finish_fill, None, xwin->overlay.graphb_picture,
				 0, 0,							/* src x, y */
				 0, 0,							/* mask x, y */
				 xwin->overlay.phase, (*row) * OVERLAY_ROW_HEIGHT,	/* dst x, y */
				 1, OVERLAY_ROW_HEIGHT - 1);

		/* extract the row from the various layers */
		snowflake_row(vwm, xwin, xwin->overlay.grapha_picture, 1, (*row));
		snowflake_row(vwm, xwin, xwin->overlay.graphb_picture, 1, (*row));
		snowflake_row(vwm, xwin, xwin->overlay.text_picture, 0, (*row));
		snowflake_row(vwm, xwin, xwin->overlay.shadow_picture, 0, (*row));
		xwin->overlay.snowflakes_cnt++;

		/* stamp the name (and whatever else we include) into overlay.text_picture */
		argv2xtext(proc, items, NELEMS(items), &nr_items);
		XDrawText(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
			  5, (xwin->overlay.heirarchy_end + 1) * OVERLAY_ROW_HEIGHT - 3,/* dst x, y */
			  items, nr_items);
		shadow_row(vwm, xwin, xwin->overlay.heirarchy_end);

		xwin->overlay.heirarchy_end--;

		if (in_stale_entrypoint) {
			VWM_TRACE("exited stale at xwin=%p depth=%i row=%i", xwin, *depth, *row);
			in_stale = 0;
		}

		return;
	} else if (proc->is_new) {
		/* what to do when a process has been introduced */
		VWM_TRACE("%i is new", proc->pid);

		allocate_row(vwm, xwin, xwin->overlay.grapha_picture, (*row));
		allocate_row(vwm, xwin, xwin->overlay.graphb_picture, (*row));
		allocate_row(vwm, xwin, xwin->overlay.text_picture, (*row));
		allocate_row(vwm, xwin, xwin->overlay.shadow_picture, (*row));

		xwin->overlay.heirarchy_end++;
	}

/* CPU utilization graphs */
	/* use the generation number to avoid recomputing this stuff for callbacks recurring on the same process in the same sample */
	if (proc_ctxt->generation != vmon.generation) {
		proc_ctxt->stime_delta = proc_stat->stime - proc_ctxt->last_stime;
		proc_ctxt->utime_delta = proc_stat->utime - proc_ctxt->last_utime;
		proc_ctxt->last_utime = proc_stat->utime;
		proc_ctxt->last_stime = proc_stat->stime;

		proc_ctxt->generation = vmon.generation;
	}

	if (proc->is_new) {
		/* we need a minimum of two samples before we can compute a delta to plot,
		 * so we suppress that and instead mark the start of monitoring with an impossible 100% of both graph contexts, a starting line. */
		stime_delta = utime_delta = total_delta;
	} else {
		stime_delta = proc_ctxt->stime_delta;
		utime_delta = proc_ctxt->utime_delta;
	}

	draw_bars(vwm, xwin, *row, stime_delta, total_delta, utime_delta, total_delta);

	draw_heirarchy_row(vwm, xwin, proc, *depth, *row, heirarchy_changed);

	(*row)++;

	/* recur any threads first, then any children processes */
	(*depth)++;
	if (!proc->is_thread) {	/* XXX: the threads member serves as the list head only when not a thread */
		list_for_each_entry(child, &proc->threads, threads) {
			draw_overlay_rest(vwm, xwin, child, depth, row, heirarchy_changed);
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		draw_overlay_rest(vwm, xwin, child, depth, row, heirarchy_changed);
	}
	(*depth)--;
}



/* recursive draw function entrypoint, draws the IOWait/Idle/HZ row, then enters draw_overlay_rest() */
static void draw_overlay(vwm_t *vwm, vwm_xwindow_t *xwin, vmon_proc_t *proc, int *depth, int *row)
{
	vmon_proc_t		*child;
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];

	/* text variables */
	char			str[256];
	int			str_len, str_width;

	int			heirarchy_changed = 0;

/* CPU utilization graphs */
	/* IOWait and Idle % @ row 0 */
	draw_bars(vwm, xwin, *row, iowait_delta, total_delta, idle_delta, total_delta);

	/* only draw the \/\/\ and HZ if necessary */
	if (xwin->overlay.redraw_needed || prev_sampling_interval != sampling_interval) {
		snprintf(str, sizeof(str), "\\/\\/\\    %2iHz %n", (int)(sampling_interval < 0 ? 0 : 1 / sampling_intervals[sampling_interval]), &str_len);
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color,
			0, 0,					/* dst x, y */
			xwin->attrs.width, OVERLAY_ROW_HEIGHT);	/* dst w, h */
		str_width = XTextWidth(overlay_font, str, str_len);
		XDrawString(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, text_gc,
			    xwin->attrs.width - str_width, OVERLAY_ROW_HEIGHT - 3,		/* dst x, y */
			    str, str_len);
		shadow_row(vwm, xwin, 0);
	}
	(*row)++;

	if (!xwin->overlay.redraw_needed) heirarchy_changed = proc_heirarchy_changed(proc);


	draw_overlay_rest(vwm, xwin, proc, depth, row, heirarchy_changed);

	xwin->overlay.redraw_needed = 0;

	return;
}


/* consolidated version of overlay text and graph rendering, makes snowflakes integration cleaner, this always gets called regadless of the overlays mode */
static void maintain_overlay(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	int	row = 0, depth = 0;

	if (!xwin->monitor || !xwin->monitor->stores[VMON_STORE_PROC_STAT]) return;

	/* TODO:
	 * I side effect of responding to window resizes in this function is there's a latency proportional to the current sample_interval.
	 * Something to fix is to resize the overlays when the window resizes.
	 * However, simply resizing the overlays is insufficient.  Their contents need to be redrawn in the new dimensions, this is where it
	 * gets annoying.  The current maintain/draw_overlay makes assumptions about being run from the periodic vmon per-process callback.
	 * There needs to be a redraw mode added where draw_overlay is just reconstructing the current state, which requires that we suppress
	 * the phase advance and in maintain_overlay() and just enter draw_overlay() to redraw everything for the same generation.
	 * So this probably requires some tweaking of draw_overlay() as well as maintain_overlay().  I want to be able tocall mainta_overlays()
	 * from anywhere, and have it detect if it's being called on the same generation or if the generation has advanced.
	 * For now, the monitors will just be a little latent in window resizes which is pretty harmless artifact.
	 */

	if (xwin->attrs.width != xwin->overlay.width || xwin->attrs.height != xwin->overlay.height) xwin->overlay.redraw_needed = 1;

	/* if the window is larger than the overlays currently are, enlarge them */
	if (xwin->attrs.width > xwin->overlay.width || xwin->attrs.height > xwin->overlay.height) {
		vwm_overlay_t	existing;
		Pixmap		pixmap;

		existing = xwin->overlay;

		xwin->overlay.width = MAX(xwin->overlay.width, MAX(xwin->attrs.width, OVERLAY_GRAPH_MIN_WIDTH));
		xwin->overlay.height = MAX(xwin->overlay.height, MAX(xwin->attrs.height, OVERLAY_GRAPH_MIN_HEIGHT));

		pixmap = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.grapha_picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), pixmap, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), OVERLAY_MASK_FORMAT), CPRepeat, &pa_repeat);
		XFreePixmap(VWM_XDISPLAY(vwm), pixmap);
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.grapha_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.graphb_picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), pixmap, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), OVERLAY_MASK_FORMAT), CPRepeat, &pa_repeat);
		XFreePixmap(VWM_XDISPLAY(vwm), pixmap);
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.graphb_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), xwin->overlay.width, OVERLAY_ROW_HEIGHT, OVERLAY_MASK_DEPTH);
		xwin->overlay.tmp_picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), pixmap, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), OVERLAY_MASK_FORMAT), 0, NULL);
		XFreePixmap(VWM_XDISPLAY(vwm), pixmap);

		/* keep the text_pixmap reference around for XDrawText usage */
		xwin->overlay.text_pixmap = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.text_picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), xwin->overlay.text_pixmap, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), OVERLAY_MASK_FORMAT), 0, NULL);
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.shadow_picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), pixmap, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), OVERLAY_MASK_FORMAT), 0, NULL);
		XFreePixmap(VWM_XDISPLAY(vwm), pixmap);
		XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.shadow_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), xwin->overlay.width, xwin->overlay.height, 32);
		xwin->overlay.picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), pixmap, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), 0, NULL);
		XFreePixmap(VWM_XDISPLAY(vwm), pixmap);

		if (existing.width) {
			/* XXX: note the graph pictures are copied from their current phase in the x dimension */
			XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, existing.grapha_picture, None, xwin->overlay.grapha_picture,
				existing.phase, 0,	/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, existing.graphb_picture, None, xwin->overlay.graphb_picture,
				existing.phase, 0,	/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, existing.text_picture, None, xwin->overlay.text_picture,
				0, 0,			/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, existing.shadow_picture, None, xwin->overlay.shadow_picture,
				0, 0,			/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, existing.picture, None, xwin->overlay.picture,
				0, 0,			/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			xwin->overlay.phase = 0;	/* having unrolled the existing graph[ab] pictures into the larger ones, phase is reset to 0 */
			XRenderFreePicture(VWM_XDISPLAY(vwm), existing.grapha_picture);
			XRenderFreePicture(VWM_XDISPLAY(vwm), existing.graphb_picture);
			XRenderFreePicture(VWM_XDISPLAY(vwm), existing.tmp_picture);
			XRenderFreePicture(VWM_XDISPLAY(vwm), existing.text_picture);
			XFreePixmap(VWM_XDISPLAY(vwm), existing.text_pixmap);
			XRenderFreePicture(VWM_XDISPLAY(vwm), existing.shadow_picture);
			XRenderFreePicture(VWM_XDISPLAY(vwm), existing.picture);
		}
	}

	xwin->overlay.phase += (xwin->overlay.width - 1);  /* simply change this to .phase++ to scroll the other direction */
	xwin->overlay.phase %= xwin->overlay.width;
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.grapha_picture, &overlay_trans_color, xwin->overlay.phase, 0, 1, xwin->overlay.height);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, xwin->overlay.graphb_picture, &overlay_trans_color, xwin->overlay.phase, 0, 1, xwin->overlay.height);

	/* recursively draw the monitored processes to the overlay */
	draw_overlay(vwm, xwin, xwin->monitor, &depth, &row);
}


/* this callback gets invoked at sample time for every process we've explicitly monitored (not autofollowed children/threads)
 * It's where we update the cumulative data for all windows, including the graph masks, regardless of their visibility
 * It's also where we compose the graphs and text for visible windows into a picture ready for compositing with the window contents */
static void proc_sample_callback(vmon_t *vmon, void *sys_cb_arg, vmon_proc_t *proc, void *proc_cb_arg)
{
	vwm_t		*vwm = sys_cb_arg;
	vwm_xwindow_t	*xwin = proc_cb_arg;
	//VWM_TRACE("proc=%p xwin=%p", proc, xwin);
	/* render the various always-updated overlays, this is the component we do regardless of the overlays mode and window visibility,
	 * essentially the incrementally rendered/historic components */
	maintain_overlay(vwm, xwin);

	/* if we've updated overlays for a mapped window, kick the compositor to do the costly parts of overlay drawing and compositing. */
	if (vwm_xwin_is_mapped(vwm, xwin)) vwm_composite_repaint_needed(vwm);
}


/* this callback gets invoked at sample time once "per sys" */
static void sample_callback(vmon_t *_vmon, void *sys_cb_arg)
{
	vmon_sys_stat_t	*sys_stat = vmon.stores[VMON_STORE_SYS_STAT];
	this_total =	sys_stat->user + sys_stat->nice + sys_stat->system +
			sys_stat->idle + sys_stat->iowait + sys_stat->irq +
			sys_stat->softirq + sys_stat->steal + sys_stat->guest;

	total_delta =	this_total - last_total;
	idle_delta =	sys_stat->idle - last_idle;
	iowait_delta =	sys_stat->iowait - last_iowait;
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


/* return the composed height of the overlay */
int vwm_overlay_xwin_composed_height(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	int	snowflakes = xwin->overlay.snowflakes_cnt ? 1 + xwin->overlay.snowflakes_cnt : 0; /* don't include the separator row if there are no snowflakes */

	return MIN((xwin->overlay.heirarchy_end + snowflakes) * OVERLAY_ROW_HEIGHT, xwin->attrs.height);
}

/* reset snowflakes on the specified window */
void vwm_overlay_xwin_reset_snowflakes(vwm_t *vwm, vwm_xwindow_t *xwin) {
	if (xwin->overlay.snowflakes_cnt) {
		xwin->overlay.snowflakes_cnt = 0;
		vwm_composite_damage_win(vwm, xwin);
	}
}

static void init_overlay(vwm_t *vwm) {
	static int	initialized;
	Window		bitmask;

	if (initialized) return;
	initialized = 1;

	/* initialize libvmon */
	vmon_init(&vmon, VMON_FLAG_2PASS, VMON_WANT_SYS_STAT, (VMON_WANT_PROC_STAT | VMON_WANT_PROC_FOLLOW_CHILDREN | VMON_WANT_PROC_FOLLOW_THREADS));
	vmon.proc_ctor_cb = vmon_ctor_cb;
	vmon.proc_dtor_cb = vmon_dtor_cb;
	vmon.sample_cb = sample_callback;
	vmon.sample_cb_arg = vwm;
	gettimeofday(&this_sample, NULL);

	/* get all the text and graphics stuff setup for overlays */
	overlay_font = XLoadQueryFont(VWM_XDISPLAY(vwm), OVERLAY_FIXED_FONT);

	/* create a GC for rendering the text using Xlib into the text overlay stencils */
	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, 1, OVERLAY_MASK_DEPTH);
	text_gc = XCreateGC(VWM_XDISPLAY(vwm), bitmask, 0, NULL);
	XSetForeground(VWM_XDISPLAY(vwm), text_gc, WhitePixel(VWM_XDISPLAY(vwm), VWM_XSCREENNUM(vwm)));
	XFreePixmap(VWM_XDISPLAY(vwm), bitmask);

	/* create some repeating source fill pictures for drawing through the text and graph stencils */
	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, 1, 32);
	overlay_text_fill = XRenderCreatePicture(VWM_XDISPLAY(vwm), bitmask, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_text_fill, &overlay_visible_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, 1, 32);
	overlay_shadow_fill = XRenderCreatePicture(VWM_XDISPLAY(vwm), bitmask, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_shadow_fill, &overlay_shadow_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, OVERLAY_ROW_HEIGHT, 32);
	overlay_bg_fill = XRenderCreatePicture(VWM_XDISPLAY(vwm), bitmask, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_bg_fill, &overlay_bg_color, 0, 0, 1, OVERLAY_ROW_HEIGHT);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_bg_fill, &overlay_div_color, 0, OVERLAY_ROW_HEIGHT - 1, 1, 1);

	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, 1, 32);
	overlay_snowflakes_text_fill = XRenderCreatePicture(VWM_XDISPLAY(vwm), bitmask, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_snowflakes_text_fill, &overlay_snowflakes_visible_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, 1, 32);
	overlay_grapha_fill = XRenderCreatePicture(VWM_XDISPLAY(vwm), bitmask, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_grapha_fill, &overlay_grapha_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, 1, 32);
	overlay_graphb_fill = XRenderCreatePicture(VWM_XDISPLAY(vwm), bitmask, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_graphb_fill, &overlay_graphb_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 1, 2, 32);
	overlay_finish_fill = XRenderCreatePicture(VWM_XDISPLAY(vwm), bitmask, XRenderFindStandardFormat(VWM_XDISPLAY(vwm), PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_finish_fill, &overlay_visible_color, 0, 0, 1, 1);
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, overlay_finish_fill, &overlay_trans_color, 0, 1, 1, 1);
}


/* install a monitor on the window if it doesn't already have one and has _NET_WM_PID set */
void vwm_overlay_xwin_create(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	Atom		type;
	int		fmt;
	unsigned long	nitems;
	unsigned long	nbytes;
	long		*foo = NULL;
	int		pid = -1;

	init_overlay(vwm);

	if (xwin->monitor) return;

	if (XGetWindowProperty(VWM_XDISPLAY(vwm), xwin->id, vwm->wm_pid_atom, 0, 1, False, XA_CARDINAL,
			       &type, &fmt, &nitems, &nbytes, (unsigned char **)&foo) != Success || !foo) return;

	pid = *foo;
	XFree(foo);

	/* add the client process to the monitoring heirarchy */
	/* XXX note libvmon here maintains a unique callback for each unique callback+xwin pair, so multi-window processes work */
	xwin->monitor = vmon_proc_monitor(&vmon, NULL, pid, VMON_WANT_PROC_INHERIT, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, xwin);
	 /* FIXME: count_rows() isn't returning the right count sometimes (off by ~1), it seems to be related to racing with the automatic child monitoring */
	 /* the result is an extra row sometimes appearing below the process heirarchy */
	xwin->overlay.heirarchy_end = 1 + count_rows(xwin->monitor);
	xwin->overlay.snowflakes_cnt = 0;
}


/* remove monitoring on the window if installed */
void vwm_overlay_xwin_destroy(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	if (xwin->monitor) vmon_proc_unmonitor(&vmon, xwin->monitor, (void (*)(vmon_t *, void *, vmon_proc_t *, void *))proc_sample_callback, xwin);
}


/* this composes the maintained overlay into the window's overlay picture, this gets called from paint_all() on every repaint of xwin */
/* we noop the call if the gen_last_composed and monitor->proc.generation numbers match, indicating there's nothing new to compose. */
void vwm_overlay_xwin_compose(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	XserverRegion	region;
	XRectangle	damage;
	int		height;

	if (!xwin->overlay.width) return; /* prevent winning race with maintain_overlay() and using an unready overlay... */

	if (xwin->overlay.gen_last_composed == xwin->monitor->generation) return; /* noop if no sampling occurred since last compose */
	xwin->overlay.gen_last_composed = xwin->monitor->generation; /* remember this generation */

	//VWM_TRACE("composing %p", xwin);

	height = vwm_overlay_xwin_composed_height(vwm, xwin);

	/* fill the overlay picture with the background */
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, overlay_bg_fill, None, xwin->overlay.picture,
		0, 0,
		0, 0,
		0, 0,
		xwin->attrs.width, height);

	/* draw the graphs into the overlay through the stencils being maintained by the sample callbacks */
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_grapha_fill, xwin->overlay.grapha_picture, xwin->overlay.picture,
		0, 0,
		xwin->overlay.phase, 0,
		0, 0,
		xwin->attrs.width, height);
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_graphb_fill, xwin->overlay.graphb_picture, xwin->overlay.picture,
		0, 0,
		xwin->overlay.phase, 0,
		0, 0,
		xwin->attrs.width, height);

	/* draw the shadow into the overlay picture using a translucent black source drawn through the shadow mask */
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_shadow_fill, xwin->overlay.shadow_picture, xwin->overlay.picture,
		0, 0,
		0, 0,
		0, 0,
		xwin->attrs.width, height);

	/* render overlay text into the overlay picture using a white source drawn through the overlay text as a mask, on top of everything */
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_text_fill, xwin->overlay.text_picture, xwin->overlay.picture,
		0, 0,
		0, 0,
		0, 0,
		xwin->attrs.width, (xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT));

	XRenderComposite(VWM_XDISPLAY(vwm), PictOpOver, overlay_snowflakes_text_fill, xwin->overlay.text_picture, xwin->overlay.picture,
		0, 0,
		0, xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT,
		0, xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, height - (xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT));

	/* damage the window to ensure the updated overlay is drawn (TODO: this can be done more selectively/efficiently) */
	damage.x = xwin->attrs.x + xwin->attrs.border_width;
	damage.y = xwin->attrs.y + xwin->attrs.border_width;
	damage.width = xwin->attrs.width;
	damage.height = height;
	region = XFixesCreateRegion(VWM_XDISPLAY(vwm), &damage, 1);
	vwm_composite_damage_add(vwm, region);
}

void vwm_overlay_rate_increase(vwm_t *vwm) {
	if (sampling_interval + 1 < sizeof(sampling_intervals) / sizeof(sampling_intervals[0])) sampling_interval++;
}

void vwm_overlay_rate_decrease(vwm_t *vwm) {
	if (sampling_interval >= 0) sampling_interval--;
}


/* comvenience function for returning the time delta as a seconds.fraction float */
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


void vwm_overlay_update(vwm_t *vwm, int *desired_delay) {
	static int	sampling_paused = 0;
	static int	contiguous_drops = 0;
	float		this_delta;

	init_overlay(vwm);

	gettimeofday(&maybe_sample, NULL);
	if ((sampling_interval == -1 && !sampling_paused) || /* XXX this is kind of a kludge to get the 0 Hz indicator drawn before pausing */
	    (sampling_interval != -1 && ((this_delta = delta(&maybe_sample, &this_sample)) >= sampling_intervals[sampling_interval]))) {
		vmon_sys_stat_t	*sys_stat;

		/* automatically lower the sample rate if we can't keep up with the current sample rate */
		if (sampling_interval != -1 && sampling_interval <= prev_sampling_interval &&
		    this_delta >= (sampling_intervals[sampling_interval] * 1.5)) {
			contiguous_drops++;
			/* require > 1 contiguous drops before lowering the rate, tolerates spurious one-off stalls */
			if (contiguous_drops > 2) sampling_interval--;
		} else contiguous_drops = 0;

		/* age the sys-wide sample data into "last" variables, before the new sample overwrites them. */
		last_sample = this_sample;
		this_sample = maybe_sample;
		if ((sys_stat = vmon.stores[VMON_STORE_SYS_STAT])) {
			last_user_cpu = sys_stat->user;
			last_system_cpu = sys_stat->system;
			last_total =	sys_stat->user +
					sys_stat->nice +
					sys_stat->system +
					sys_stat->idle +
					sys_stat->iowait +
					sys_stat->irq +
					sys_stat->softirq +
					sys_stat->steal +
					sys_stat->guest;

			last_idle = sys_stat->idle;
			last_iowait = sys_stat->iowait;
		}

		vmon_sample(&vmon);	/* XXX: calls proc_sample_callback() for explicitly monitored processes after sampling their descendants */
					/* XXX: also calls sample_callback() per invocation after sampling the sys wants */
		sampling_paused = (sampling_interval == -1);
		prev_sampling_interval = sampling_interval;
	}


	/* TODO: make some effort to compute how long to sleep, but this is perfectly fine for now. */
	*desired_delay = sampling_interval != -1 ? sampling_intervals[sampling_interval] * 300.0 : -1;
}
