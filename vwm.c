/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2014  Vito Caputo - <vcaputo@gnugeneration.com>
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

/* The compositing code is heavily influenced by Keith Packard's xcompmgr.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/extensions/sync.h>	/* SYNC extension, enables us to give vwm the highest X client priority, helps keep vwm responsive at all times */
#include <X11/extensions/Xinerama.h>	/* XINERAMA extension, facilitates easy multihead awareness */
#include <X11/extensions/Xrandr.h>	/* RANDR extension, facilitates display configuration change awareness */
#include <X11/extensions/Xdamage.h>	/* Damage extension, enables receipt of damage events, reports visible regions needing updating (compositing) */
#include <X11/extensions/Xrender.h>     /* Render extension, enables use of alpha channels and accelerated rendering of surfaces having alpha (compositing) */
#include <X11/extensions/Xcomposite.h>	/* Composite extension, enables off-screen redirection of window rendering (compositing) */
#include <X11/extensions/Xfixes.h>	/* XFixes extension exposes things like regions (compositing) */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <values.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <poll.h>
#include "libvmon/vmon.h"
#include "vwm.h"

#define WINDOW_BORDER_WIDTH			1
#define CONSOLE_WM_CLASS			"VWMConsoleXTerm"		/* the class we specify to the "console" xterm */
#define CONSOLE_SESSION_STRING			"_vwm_console.$DISPLAY"		/* the unique console screen session identifier */

#define WM_GRAB_MODIFIER			Mod1Mask			/* the modifier for invoking vwm's controls */
										/* Mod4Mask would be the windows key instead of Alt, but there's an assumption
										 * in the code that grabs are being activated by Alt which complicates changing it,
										 * search for XGetModifierMapping to see where, feel free to fix it.  Or you can
										 * just hack the code to expect the appropriate key instead of Alt, I didn't see the
										 * value of making it modifier mapping aware if it's always Alt for me. */
#define LAUNCHED_RELATIVE_PRIORITY		10				/* the wm priority plus this is used as the priority of launched processes */
#define HONOR_OVERRIDE_REDIRECT							/* search for HONOR_OVERRIDE_REDIRECT for understanding */


#define OVERLAY_MASK_DEPTH			8				/* XXX: 1 would save memory, but Xorg isn't good at it */
#define OVERLAY_MASK_FORMAT			PictStandardA8
#define OVERLAY_FIXED_FONT			"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1"
#define OVERLAY_ROW_HEIGHT			15				/* this should always be larger than the font height */
#define OVERLAY_GRAPH_MIN_WIDTH			200				/* always create graphs at least this large */
#define OVERLAY_GRAPH_MIN_HEIGHT		(4 * OVERLAY_ROW_HEIGHT)
#define OVERLAY_ISTHREAD_ARGV			"~"				/* use this string to mark threads in the argv field */
#define OVERLAY_NOCOMM_ARGV			"#missed it!"			/* use this string to substitute the command when missing in argv field */


typedef enum _vwm_context_focus_t {
	VWM_CONTEXT_FOCUS_OTHER = 0,						/* focus the other context relative to the current one */
	VWM_CONTEXT_FOCUS_DESKTOP,						/* focus the desktop context */
	VWM_CONTEXT_FOCUS_SHELF							/* focus the shelf context */
} vwm_context_focus_t;

typedef enum _vwm_compositing_mode_t {
	VWM_COMPOSITING_OFF = 0,						/* non-composited, no redirected windows, most efficient */
	VWM_COMPOSITING_MONITORS = 1						/* composited process monitoring overlays, slower but really useful. */
} vwm_compositing_mode_t;

typedef XineramaScreenInfo vwm_screen_t;					/* conveniently reuse the xinerama type for describing screens */

static LIST_HEAD(desktops);							/* global list of all (virtual) desktops in spatial created-in order */
static LIST_HEAD(desktops_mru);							/* global list of all (virtual) desktops in MRU order */
static LIST_HEAD(windows_mru);							/* global list of all managed windows kept in MRU order */
static LIST_HEAD(xwindows);							/* global list of all xwindows kept in the X server stacking order */
static vwm_window_t		*console = NULL;				/* the console window */
static vwm_desktop_t		*focused_desktop = NULL;			/* currently focused (virtual) desktop */
static vwm_window_t		*focused_shelf = NULL;				/* currently focused shelved window */
static vwm_context_focus_t	focused_context = VWM_CONTEXT_FOCUS_DESKTOP;	/* currently focused context */ 
static vwm_compositing_mode_t	compositing_mode = VWM_COMPOSITING_OFF;		/* current compositing mode */
static int			key_is_grabbed = 0;				/* flag for tracking keyboard grab state */
static int			priority;					/* scheduling priority of the vwm process, launcher nices relative to this */
static unsigned long		fence_mask = 0;					/* global mask state for vwm_win_focus_next(... VWM_FENCE_MASKED_VIOLATE),
										 * if you use vwm on enough screens to overflow this, pics or it didn't happen. */

	/* Uninteresting stuff */
static Display			*display;
static Colormap			cmap;

#define color(_sym, _str) \
static XColor			_sym ## _color;
#include "colors.def"
#undef color

static int			screen_num;
static GC			gc;
static Atom			wm_delete_atom;
static Atom			wm_protocols_atom;
static Atom                     wm_pid_atom;
static int			sync_event, sync_error;

	/* Xinerama */
static int			xinerama_event, xinerama_error;
static XineramaScreenInfo	*xinerama_screens = NULL;
static int			xinerama_screens_cnt;
static int			randr_event, randr_error;

	/* Compositing */
static int			composite_event, composite_error, composite_opcode;
static int			damage_event, damage_error;
static XserverRegion            combined_damage = None;
static Picture                  root_picture = None, root_buffer = None;        /* compositing gets double buffered */
static XWindowAttributes	root_attrs;
static XRenderPictureAttributes pa_inferiors = { .subwindow_mode = IncludeInferiors };

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
				overlay_grapha_color  = { 0xff00, 0x0000, 0x0000, 0x3000 },	/* ~red */
				overlay_graphb_color = { 0x0000, 0xffff, 0xffff, 0x3000 };	/* ~cyan */

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

	/* some needed prototypes */
static vwm_xwindow_t * vwm_xwin_lookup(Window win);
static inline int vwm_xwin_is_visible(vwm_xwindow_t *xwin);
static void vwm_comp_damage_add(XserverRegion damage);
static vwm_xwindow_t * vwm_win_unmanage(vwm_window_t *vwin);
static vwm_window_t * vwm_win_manage_xwin(vwm_xwindow_t *xwin);
static void vwm_win_unmap(vwm_window_t *vwin);
static void vwm_win_map(vwm_window_t *vwin);
static void vwm_win_focus(vwm_window_t *vwin);
static void vwm_keypressed(Window win, XEvent *keypress);

#define MIN(_a, _b)	((_a) < (_b) ? (_a) : (_b))
#define MAX(_a, _b)	((_a) > (_b) ? (_a) : (_b))


	/* libvmon integration, warning: this gets a little crazy especially in the rendering. */

/* space we need for every process being monitored */
typedef struct _vwm_perproc_ctxt_t {
	typeof(vmon.generation)			generation;
	typeof(((vmon_proc_stat_t *)0)->utime)	last_utime;
	typeof(((vmon_proc_stat_t *)0)->stime)	last_stime;
	typeof(((vmon_proc_stat_t *)0)->utime)	utime_delta;
	typeof(((vmon_proc_stat_t *)0)->stime)	stime_delta;
} vwm_perproc_ctxt_t;


/* moves what's below a given row up above it if specified, the row becoming discarded */
static void snowflake_row(vwm_xwindow_t *xwin, Picture pic, int copy, int row)
{
	VWM_TRACE("pid=%i xwin=%p row=%i copy=%i heirarhcy_end=%i", xwin->monitor->pid, xwin, row, copy, xwin->overlay.heirarchy_end);

	if(copy) {
		/* copy row to tmp */
		XRenderComposite(display, PictOpSrc, pic, None, xwin->overlay.tmp_picture,
			0, row * OVERLAY_ROW_HEIGHT,			/* src */
			0, 0,						/* mask */
			0, 0,						/* dest */
			xwin->overlay.width, OVERLAY_ROW_HEIGHT);	/* dimensions */
	}

	/* shift up */
	XRenderChangePicture(display, pic, CPRepeat, &pa_no_repeat);
	XRenderComposite(display, PictOpSrc, pic, None, pic,
		0, (1 + row) * OVERLAY_ROW_HEIGHT,									/* src */
		0, 0,													/* mask */
		0, row * OVERLAY_ROW_HEIGHT,										/* dest */
		xwin->overlay.width, (1 + xwin->overlay.heirarchy_end) * OVERLAY_ROW_HEIGHT - (1 + row) * OVERLAY_ROW_HEIGHT);	/* dimensions */
	XRenderChangePicture(display, pic, CPRepeat, &pa_repeat);

	if(copy) {
		/* copy tmp to top of snowflakes */
		XRenderComposite(display, PictOpSrc, xwin->overlay.tmp_picture, None, pic,
			0, 0,									/* src */
			0, 0,									/* mask */
			0, (xwin->overlay.heirarchy_end) * OVERLAY_ROW_HEIGHT,			/* dest */
			xwin->overlay.width, OVERLAY_ROW_HEIGHT);				/* dimensions */
	} else {
		/* clear the snowflake row */
		XRenderFillRectangle(display, PictOpSrc, pic, &overlay_trans_color,
			0, (xwin->overlay.heirarchy_end) * OVERLAY_ROW_HEIGHT,			/* dest */
			xwin->overlay.width, OVERLAY_ROW_HEIGHT);				/* dimensions */
	}
}

/* XXX TODO libvmon automagic children following races with explicit X client pid monitoring with different outcomes, it should be irrelevant which wins,
 *     currently the only visible difference is the snowflakes gap (heirarchy_end) varies, which is why I haven't bothered to fix it, I barely even notice.
 */

/* shifts what's below a given row down a row, and clears the row, preparing it for populating */
static void allocate_row(vwm_xwindow_t *xwin, Picture pic, int row)
{
	VWM_TRACE("pid=%i xwin=%p row=%i", xwin->monitor->pid, xwin, row);

	/* shift everything below the row down */
	XRenderComposite(display, PictOpSrc, pic, None, pic,
		0, row * OVERLAY_ROW_HEIGHT, 							/* src */
		0, 0, 										/* mask */
		0, (1 + row) * OVERLAY_ROW_HEIGHT,						/* dest */
		xwin->overlay.width, xwin->overlay.height - (1 + row) * OVERLAY_ROW_HEIGHT);	/* dimensions */
	/* fill the space created with transparent pixels */
	XRenderFillRectangle(display, PictOpSrc, pic, &overlay_trans_color,
		0, row * OVERLAY_ROW_HEIGHT,							/* dest */
		xwin->overlay.width, OVERLAY_ROW_HEIGHT);					/* dimensions */
}


/* shadow a row from the text layer in the shadow layer */
static void shadow_row(vwm_xwindow_t *xwin, int row)
{
	/* the current technique for creating the shadow is to simply render the text at +1/-1 pixel offsets on both axis in translucent black */
	XRenderComposite(display, PictOpSrc, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		-1, row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(display, PictOpOver, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		0, -1 + row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(display, PictOpOver, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		1, row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);

	XRenderComposite(display, PictOpOver, overlay_shadow_fill, xwin->overlay.text_picture, xwin->overlay.shadow_picture,
		0, 0,
		0, 1 + row * OVERLAY_ROW_HEIGHT,
		0, row * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, OVERLAY_ROW_HEIGHT);
}


/* simple helper to map the vmon per-proc argv array into an XTextItem array, deals with threads vs. processes and the possibility of the comm field not getting read in before the process exited... */
static void argv2xtext(vmon_proc_t *proc, XTextItem *items, int *nr_items)	/* XXX TODO: reallocate items when too small... */
{
	int	i;
	int	nr = 0;

	if(proc->is_thread) {	/* stick the thread marker at the start of threads */
		items[0].nchars = sizeof(OVERLAY_ISTHREAD_ARGV) - 1;
		items[0].chars =  OVERLAY_ISTHREAD_ARGV;
		items[0].delta = 4;
		items[0].font = None;
		nr++;
	}

	if(((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len) {
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

	for(i = 1; i < ((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->argc; nr++, i++) {
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

	if(!proc->is_thread) {
		list_for_each_entry(child, &proc->threads, threads) {
			count += count_rows(child);
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		count += count_rows(child);
	}

	return count;
}


/* recursive draw function for the consolidated version of the overlay rendering which also implements snowflakes */
static void draw_overlay(vwm_xwindow_t *xwin, vmon_proc_t *proc, int *depth, int *row)
{
	vmon_proc_t		*child;
	vwm_perproc_ctxt_t	*proc_ctxt = proc->foo;
	vmon_proc_stat_t	*proc_stat = proc->stores[VMON_STORE_PROC_STAT];

	/* graph variables */
	int			a_height, b_height;
	double			utime_delta, stime_delta;

	/* text variables */
	char			str[256];
	int			str_len;
	XTextItem		items[1024]; /* XXX TODO: dynamically allocate this and just keep it at the high water mark.. create a struct to encapsulate this, nr_items, and alloc_items... */
	int			nr_items;
	int			direction, ascent, descent;
	XCharStruct		charstruct;

	if((*row)) { /* except row 0 (Idle/IOWait graph), handle any stale and new processes/threads */
		if(proc->is_stale) {
			/* what to do when a process (subtree) has gone away */
			static int	in_stale = 0;
			int		in_stale_entrypoint = 0;

			/* I snowflake the stale processes from the leaves up for a more intuitive snowflake order...
			 * (I expect the command at the root of the subtree to appear at the top of the snowflakes...) */
			/* This does require that I do a separate forward recursion to determine the number of rows
			 * so I  can correctly snowflake in reverse */
			if(!in_stale) {
				VWM_TRACE("entered stale at xwin=%p depth=%i row=%i", xwin, *depth, *row);
				in_stale_entrypoint = in_stale = 1;
				(*row) += count_rows(proc) - 1;
			}

			(*depth)++;
			list_for_each_entry_prev(child, &proc->children, siblings) {
				draw_overlay(xwin, child, depth, row);
				(*row)--;
			}

			if(!proc->is_thread) {
				list_for_each_entry_prev(child, &proc->threads, threads) {
					draw_overlay(xwin, child, depth, row);
					(*row)--;
				}
			}
			(*depth)--;

			VWM_TRACE("%i (%.*s) is stale @ depth %i row %i is_thread=%i", proc->pid,
				((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.len - 1,
				((vmon_proc_stat_t *)proc->stores[VMON_STORE_PROC_STAT])->comm.array,
				(*depth), (*row), proc->is_thread);

			/* stamp the graphs with the finish line */
			XRenderComposite(display, PictOpSrc, overlay_finish_fill, None, xwin->overlay.grapha_picture,
					 0, 0,							/* src x, y */
					 0, 0,							/* mask x, y */
					 xwin->overlay.phase, (*row) * OVERLAY_ROW_HEIGHT,	/* dst x, y */ 
					 1, OVERLAY_ROW_HEIGHT - 1);
			XRenderComposite(display, PictOpSrc, overlay_finish_fill, None, xwin->overlay.graphb_picture,
					 0, 0,							/* src x, y */
					 0, 0,							/* mask x, y */
					 xwin->overlay.phase, (*row) * OVERLAY_ROW_HEIGHT,	/* dst x, y */
					 1, OVERLAY_ROW_HEIGHT - 1);

			/* extract the row from the various layers */
			snowflake_row(xwin, xwin->overlay.grapha_picture, 1, (*row));
			snowflake_row(xwin, xwin->overlay.graphb_picture, 1, (*row));
			snowflake_row(xwin, xwin->overlay.text_picture, 0, (*row));
			snowflake_row(xwin, xwin->overlay.shadow_picture, 0, (*row));
			xwin->overlay.snowflakes_cnt++;

			/* stamp the name (and whatever else we include) into overlay.text_picture */
			argv2xtext(proc, items, &nr_items);
			XDrawText(display, xwin->overlay.text_pixmap, text_gc,
				  5, (xwin->overlay.heirarchy_end + 1) * OVERLAY_ROW_HEIGHT - 3,/* dst x, y */
				  items, nr_items);
			shadow_row(xwin, xwin->overlay.heirarchy_end);

			xwin->overlay.heirarchy_end--;

			if(in_stale_entrypoint) {
				VWM_TRACE("exited stale at xwin=%p depth=%i row=%i", xwin, *depth, *row);
				in_stale = 0;
			}

			return;
		} else if(proc->is_new) {
			/* what to do when a process has been introduced */
			VWM_TRACE("%i is new", proc->pid);

			allocate_row(xwin, xwin->overlay.grapha_picture, (*row));
			allocate_row(xwin, xwin->overlay.graphb_picture, (*row));
			allocate_row(xwin, xwin->overlay.text_picture, (*row));
			allocate_row(xwin, xwin->overlay.shadow_picture, (*row));

			xwin->overlay.heirarchy_end++;
		}
	}

/* CPU utilization graphs */
	if(!(*row)) {
		/* XXX: sortof kludged in IOWait and Idle % @ row 0 */
		stime_delta = iowait_delta;
		utime_delta = idle_delta;
	} else {
		/* use the generation number to avoid recomputing this stuff for callbacks recurring on the same process in the same sample */
		if(proc_ctxt->generation != vmon.generation) {
			proc_ctxt->stime_delta = proc_stat->stime - proc_ctxt->last_stime;
			proc_ctxt->utime_delta = proc_stat->utime - proc_ctxt->last_utime;
			proc_ctxt->last_utime = proc_stat->utime;
			proc_ctxt->last_stime = proc_stat->stime;

			proc_ctxt->generation = vmon.generation;
		}

		if(proc->is_new) {
			/* we need a minimum of two samples before we can compute a delta to plot,
			 * so we suppress that and instead mark the start of monitoring with an impossible 100% of both graph contexts, a starting line. */
			stime_delta = utime_delta = total_delta;
		} else {
			stime_delta = proc_ctxt->stime_delta;
			utime_delta = proc_ctxt->utime_delta;
		}
	}

	/* compute the bar heights for this sample */
	a_height = (stime_delta / total_delta * (double)(OVERLAY_ROW_HEIGHT - 1)); /* give up 1 pixel for the div */
	b_height = (utime_delta / total_delta * (double)(OVERLAY_ROW_HEIGHT - 1));

	/* round up to 1 pixel when the scaled result is a fraction less than 1,
	 * I want to at least see 1 pixel blips for the slightest cpu utilization */
	if(stime_delta && !a_height) a_height = 1;
	if(utime_delta && !b_height) b_height = 1;

	/* draw the two bars for this sample at the current phase in the graphs, note the first is ceiling-based, second floor-based */
	XRenderFillRectangle(display, PictOpSrc, xwin->overlay.grapha_picture, &overlay_visible_color,
		xwin->overlay.phase, (*row) * OVERLAY_ROW_HEIGHT,					/* dst x, y */
		1, a_height);										/* dst w, h */
	XRenderFillRectangle(display, PictOpSrc, xwin->overlay.graphb_picture, &overlay_visible_color,
		xwin->overlay.phase, (*row) * OVERLAY_ROW_HEIGHT + (OVERLAY_ROW_HEIGHT - b_height) - 1,	/* dst x, y */
		1, b_height);										/* dst w, h */

	if(!(*row)) {
		/* here's where the Idle/IOWait row drawing concludes */
		if(compositing_mode) {
			snprintf(str, sizeof(str), "\\/\\/\\    %2iHz %n", (int)(sampling_interval < 0 ? 0 : 1 / sampling_intervals[sampling_interval]), &str_len);
			/* TODO: I clear and redraw this row every time, which is unnecessary, small optimization would be to only do so when:
			 * - overlay resized, and then constrain the clear to the affected width
			 * - Hz changed
			 */
			XRenderFillRectangle(display, PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color,
				0, 0,					/* dst x, y */
				xwin->attrs.width, OVERLAY_ROW_HEIGHT);	/* dst w, h */
			XTextExtents(overlay_font, str, str_len, &direction, &ascent, &descent, &charstruct);
			XDrawString(display, xwin->overlay.text_pixmap, text_gc,
				    xwin->attrs.width - charstruct.width, OVERLAY_ROW_HEIGHT - 3,		/* dst x, y */
				    str, str_len);
			shadow_row(xwin, 0);
		}
		(*row)++;
		draw_overlay(xwin, proc, depth, row);
		return;
	}

/* process heirarchy text and accompanying per-process details like wchan/pid/state... */
	if(compositing_mode) {	/* this stuff can be skipped when monitors aren't visible */
		/* TODO: make the columns interactively configurable @ runtime */
		if(!proc->is_new) {
		/* XXX for now always clear the row, this should be capable of being optimized in the future (if the datums driving the text haven't changed...) */
			XRenderFillRectangle(display, PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color,
				0, (*row) * OVERLAY_ROW_HEIGHT,			/* dst x, y */
				xwin->overlay.width, OVERLAY_ROW_HEIGHT);	/* dst w, h */
		}

		/* put the process' wchan, state, and PID columns @ the far right */
		if(proc->is_thread || list_empty(&proc->threads)) {	/* only threads or non-threaded processes include the wchan and state */
			snprintf(str, sizeof(str), "   %.*s %5i %c %n",
				proc_stat->wchan.len,
				proc_stat->wchan.len == 1 && proc_stat->wchan.array[0] == '0' ? "-" : proc_stat->wchan.array,
				proc->pid,
				proc_stat->state,
				&str_len);
		} else { /* we're a process having threads, suppress the wchan and state, as they will be displayed for the thread of same pid */
			snprintf(str, sizeof(str), "  %5i   %n", proc->pid, &str_len);
		}

		XTextExtents(overlay_font, str, str_len, &direction, &ascent, &descent, &charstruct);

		/* the process' comm label indented according to depth, followed with their respective argv's  */
		argv2xtext(proc, items, &nr_items);
		XDrawText(display, xwin->overlay.text_pixmap, text_gc,
			  (*depth) * (OVERLAY_ROW_HEIGHT / 2), ((*row) + 1) * OVERLAY_ROW_HEIGHT - 3,			/* dst x, y */
			  items, nr_items);

		/* ensure the area for the rest of the stuff is cleared, we don't put much text into thread rows so skip it for those. */
		if(!proc->is_thread) {
			XRenderFillRectangle(display, PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color,
				xwin->attrs.width - charstruct.width, (*row) * OVERLAY_ROW_HEIGHT,			/* dst x,y */
				xwin->overlay.width - (xwin->attrs.width - charstruct.width), OVERLAY_ROW_HEIGHT);	/* dst w,h */
		}

		XDrawString(display, xwin->overlay.text_pixmap, text_gc,
			    xwin->attrs.width - charstruct.width, ((*row) + 1) * OVERLAY_ROW_HEIGHT - 3,		/* dst x, y */
			    str, str_len);

		/* only if this process isn't the root process @ the window shall we consider all relational drawing conditions */
		if(proc != xwin->monitor) {
			vmon_proc_t		*ancestor, *sibling, *last_sibling = NULL;
			struct list_head	*rem;
			int			needs_tee = 0;
			int			bar_x = 0, bar_y = 0;
			int			sub;

			/* XXX: everything done in this code block only dirties _this_ process' row in the rendered overlay output */

			/* walk up the ancestors until reaching xwin->monitor, any ancestors we encounter which have more siblings we draw a vertical bar for */
			/* this draws the |'s in something like:  | |   |    | comm */
			for(sub = 1, ancestor = proc->parent; ancestor && ancestor != xwin->monitor; ancestor = ancestor->parent) {
				sub++;
				bar_x = ((*depth) - sub) * (OVERLAY_ROW_HEIGHT / 2) + 4;
				bar_y = ((*row) + 1) * OVERLAY_ROW_HEIGHT;

				/* determine if the ancestor has remaining siblings which are not stale, if so, draw a connecting bar at its depth */
				for(rem = ancestor->siblings.next; rem != &ancestor->parent->children; rem = rem->next) {
					if(!(list_entry(rem, vmon_proc_t, siblings)->is_stale)) {
						XDrawLine(display, xwin->overlay.text_pixmap, text_gc,
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
				if(!sibling->is_stale) last_sibling = sibling;
			}

			/* now look for siblings with non-stale children to determine if a tee is needed, ignoring the last sibling */
			list_for_each_entry(sibling, &proc->parent->children, siblings) {
				/* skip stale siblings, they aren't interesting as they're invisible, and the last sibling has no bearing on wether we tee or not. */
				if(sibling->is_stale || sibling == last_sibling) continue;

				/* if any of the other siblings have children which are not stale, put a tee in front of our name, but ignore stale children */
				list_for_each_entry(child, &sibling->children, siblings) {
					if(!child->is_stale) {
						needs_tee = 1;
						break;
					}
				}

				/* if we still don't think we need a tee, check if there are threads */
				if(!needs_tee) {
					list_for_each_entry(child, &sibling->threads, threads) {
						if(!child->is_stale) {
							needs_tee = 1;
							break;
						}
					}
				}

				/* found a tee is necessary, all that's left is to determine if the tee is a corner and draw it accordingly, stopping the search. */
				if(needs_tee) {
					bar_x = ((*depth) - 1) * (OVERLAY_ROW_HEIGHT / 2) + 4;

					/* if we're the last sibling, corner the tee by shortening the vbar */
					if(proc == last_sibling) {
						XDrawLine(display, xwin->overlay.text_pixmap, text_gc,
							  bar_x, bar_y - OVERLAY_ROW_HEIGHT,	/* dst x1, y1 */
							  bar_x, bar_y - 4);			/* dst x2, y2 (vertical bar) */
					} else {
						XDrawLine(display, xwin->overlay.text_pixmap, text_gc,
							  bar_x, bar_y - OVERLAY_ROW_HEIGHT,	/* dst x1, y1 */
							  bar_x, bar_y);			/* dst x2, y2 (vertical bar) */
					}

					XDrawLine(display, xwin->overlay.text_pixmap, text_gc,
						  bar_x, bar_y - 4,				/* dst x1, y1 */
						  bar_x + 2, bar_y - 4);			/* dst x2, y2 (horizontal bar) */

					/* terminate the outer sibling loop upon drawing the tee... */
					break;
				}
			}
		}
		shadow_row(xwin, (*row));
	}

	(*row)++;

	/* recur any threads first, then any children processes */
	(*depth)++;
	if(!proc->is_thread) {	/* XXX: the threads member serves as the list head only when not a thread */
		list_for_each_entry(child, &proc->threads, threads) {
			draw_overlay(xwin, child, depth, row);
		}
	}

	list_for_each_entry(child, &proc->children, siblings) {
		draw_overlay(xwin, child, depth, row);
	}
	(*depth)--;
}


/* consolidated version of overlay text and graph rendering, makes snowflakes integration cleaner, this always gets called regadless of the overlays mode */
static void maintain_overlay(vwm_xwindow_t *xwin)
{
	int	row = 0, depth = 0;

	if(!xwin->monitor || !xwin->monitor->stores[VMON_STORE_PROC_STAT]) return;

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

	/* if the window is larger than the overlays currently are, enlarge them */
	if(xwin->attrs.width > xwin->overlay.width || xwin->attrs.height > xwin->overlay.height) {
		vwm_overlay_t	existing;
		Pixmap		pixmap;

		existing = xwin->overlay;

		xwin->overlay.width =  MAX(xwin->overlay.width, MAX(xwin->attrs.width, OVERLAY_GRAPH_MIN_WIDTH));
		xwin->overlay.height = MAX(xwin->overlay.height, MAX(xwin->attrs.height, OVERLAY_GRAPH_MIN_HEIGHT));

		pixmap = XCreatePixmap(display, RootWindow(display, screen_num), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.grapha_picture = XRenderCreatePicture(display, pixmap, XRenderFindStandardFormat(display, OVERLAY_MASK_FORMAT), CPRepeat, &pa_repeat);
		XFreePixmap(display, pixmap);
		XRenderFillRectangle(display, PictOpSrc, xwin->overlay.grapha_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(display, RootWindow(display, screen_num), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.graphb_picture = XRenderCreatePicture(display, pixmap, XRenderFindStandardFormat(display, OVERLAY_MASK_FORMAT), CPRepeat, &pa_repeat);
		XFreePixmap(display, pixmap);
		XRenderFillRectangle(display, PictOpSrc, xwin->overlay.graphb_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(display, RootWindow(display, screen_num), xwin->overlay.width, OVERLAY_ROW_HEIGHT, OVERLAY_MASK_DEPTH);
		xwin->overlay.tmp_picture = XRenderCreatePicture(display, pixmap, XRenderFindStandardFormat(display, OVERLAY_MASK_FORMAT), 0, NULL);
		XFreePixmap(display, pixmap);

		/* keep the text_pixmap reference around for XDrawText usage */
		xwin->overlay.text_pixmap = XCreatePixmap(display, RootWindow(display, screen_num), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.text_picture = XRenderCreatePicture(display, xwin->overlay.text_pixmap, XRenderFindStandardFormat(display, OVERLAY_MASK_FORMAT), 0, NULL);
		XRenderFillRectangle(display, PictOpSrc, xwin->overlay.text_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(display, RootWindow(display, screen_num), xwin->overlay.width, xwin->overlay.height, OVERLAY_MASK_DEPTH);
		xwin->overlay.shadow_picture = XRenderCreatePicture(display, pixmap, XRenderFindStandardFormat(display, OVERLAY_MASK_FORMAT), 0, NULL);
		XFreePixmap(display, pixmap);
		XRenderFillRectangle(display, PictOpSrc, xwin->overlay.shadow_picture, &overlay_trans_color, 0, 0, xwin->overlay.width, xwin->overlay.height);

		pixmap = XCreatePixmap(display, RootWindow(display, screen_num), xwin->overlay.width, xwin->overlay.height, 32);
		xwin->overlay.picture = XRenderCreatePicture(display, pixmap, XRenderFindStandardFormat(display, PictStandardARGB32), 0, NULL);
		XFreePixmap(display, pixmap);

		if(existing.width) {
			/* XXX: note the graph pictures are copied from their current phase in the x dimension */
			XRenderComposite(display, PictOpSrc, existing.grapha_picture, None, xwin->overlay.grapha_picture,
				existing.phase, 0,	/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(display, PictOpSrc, existing.graphb_picture, None, xwin->overlay.graphb_picture,
				existing.phase, 0,	/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(display, PictOpSrc, existing.text_picture, None, xwin->overlay.text_picture,
				0, 0,			/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(display, PictOpSrc, existing.shadow_picture, None, xwin->overlay.shadow_picture,
				0, 0,			/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			XRenderComposite(display, PictOpSrc, existing.picture, None, xwin->overlay.picture,
				0, 0,			/* src x, y */
				0, 0,			/* mask x, y */
				0, 0,			/* dest x, y */
				existing.width, existing.height);
			xwin->overlay.phase = 0;	/* having unrolled the existing graph[ab] pictures into the larger ones, phase is reset to 0 */
			XRenderFreePicture(display, existing.grapha_picture);
			XRenderFreePicture(display, existing.graphb_picture);
			XRenderFreePicture(display, existing.tmp_picture);
			XRenderFreePicture(display, existing.text_picture);
			XFreePixmap(display, existing.text_pixmap);
			XRenderFreePicture(display, existing.shadow_picture);
			XRenderFreePicture(display, existing.picture);
		}
	}

	xwin->overlay.phase += (xwin->overlay.width - 1);  /* simply change this to .phase++ to scroll the other direction */
	xwin->overlay.phase %= xwin->overlay.width;
	XRenderFillRectangle(display, PictOpSrc, xwin->overlay.grapha_picture, &overlay_trans_color, xwin->overlay.phase, 0, 1, xwin->overlay.height);
	XRenderFillRectangle(display, PictOpSrc, xwin->overlay.graphb_picture, &overlay_trans_color, xwin->overlay.phase, 0, 1, xwin->overlay.height);

	/* recursively draw the monitored processes to the overlay */
	draw_overlay(xwin, xwin->monitor, &depth, &row);
}


/* return the composed height of the overlay */
static int overlay_composed_height(vwm_xwindow_t *xwin)
{
	int	snowflakes = xwin->overlay.snowflakes_cnt ? 1 + xwin->overlay.snowflakes_cnt : 0; /* don't include the separator row if there are no snowflakes */
	return MIN((xwin->overlay.heirarchy_end + snowflakes) * OVERLAY_ROW_HEIGHT, xwin->attrs.height);
}


/* this composes the maintained overlay into the window's overlay picture, this gets called from paint_all() on every repaint of xwin */
/* we noop the call if the gen_last_composed and monitor->proc.generation numbers match, indicating there's nothing new to compose. */
static void compose_overlay(vwm_xwindow_t *xwin)
{
	XserverRegion	region;
	XRectangle	damage;
	int		height;

	if(!xwin->overlay.width) return; /* prevent winning race with maintain_overlay() and using an unready overlay... */

	if(xwin->overlay.gen_last_composed == xwin->monitor->generation) return; /* noop if no sampling occurred since last compose */
	xwin->overlay.gen_last_composed = xwin->monitor->generation; /* remember this generation */

	//VWM_TRACE("composing %p", xwin);

	height = overlay_composed_height(xwin);

	/* fill the overlay picture with the background */
	XRenderComposite(display, PictOpSrc, overlay_bg_fill, None, xwin->overlay.picture,
		0, 0,
		0, 0,
		0, 0,
		xwin->attrs.width, height);

	/* draw the graphs into the overlay through the stencils being maintained by the sample callbacks */
	XRenderComposite(display, PictOpOver, overlay_grapha_fill, xwin->overlay.grapha_picture, xwin->overlay.picture,
		0, 0,
		xwin->overlay.phase, 0,
		0, 0,
		xwin->attrs.width, height);
	XRenderComposite(display, PictOpOver, overlay_graphb_fill, xwin->overlay.graphb_picture, xwin->overlay.picture,
		0, 0,
		xwin->overlay.phase, 0,
		0, 0,
		xwin->attrs.width, height);

	/* draw the shadow into the overlay picture using a translucent black source drawn through the shadow mask */
	XRenderComposite(display, PictOpOver, overlay_shadow_fill, xwin->overlay.shadow_picture, xwin->overlay.picture,
		0, 0,
		0, 0,
		0, 0,
		xwin->attrs.width, height);

	/* render overlay text into the overlay picture using a white source drawn through the overlay text as a mask, on top of everything */
	XRenderComposite(display, PictOpOver, overlay_text_fill, xwin->overlay.text_picture, xwin->overlay.picture,
		0, 0,
		0, 0,
		0, 0,
		xwin->attrs.width, (xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT));

	XRenderComposite(display, PictOpOver, overlay_snowflakes_text_fill, xwin->overlay.text_picture, xwin->overlay.picture,
		0, 0,
		0, xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT,
		0, xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT,
		xwin->attrs.width, height - (xwin->overlay.heirarchy_end * OVERLAY_ROW_HEIGHT));

	/* damage the window to ensure the updated overlay is drawn (TODO: this can be done more selectively/efficiently) */
	damage.x = xwin->attrs.x + xwin->attrs.border_width;
	damage.y = xwin->attrs.y + xwin->attrs.border_width;
	damage.width = xwin->attrs.width;
	damage.height = height;
	region = XFixesCreateRegion(display, &damage, 1);
	vwm_comp_damage_add(region);
}


/* this callback gets invoked at sample time for every process we've explicitly monitored (not autofollowed children/threads)
 * It's where we update the cumulative data for all windows, including the graph masks, regardless of their visibility
 * It's also where we compose the graphs and text for visible windows into a picture ready for compositing with the window contents */
static void proc_sample_callback(vmon_t *vmon, vmon_proc_t *proc, vwm_xwindow_t *xwin)
{
	//VWM_TRACE("proc=%p xwin=%p", proc, xwin);
	/* render the various always-updated overlays, this is the component we do regardless of the overlays mode and window visibility,
	 * essentially the incrementally rendered/historic components */
	maintain_overlay(xwin);

	/* render other non-historic things and compose the various layers into an updated overlay */
	/* this leaves everything ready to be composed with the window contents in paint_all() */
	/* paint_all() also enters compose_overlay() to update the overlays on windows which become visible (desktop switches) */
	if(compositing_mode && vwm_xwin_is_visible(xwin)) compose_overlay(xwin);
}


/* this callback gets invoked at sample time once "per sys" */
static void sample_callback(vmon_t *_vmon)
{
	vmon_sys_stat_t	*sys_stat = vmon.stores[VMON_STORE_SYS_STAT];
	this_total = 	sys_stat->user + sys_stat->nice + sys_stat->system +
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
	if(proc->foo) {
		free(proc->foo);
		proc->foo = NULL;
	}
}


	/* Xinerama/multihead screen functions */

/* return what fraction (0.0-1.0) of vwin overlaps with scr */
static float vwm_screen_overlaps_xwin(const vwm_screen_t *scr, vwm_xwindow_t *xwin)
{
	float	pct = 0, xover = 0, yover = 0;

	if(scr->x_org + scr->width < xwin->attrs.x || scr->x_org > xwin->attrs.x + xwin->attrs.width ||
	   scr->y_org + scr->height < xwin->attrs.y || scr->y_org > xwin->attrs.y + xwin->attrs.height)
	   	goto _out;

	/* they overlap, by how much? */
	xover = MIN(scr->x_org + scr->width, xwin->attrs.x + xwin->attrs.width) - MAX(scr->x_org, xwin->attrs.x);
	yover = MIN(scr->y_org + scr->height, xwin->attrs.y + xwin->attrs.height) - MAX(scr->y_org, xwin->attrs.y);

	pct = (xover * yover) / (xwin->attrs.width * xwin->attrs.height);
_out:
	VWM_TRACE("xover=%f yover=%f width=%i height=%i pct=%.4f", xover, yover, xwin->attrs.width, xwin->attrs.height, pct);
	return pct;
}


/* return the appropriate screen, don't use the return value across event loops because randr events reallocate the array. */
typedef enum _vwm_screen_rel_t {
	VWM_SCREEN_REL_XWIN,	/* return the screen the supplied window most resides in */
	VWM_SCREEN_REL_POINTER,	/* return the screen the pointer resides in */
	VWM_SCREEN_REL_TOTAL,	/* return the bounding rectangle of all screens as one */
} vwm_screen_rel_t;

static const vwm_screen_t * vwm_screen_find(vwm_screen_rel_t rel, ...)
{
	static vwm_screen_t	faux;
	vwm_screen_t		*scr, *best = &faux; /* default to faux as best */
	int			i;

	faux.screen_number = 0;
	faux.x_org = 0;
	faux.y_org = 0;
	faux.width = WidthOfScreen(DefaultScreenOfDisplay(display));
	faux.height = HeightOfScreen(DefaultScreenOfDisplay(display));

	if(!xinerama_screens) goto _out;

#define for_each_screen(_tmp) \
	for(i = 0, _tmp = xinerama_screens; i < xinerama_screens_cnt; _tmp = &xinerama_screens[++i])

	switch(rel) {
		case VWM_SCREEN_REL_XWIN: {
			va_list		ap;
			vwm_xwindow_t	*xwin;
			float		best_pct = 0, this_pct;

			va_start(ap, rel);
			xwin = va_arg(ap, vwm_xwindow_t *);
			va_end(ap);

			for_each_screen(scr) {
				this_pct = vwm_screen_overlaps_xwin(scr, xwin);
				if(this_pct > best_pct) {
					best = scr;
					best_pct = this_pct;
				}
			}
			break;
		}

		case VWM_SCREEN_REL_POINTER: {
			int		root_x, root_y, win_x, win_y;
			unsigned int	mask;
			Window		root, child;

			/* get the pointer coordinates and find which screen it's in */
			XQueryPointer(display, RootWindow(display, screen_num), &root, &child, &root_x, &root_y, &win_x, &win_y, &mask);

			for_each_screen(scr) {
				if(root_x >= scr->x_org && root_x < scr->x_org + scr->width &&
				   root_y >= scr->y_org && root_y < scr->y_org + scr->height) {
					best = scr;
					break;
				}
			}
			break;
		}

		case VWM_SCREEN_REL_TOTAL: {
			short	x1 = MAXSHORT, y1 = MAXSHORT, x2 = MINSHORT, y2 = MINSHORT;
			/* find the smallest x_org and y_org, the highest x_org + width and y_org + height, those are the two corners of the total rect */
			for_each_screen(scr) {
				if(scr->x_org < x1) x1 = scr->x_org;
				if(scr->y_org < y1) y1 = scr->y_org;
				if(scr->x_org + scr->width > x2) x2 = scr->x_org + scr->width;
				if(scr->y_org + scr->height > y2) y2 = scr->y_org + scr->height;
			}
			faux.x_org = x1;
			faux.y_org = y1;
			faux.width = x2 - x1;
			faux.height = y2 - y1;
			best = &faux;
			break;
		}
	}
_out:
	VWM_TRACE("Found Screen #%i: %hix%hi @ %hi,%hi", best->screen_number, best->width, best->height, best->x_org, best->y_org);

	return best;
}


/* check if a screen contains any windows (assuming the current desktop) */
static int vwm_screen_is_empty(const vwm_screen_t *scr)
{
	vwm_xwindow_t	*xwin;
	int		is_empty = 1;

	list_for_each_entry(xwin, &xwindows, xwindows) {
		if(!xwin->mapped) continue;
		if(!xwin->managed || (xwin->managed->desktop == focused_desktop && !xwin->managed->shelved && !xwin->managed->configuring)) {
			/* XXX: it may make more sense to see what %age of the screen is overlapped by windows, and consider it empty if < some % */
			/*      This is just seeing if any window is predominantly within the specified screen, the rationale being if you had a focusable
			 *      window on the screen you would have used the keyboard to make windows go there; this function is only used in determining
			 *      wether a new window should go where the pointer is or not. */
			if(vwm_screen_overlaps_xwin(scr, xwin) >= 0.05) {
				is_empty = 0;
				break;
			}
		}
	}

	return is_empty;
}


	/* startup logo */

/* animated \/\/\ logo done with simple XOR'd lines, a display of the WM being started and ready */
#define VWM_LOGO_POINTS 6
static void vwm_draw_logo(void)
{
	int			i;
	unsigned int		width, height, yoff, xoff;
	XPoint			points[VWM_LOGO_POINTS];
	const vwm_screen_t	*scr = vwm_screen_find(VWM_SCREEN_REL_POINTER);

	XGrabServer(display);

	/* use the dimensions of the pointer-containing screen */
	width = scr->width;
	height = scr->height;
	xoff = scr->x_org;
	yoff = scr->y_org + ((float)height * .333);
	height /= 3;

	/* the logo gets shrunken vertically until it's essentially a flat line */
	while(height -= 2) {
		/* scale and center the points to the screen size */
		for(i = 0; i < VWM_LOGO_POINTS; i++) {
			points[i].x = xoff + (i * .2 * (float)width);
			points[i].y = (i % 2 * (float)height) + yoff;
		}

		XDrawLines(display, RootWindow(display, screen_num), gc, points, sizeof(points) / sizeof(XPoint), CoordModeOrigin);
		XFlush(display);
		usleep(3333);
		XDrawLines(display, RootWindow(display, screen_num), gc, points, sizeof(points) / sizeof(XPoint), CoordModeOrigin);
		XFlush(display);

		/* the width is shrunken as well, but only by as much as it is tall */
		yoff++;
		width -= 4;
		xoff += 2;
	}

	XUngrabServer(display);
}


	/* launching of external processes / X clients */ 

/* launch a child command specified in argv, mode decides if we wait for the child to exit before returning. */
typedef enum _vwm_launch_mode_t {
	VWM_LAUNCH_MODE_FG,
	VWM_LAUNCH_MODE_BG,
} vwm_launch_mode_t;

static void vwm_launch(char **argv, vwm_launch_mode_t mode)
{
	/* XXX: in BG mode I double fork and let init inherit the orphan so I don't have to collect the return status */
	if(mode == VWM_LAUNCH_MODE_FG || !fork()) {
		if(!fork()) {
			/* child */
			setpriority(PRIO_PROCESS, getpid(), priority + LAUNCHED_RELATIVE_PRIORITY);
			execvp(argv[0], argv);
		}
		if(mode == VWM_LAUNCH_MODE_BG) exit(0);
	}
	wait(NULL); /* TODO: could wait for the specific pid, particularly in FG mode ... */
}


	/* desktop/shelf context handling */

/* switch to the desired context if it isn't already the focused one, inform caller if anything happened */
static int vwm_context_focus(vwm_context_focus_t desired_context)
{
	vwm_context_focus_t	entry_context = focused_context;

	switch(focused_context) {
		vwm_xwindow_t	*xwin;
		vwm_window_t	*vwin;

		case VWM_CONTEXT_FOCUS_SHELF:
			if(desired_context == VWM_CONTEXT_FOCUS_SHELF) break;

			/* desired == DESKTOP && focused == SHELF */

			VWM_TRACE("unmapping shelf window \"%s\"", focused_shelf->xwindow->name);
			vwm_win_unmap(focused_shelf);
			XFlush(display); /* for a more responsive feel */

			/* map the focused desktop, from the top of the stack down */
			list_for_each_entry_prev(xwin, &xwindows, xwindows) {
				if(!(vwin = xwin->managed)) continue;
				if(vwin->desktop == focused_desktop && !vwin->shelved) {
					VWM_TRACE("Mapping desktop window \"%s\"", xwin->name);
					vwm_win_map(vwin);
				}
			}

			if(focused_desktop->focused_window) {
				VWM_TRACE("Focusing \"%s\"", focused_desktop->focused_window->xwindow->name);
				XSetInputFocus(display, focused_desktop->focused_window->xwindow->id, RevertToPointerRoot, CurrentTime);
			}

			focused_context = VWM_CONTEXT_FOCUS_DESKTOP;
			break;

		case VWM_CONTEXT_FOCUS_DESKTOP:
			/* unmap everything, map the shelf */
			if(desired_context == VWM_CONTEXT_FOCUS_DESKTOP) break;

			/* desired == SHELF && focused == DESKTOP */

			/* there should be a focused shelf if the shelf contains any windows, we NOOP the switch if the shelf is empty. */
			if(focused_shelf) {
				/* unmap everything on the current desktop */
				list_for_each_entry(xwin, &xwindows, xwindows) {
					if(!(vwin = xwin->managed)) continue;
					if(vwin->desktop == focused_desktop) {
						VWM_TRACE("Unmapping desktop window \"%s\"", xwin->name);
						vwm_win_unmap(vwin);
					}
				}

				XFlush(display); /* for a more responsive feel */

				VWM_TRACE("Mapping shelf window \"%s\"", focused_shelf->xwindow->name);
				vwm_win_map(focused_shelf);
				vwm_win_focus(focused_shelf);

				focused_context = VWM_CONTEXT_FOCUS_SHELF;
			}
			break;

		default:
			VWM_BUG("unexpected focused context %x", focused_context);
			break;
	}

	/* return if the context has been changed, the caller may need to branch differently if nothing happened */
	return (focused_context != entry_context);
}


	/* virtual desktops */

/* make the specified desktop the most recently used one */
static void vwm_desktop_mru(vwm_desktop_t *desktop)
{
	VWM_TRACE("MRU desktop: %p", desktop);
	list_move(&desktop->desktops_mru, &desktops_mru);
}


/* focus a virtual desktop */
/* this switches to the desktop context if necessary, maps and unmaps windows accordingly if necessary */
static int vwm_desktop_focus(vwm_desktop_t *desktop)
{
	XGrabServer(display);
	XSync(display, False);

	/* if the context switched and the focused desktop is the desired desktop there's nothing else to do */
	if((vwm_context_focus(VWM_CONTEXT_FOCUS_DESKTOP) && focused_desktop != desktop) || focused_desktop != desktop) {
		vwm_xwindow_t	*xwin;
		vwm_window_t	*vwin;

		/* unmap the windows on the currently focused desktop, map those on the newly focused one */
		list_for_each_entry(xwin, &xwindows, xwindows) {
			if(!(vwin = xwin->managed) || vwin->shelved) continue;
			if(vwin->desktop == focused_desktop) vwm_win_unmap(vwin);
		}

		XFlush(display);

		list_for_each_entry_prev(xwin, &xwindows, xwindows) {
			if(!(vwin = xwin->managed) || vwin->shelved) continue;
			if(vwin->desktop == desktop) vwm_win_map(vwin);
		}

		focused_desktop = desktop;
	}

	/* directly focus the desktop's focused window if there is one, we don't use vwm_win_focus() intentionally XXX */
	if(focused_desktop->focused_window) {
		VWM_TRACE("Focusing \"%s\"", focused_desktop->focused_window->xwindow->name);
		XSetInputFocus(display, focused_desktop->focused_window->xwindow->id, RevertToPointerRoot, CurrentTime);
	}

	XUngrabServer(display);

	return 1;
}


/* create a virtual desktop */
static vwm_desktop_t * vwm_desktop_create(char *name)
{
	vwm_desktop_t	*desktop;

	desktop = malloc(sizeof(vwm_desktop_t));
	if(desktop == NULL) {
		VWM_PERROR("Failed to allocate desktop");
		goto _fail;
	}

	desktop->name = name == NULL ? name : strdup(name);
	desktop->focused_window = NULL;

	list_add_tail(&desktop->desktops, &desktops);
	list_add_tail(&desktop->desktops_mru, &desktops_mru);

	return desktop;

_fail:
	return NULL;
}


/* destroy a virtual desktop */
static void vwm_desktop_destroy(vwm_desktop_t *desktop)
{
	/* silently refuse to destroy a desktop having windows (for now) */
	/* there's _always_ a focused window on a desktop having mapped windows */
	/* also silently refuse to destroy the last desktop (for now) */
	if(desktop->focused_window || (desktop->desktops.next == desktop->desktops.prev)) return;

	/* focus the MRU desktop that isn't this one if we're the focused desktop */
	if(desktop == focused_desktop) {
		vwm_desktop_t	*next_desktop;

		list_for_each_entry(next_desktop, &desktops_mru, desktops_mru) {
			if(next_desktop != desktop) {
				vwm_desktop_focus(next_desktop);
				break;
			}
		}
	}

	list_del(&desktop->desktops);
	list_del(&desktop->desktops_mru);
}


	/* bare X windows stuff, there's a distinction between bare xwindows and the vwm managed windows */

/* send a client message to a window (currently used for WM_DELETE) */
static void vwm_xwin_message(vwm_xwindow_t *xwin, Atom type, long foo)
{
	XEvent	event;

	memset(&event, 0, sizeof(event));
	event.xclient.type = ClientMessage;
	event.xclient.window = xwin->id;
	event.xclient.message_type = type;
	event.xclient.format = 32;
	event.xclient.data.l[0] = foo;
	event.xclient.data.l[1] = CurrentTime;	/* XXX TODO: is CurrentTime actually correct to use for this purpose? */

	XSendEvent(display, xwin->id, False, 0, &event);
}


/* look up the X window in the global xwindows list (includes unmanaged windows like override_redirect/popup menus) */
static vwm_xwindow_t * vwm_xwin_lookup(Window win)
{
	vwm_xwindow_t	*tmp, *xwin = NULL;

	list_for_each_entry(tmp, &xwindows, xwindows) {
		if(tmp->id == win) {
			xwin = tmp;
			break;
		}
	}

	return xwin;
}


/* determine if a window is visible (vwm-mapped) according to the current context */
static inline int vwm_xwin_is_visible(vwm_xwindow_t *xwin)
{
	int ret = 0;

	if(!xwin->mapped) return 0;
		
	if(xwin->managed) {
		switch(focused_context) {
			case VWM_CONTEXT_FOCUS_SHELF:
				if(focused_shelf == xwin->managed) ret = xwin->mapped;
				break;

			case VWM_CONTEXT_FOCUS_DESKTOP:
				if(focused_desktop == xwin->managed->desktop && !xwin->managed->shelved) ret = xwin->mapped;
				break;

			default:
				VWM_BUG("Unsupported context");
				break;
		}
	} else { /* unmanaged xwins like popup dialogs when mapped are always visible */
		ret = 1;
	}

	/* annoyingly, Xorg stops delivering VisibilityNotify events for redirected windows, so we don't conveniently know if a window is obscured or not :( */
	/* I could maintain my own data structure for answering this question, but that's pretty annoying when Xorg already has that knowledge. */

	return ret;
}


/* bind the window to a "namewindowpixmap" and create a picture from it (compositing) */
void vwm_xwin_bind_namewindow(vwm_xwindow_t *xwin)
{
	xwin->pixmap = XCompositeNameWindowPixmap(display, xwin->id);
	xwin->picture = XRenderCreatePicture(display, xwin->pixmap,
					     XRenderFindVisualFormat(display, xwin->attrs.visual),
					     CPSubwindowMode, &pa_inferiors);
	XFreePixmap(display, xwin->pixmap);
}


/* free the window's picture for accessing its redirected contents (compositing) */
void vwm_xwin_unbind_namewindow(vwm_xwindow_t *xwin)
{
	XRenderFreePicture(display, xwin->picture);
}


/* install a monitor on the window if it doesn't already have one and has _NET_WM_PID set */
static void vwm_xwin_monitor(vwm_xwindow_t *xwin)
{
	Atom		type;
	int		fmt;
	unsigned long	nitems;
	unsigned long	nbytes;
	long		*foo = NULL;
	int		pid = -1;

	if(xwin->monitor) return;

	if(XGetWindowProperty(display, xwin->id, wm_pid_atom, 0, 1, False, XA_CARDINAL,
			      &type, &fmt, &nitems, &nbytes, (unsigned char **)&foo) != Success || !foo) return;

	pid = *foo;
	XFree(foo);

	/* add the client process to the monitoring heirarchy */
	/* XXX note libvmon here maintains a unique callback for each unique callback+xwin pair, so multi-window processes work */
	xwin->monitor = vmon_proc_monitor(&vmon, NULL, pid, VMON_WANT_PROC_INHERIT, (void (*)(vmon_t *, vmon_proc_t *, void *))proc_sample_callback, xwin);
	 /* FIXME: count_rows() isn't returning the right count sometimes (off by ~1), it seems to be related to racing with the automatic child monitoring */
	 /* the result is an extra row sometimes appearing below the process heirarchy */
	xwin->overlay.heirarchy_end = 1 + count_rows(xwin->monitor);
	xwin->overlay.snowflakes_cnt = 0;
}


/* creates and potentially manages a new window (called in response to CreateNotify events, and during startup for all existing windows) */
/* if the window is already mapped and not an override_redirect window, it becomes managed here. */
typedef enum _vwm_grab_mode_t {
	VWM_NOT_GRABBED = 0,
	VWM_GRABBED
} vwm_grab_mode_t;

static vwm_xwindow_t * vwm_xwin_create(Window win, vwm_grab_mode_t grabbed)
{
	XWindowAttributes	attrs;
	vwm_xwindow_t		*xwin = NULL;

	VWM_TRACE("creating %#x", (unsigned int)win);

	/* prevent races */
	if(!grabbed) {
		XGrabServer(display);
		XSync(display, False);
	}

	/* verify the window still exists */
	if(!XGetWindowAttributes(display, win, &attrs)) goto _out_grabbed;

	/* don't create InputOnly windows */
	if(attrs.class == InputOnly) goto _out_grabbed;

	if(!(xwin = (vwm_xwindow_t *)malloc(sizeof(vwm_xwindow_t)))) {
		VWM_PERROR("Failed to allocate xwin");
		goto _out_grabbed;
	}

	xwin->id = win;
	xwin->attrs = attrs;
	xwin->managed = NULL;
	xwin->name = NULL;
	XFetchName(display, win, &xwin->name);

	xwin->monitor = NULL;
	xwin->overlay.width = xwin->overlay.height = xwin->overlay.phase = 0;
	xwin->overlay.gen_last_composed = -1;

	/* This is so we get the PropertyNotify event and can get the pid when it's set post-create,
	 * with my _NET_WM_PID patch the property is immediately available */
	XSelectInput(display, win, PropertyChangeMask);

	vwm_xwin_monitor(xwin);

	/* we must track the mapped-by-client state of the window independent of managed vs. unmanaged because
	 * in the case of override_redirect windows they may be unmapped (invisible) or mapped (visible) like menus without being managed.
	 * otherwise we could just use !xwin.managed to indicate unmapped, which is more vwm2-like, but insufficient when compositing.  */
	xwin->mapped = (attrs.map_state != IsUnmapped);

	if(compositing_mode) {
		vwm_xwin_bind_namewindow(xwin);
		xwin->damage = XDamageCreate(display, xwin->id, XDamageReportNonEmpty);
	}

	list_add_tail(&xwin->xwindows, &xwindows);	/* created windows are always placed on the top of the stacking order */

#ifdef HONOR_OVERRIDE_REDIRECT
	if(!attrs.override_redirect && xwin->mapped) vwm_win_manage_xwin(xwin);
#else
	if(xwin->mapped) vwm_win_manage_xwin(xwin);
#endif
_out_grabbed:
	if(!grabbed) XUngrabServer(display);

	return xwin;
}


/* destroy a window, called in response to DestroyNotify events */
/* if the window is also managed it will be unmanaged first */
static void vwm_xwin_destroy(vwm_xwindow_t *xwin)
{
	XGrabServer(display);
	XSync(display, False);

	if(xwin->managed) vwm_win_unmanage(xwin->managed);

	list_del(&xwin->xwindows);

	if(xwin->monitor) vmon_proc_unmonitor(&vmon, xwin->monitor, (void (*)(vmon_t *, vmon_proc_t *, void *))proc_sample_callback, xwin);
	if(xwin->name) XFree(xwin->name);

	if(compositing_mode) {
		vwm_xwin_unbind_namewindow(xwin);
		XDamageDestroy(display, xwin->damage);
	}
	free(xwin);

	XUngrabServer(display);
}


/* maintain the stack-ordered xwindows list, when new_above is != None xwin is to be placed above new_above, when == None xwin goes to the bottom. */
void vwm_xwin_restack(vwm_xwindow_t *xwin, Window new_above)
{
	Window		old_above;
#ifdef TRACE
	vwm_xwindow_t	*tmp;
	fprintf(stderr, "restack of %#x new_above=%#x\n", (unsigned int)xwin->id, (unsigned int)new_above);
	fprintf(stderr, "restack pre:");
	list_for_each_entry(tmp, &xwindows, xwindows) {
		fprintf(stderr, " %#x", (unsigned int)tmp->id);
	}
	fprintf(stderr, "\n");
#endif
	if(xwin->xwindows.prev != &xwindows) {
		old_above = list_entry(xwin->xwindows.prev, vwm_xwindow_t, xwindows)->id;
	} else {
		old_above = None;
	}

	if(old_above != new_above) {
		vwm_xwindow_t	*new;

		if(new_above == None) {				/* to the bottom of the stack, so just above the &xwindows head */
			list_move(&xwin->xwindows, &xwindows);
		} else if((new = vwm_xwin_lookup(new_above))) {	/* to just above new_above */
			list_move(&xwin->xwindows, &new->xwindows);
		}
	}
#ifdef TRACE
	fprintf(stderr, "restack post:");
	list_for_each_entry(tmp, &xwindows, xwindows) {
		fprintf(stderr, " %#x", (unsigned int)tmp->id);
	}
	fprintf(stderr, "\n\n");
#endif
}


	/* vwm "managed" windows (vwm_window_t) (which are built upon the "core" X windows (vwm_xwindow_t)) */

/* unmap the specified window and set the unmapping-in-progress flag so we can discard vwm-generated UnmapNotify events */
static void vwm_win_unmap(vwm_window_t *vwin)
{
	if(!vwin->xwindow->mapped) {
		VWM_TRACE("inhibited unmap of \"%s\", not mapped by client", vwin->xwindow->name);
		return;
	}
	VWM_TRACE("Unmapping \"%s\"", vwin->xwindow->name);
	vwin->unmapping = 1;
	XUnmapWindow(display, vwin->xwindow->id);
}


/* map the specified window and set the mapping-in-progress flag so we can discard vwm-generated MapNotify events */
static void vwm_win_map(vwm_window_t *vwin)
{
	if(!vwin->xwindow->mapped) {
		VWM_TRACE("inhibited map of \"%s\", not mapped by client", vwin->xwindow->name);
		return;
	}
	VWM_TRACE("Mapping \"%s\"", vwin->xwindow->name);
	vwin->mapping = 1;
	XMapWindow(display, vwin->xwindow->id);
}


/* make the specified window the most recently used one */
static void vwm_win_mru(vwm_window_t *vwin)
{
	list_move(&vwin->windows_mru, &windows_mru);
}


/* look up the X window in the global managed windows list */
static vwm_window_t * vwm_win_lookup(Window win)
{
	vwm_window_t	*tmp, *vwin = NULL;

	list_for_each_entry(tmp, &windows_mru, windows_mru) {
		if(tmp->xwindow->id == win) {
			vwin = tmp;
			break;
		}
	}

	return vwin;
}


/* return the currently focused window (considers current context...), may return NULL */
static vwm_window_t * vwm_win_focused(void)
{
	vwm_window_t	*vwin = NULL;

	switch(focused_context) {
		case VWM_CONTEXT_FOCUS_SHELF:
			vwin = focused_shelf;
			break;

		case VWM_CONTEXT_FOCUS_DESKTOP:
			if(focused_desktop) vwin = focused_desktop->focused_window;
			break;

		default:
			VWM_BUG("Unsupported context");
			break;
	}

	return vwin;
}


/* "autoconfigure" windows (configuration shortcuts like fullscreen/halfscreen/quarterscreen) and restoring the window */
typedef enum _vwm_win_autoconf_t {
	VWM_WIN_AUTOCONF_NONE,		/* un-autoconfigured window (used to restore the configuration) */
	VWM_WIN_AUTOCONF_QUARTER,	/* quarter-screened */
	VWM_WIN_AUTOCONF_HALF,		/* half-screened */
	VWM_WIN_AUTOCONF_FULL,		/* full-screened */
	VWM_WIN_AUTOCONF_ALL		/* all-screened (borderless) */
} vwm_win_autoconf_t;

typedef enum _vwm_side_t {
	VWM_SIDE_TOP,
	VWM_SIDE_BOTTOM,
	VWM_SIDE_LEFT,
	VWM_SIDE_RIGHT
} vwm_side_t;

typedef enum _vwm_corner_t {
	VWM_CORNER_TOP_LEFT,
	VWM_CORNER_TOP_RIGHT,
	VWM_CORNER_BOTTOM_RIGHT,
	VWM_CORNER_BOTTOM_LEFT
} vwm_corner_t;

static void vwm_win_autoconf(vwm_window_t *vwin, vwm_screen_rel_t rel, vwm_win_autoconf_t conf, ...)
{
	const vwm_screen_t	*scr;
	va_list			ap;
	XWindowChanges		changes = { .border_width = WINDOW_BORDER_WIDTH };

	/* remember the current configuration as the "client" configuration if it's not an autoconfigured one. */
	if(vwin->autoconfigured == VWM_WIN_AUTOCONF_NONE) vwin->client = vwin->xwindow->attrs;

	scr = vwm_screen_find(rel, vwin->xwindow);
	va_start(ap, conf);
	switch(conf) {
		case VWM_WIN_AUTOCONF_QUARTER: {
			vwm_corner_t corner = va_arg(ap, vwm_corner_t);
			changes.width = scr->width / 2 - (WINDOW_BORDER_WIDTH * 2);
			changes.height = scr->height / 2 - (WINDOW_BORDER_WIDTH * 2);
			switch(corner) {
				case VWM_CORNER_TOP_LEFT:
					changes.x = scr->x_org;
					changes.y = scr->y_org;
					break;

				case VWM_CORNER_TOP_RIGHT:
					changes.x = scr->x_org + scr->width / 2;
					changes.y = scr->y_org;
					break;

				case VWM_CORNER_BOTTOM_RIGHT:
					changes.x = scr->x_org + scr->width / 2;
					changes.y = scr->y_org + scr->height / 2;
					break;

				case VWM_CORNER_BOTTOM_LEFT:
					changes.x = scr->x_org;
					changes.y = scr->y_org + scr->height / 2;
					break;
			}
			break;
		}

		case VWM_WIN_AUTOCONF_HALF: {
			vwm_side_t side = va_arg(ap, vwm_side_t);
			switch(side) {
				case VWM_SIDE_TOP:
					changes.width = scr->width - (WINDOW_BORDER_WIDTH * 2);
					changes.height = scr->height / 2 - (WINDOW_BORDER_WIDTH * 2);
					changes.x = scr->x_org;
					changes.y = scr->y_org;
					break;

				case VWM_SIDE_BOTTOM:
					changes.width = scr->width - (WINDOW_BORDER_WIDTH * 2);
					changes.height = scr->height / 2 - (WINDOW_BORDER_WIDTH * 2);
					changes.x = scr->x_org;
					changes.y = scr->y_org + scr->height / 2;
					break;

				case VWM_SIDE_LEFT:
					changes.width = scr->width / 2 - (WINDOW_BORDER_WIDTH * 2);
					changes.height = scr->height - (WINDOW_BORDER_WIDTH * 2);
					changes.x = scr->x_org;
					changes.y = scr->y_org;
					break;

				case VWM_SIDE_RIGHT:
					changes.width = scr->width / 2 - (WINDOW_BORDER_WIDTH * 2);
					changes.height = scr->height - (WINDOW_BORDER_WIDTH * 2);
					changes.x = scr->x_org + scr->width / 2;
					changes.y = scr->y_org;
					break;
			}
			break;
		}

		case VWM_WIN_AUTOCONF_FULL:
			changes.width = scr->width - WINDOW_BORDER_WIDTH * 2;
			changes.height = scr->height - WINDOW_BORDER_WIDTH * 2;
			changes.x = scr->x_org;
			changes.y = scr->y_org;
			break;

		case VWM_WIN_AUTOCONF_ALL:
			changes.width = scr->width;
			changes.height = scr->height;
			changes.x = scr->x_org;
			changes.y = scr->y_org;
			changes.border_width = 0;
			break;

		case VWM_WIN_AUTOCONF_NONE: /* restore window if autoconfigured */
			changes.width = vwin->client.width;
			changes.height = vwin->client.height;
			changes.x = vwin->client.x;
			changes.y = vwin->client.y;
			break;
	}
	va_end(ap);

	XConfigureWindow(display, vwin->xwindow->id, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &changes);
	vwin->autoconfigured = conf;
}


/* focus a window */
/* this updates window border color as needed and the X input focus */
static void vwm_win_focus(vwm_window_t *vwin)
{
	VWM_TRACE("focusing: %#x", (unsigned int)vwin->xwindow->id);

	/* change the focus to the new window */
	XSetInputFocus(display, vwin->xwindow->id, RevertToPointerRoot, CurrentTime);

	/* update the border color accordingly */
	if(vwin->shelved) {
		/* set the border of the newly focused window to the shelved color */
		XSetWindowBorder(display, vwin->xwindow->id, vwin == console ? shelved_console_border_color.pixel : shelved_window_border_color.pixel);
		/* fullscreen windows in the shelf when focused, since we don't intend to overlap there */
		vwm_win_autoconf(vwin, VWM_SCREEN_REL_POINTER, VWM_WIN_AUTOCONF_FULL);	/* XXX TODO: for now the shelf follows the pointer, it's simple. */
	} else {
		if(vwin->desktop == focused_desktop && focused_desktop->focused_window) {
			/* if we've changed focus within the same desktop, set the currently focused window border to the
			 * unfocused color.  Otherwise, we want to leave the focused color on the old window on the old desktop */
			XSetWindowBorder(display, focused_desktop->focused_window->xwindow->id, unfocused_window_border_color.pixel);
		}

		/* set the border of the newly focused window to the focused color */
		XSetWindowBorder(display, vwin->xwindow->id, focused_window_border_color.pixel);

		/* persist this on a per-desktop basis so it can be restored on desktop switches */
		focused_desktop->focused_window = vwin;
	}
}


/* focus the next window on a virtual desktop relative to the supplied window, in the specified context, respecting screen boundaries according to fence. */
typedef enum _vwm_fence_t {
	VWM_FENCE_IGNORE = 0,		/* behave as if screen boundaries don't exist (like the pre-Xinerama code) */
	VWM_FENCE_RESPECT,		/* confine operation to within the screen */
	VWM_FENCE_TRY_RESPECT,		/* confine operation to within the screen, unless no options exist. */
	VWM_FENCE_VIOLATE,		/* leave the screen for any other */
	VWM_FENCE_MASKED_VIOLATE	/* leave the screen for any other not masked */
} vwm_fence_t;

static vwm_window_t * vwm_win_focus_next(vwm_window_t *vwin, vwm_context_focus_t context, vwm_fence_t fence)
{
	const vwm_screen_t	*scr = vwm_screen_find(VWM_SCREEN_REL_XWIN, vwin->xwindow), *next_scr = NULL;
	vwm_window_t		*next;
	unsigned long		visited_mask;

_retry:
	visited_mask = 0;
	list_for_each_entry(next, &vwin->windows_mru, windows_mru) {
		/* searching for the next mapped window in this context, using vwin->windows as the head */
		if(&next->windows_mru == &windows_mru) continue;	/* XXX: skip the containerless head, we're leveraging the circular list implementation */

		if((context == VWM_CONTEXT_FOCUS_SHELF && next->shelved) ||
		   ((context == VWM_CONTEXT_FOCUS_DESKTOP && !next->shelved && next->desktop == focused_desktop) &&
		    (fence == VWM_FENCE_IGNORE ||
		     ((fence == VWM_FENCE_RESPECT || fence == VWM_FENCE_TRY_RESPECT) && vwm_screen_find(VWM_SCREEN_REL_XWIN, next->xwindow) == scr) ||
		     (fence == VWM_FENCE_VIOLATE && vwm_screen_find(VWM_SCREEN_REL_XWIN, next->xwindow) != scr) ||
		     (fence == VWM_FENCE_MASKED_VIOLATE && (next_scr = vwm_screen_find(VWM_SCREEN_REL_XWIN, next->xwindow)) != scr &&
		      !((1UL << next_scr->screen_number) & fence_mask))
		    ))) break;

		  if(fence == VWM_FENCE_MASKED_VIOLATE && next_scr && next_scr != scr) visited_mask |= (1UL << next_scr->screen_number);
	}

	if(fence == VWM_FENCE_TRY_RESPECT && next == vwin) {
		/* if we tried to respect the fence and failed to find a next, fallback to ignoring the fence and try again */
		fence = VWM_FENCE_IGNORE;
		goto _retry;
	} else if(fence == VWM_FENCE_MASKED_VIOLATE && next_scr) {
		/* if we did a masked violate update the mask with the potentially new screen number */
		if(next == vwin && visited_mask) {
			/* if we failed to find a next window on a different screen but had candidates we've exhausted screens and need to reset the mask then retry */
			VWM_TRACE("all candidate screens masked @ 0x%lx, resetting mask", fence_mask);
			fence_mask = 0;
			goto _retry;
		}
		fence_mask |= (1UL << next_scr->screen_number);
		VWM_TRACE("VWM_FENCE_MASKED_VIOLATE fence_mask now: 0x%lx\n", fence_mask);
	}

	switch(context) {
		case VWM_CONTEXT_FOCUS_DESKTOP:
			if(next != next->desktop->focused_window) {
				/* focus the changed window */
				vwm_win_focus(next);
				XRaiseWindow(display, next->xwindow->id);
			}
			break;

		case VWM_CONTEXT_FOCUS_SHELF:
			if(next != focused_shelf) {
				/* shelf switch, unmap the focused shelf and take it over */
				vwm_win_unmap(focused_shelf);

				XFlush(display);

				vwm_win_map(next);
				focused_shelf = next;
				vwm_win_focus(next);
			}
			break;

		default:
			VWM_ERROR("Unhandled focus context %#x", context);
			break;
	}

	VWM_TRACE("vwin=%p xwin=%p name=\"%s\"", next, next->xwindow, next->xwindow->name);

	return next;
}


/* shelves a window, if the window is focused we focus the next one (if possible) */
static void vwm_win_shelve(vwm_window_t *vwin)
{
	/* already shelved, NOOP */
	if(vwin->shelved) return;

	/* shelving focused window, focus the next window */
	if(vwin == vwin->desktop->focused_window) {
		vwm_win_mru(vwm_win_focus_next(vwin, VWM_CONTEXT_FOCUS_DESKTOP, VWM_FENCE_RESPECT));
	}

	if(vwin == vwin->desktop->focused_window) {
		/* TODO: we can probably put this into vwm_win_focus_next() and have it always handled there... */
		vwin->desktop->focused_window = NULL;
	}

	vwin->shelved = 1;
	vwm_win_mru(vwin);

	/* newly shelved windows always become the focused shelf */
	focused_shelf = vwin;

	vwm_win_unmap(vwin);
}


/* helper for (idempotently) unfocusing a window, deals with context switching etc... */
static void vwm_win_unfocus(vwm_window_t *vwin)
{
	/* if we're the shelved window, cycle the focus to the next shelved window if possible, if there's no more shelf, switch to the desktop */
	/* TODO: there's probably some icky behaviors for focused windows unmapping/destroying in unfocused contexts, we probably jump contexts suddenly. */
	if(vwin == focused_shelf) {
		VWM_TRACE("unfocusing focused shelf");
		vwm_win_focus_next(vwin, VWM_CONTEXT_FOCUS_SHELF, VWM_FENCE_IGNORE);

		if(vwin == focused_shelf) {
			VWM_TRACE("shelf empty, leaving");
			/* no other shelved windows, exit the shelf context */
			vwm_context_focus(VWM_CONTEXT_FOCUS_DESKTOP);
			focused_shelf = NULL;
		}
	}

	/* if we're the focused window cycle the focus to the next window on the desktop if possible */
	if(vwin->desktop->focused_window == vwin) {
		VWM_TRACE("unfocusing focused window");
		vwm_win_focus_next(vwin, VWM_CONTEXT_FOCUS_DESKTOP, VWM_FENCE_TRY_RESPECT);
	}

	if(vwin->desktop->focused_window == vwin) {
		VWM_TRACE("desktop empty");
		vwin->desktop->focused_window = NULL;
	}
}


/* demote an managed window to an unmanaged one */
static vwm_xwindow_t * vwm_win_unmanage(vwm_window_t *vwin)
{
	vwm_win_mru(vwin); /* bump vwin to the mru head before unfocusing so we always move focus to the current head on unmanage of the focused window */
	vwm_win_unfocus(vwin);
	list_del(&vwin->windows_mru);

	if(vwin == console) console = NULL;

	vwin->xwindow->managed = NULL;

	return vwin->xwindow;
}


/* promote an unmanaged window to a managed one */
static vwm_window_t * vwm_win_manage_xwin(vwm_xwindow_t *xwin)
{
	vwm_window_t	*focused, *vwin = NULL;

	if(xwin->managed) {
		VWM_TRACE("suppressed re-management of xwin=%p", xwin);
		goto _fail;
	}

	if(!(vwin = (vwm_window_t *)malloc(sizeof(vwm_window_t)))) {
		VWM_PERROR("Failed to allocate vwin");
		goto _fail;
	}

	XUngrabButton(display, AnyButton, AnyModifier, xwin->id);
	XGrabButton(display, AnyButton, Mod1Mask, xwin->id, False, (PointerMotionMask | ButtonPressMask | ButtonReleaseMask), GrabModeAsync, GrabModeAsync, None, None);
	XGrabKey(display, AnyKey, Mod1Mask, xwin->id, False, GrabModeAsync, GrabModeAsync);
	XSetWindowBorder(display, xwin->id, unfocused_window_border_color.pixel);

	vwin->hints = XAllocSizeHints();
	if(!vwin->hints) {
		VWM_PERROR("Failed to allocate WM hints");
		goto _fail;
	}
	XGetWMNormalHints(display, xwin->id, vwin->hints, &vwin->hints_supplied);

	xwin->managed = vwin;
	vwin->xwindow = xwin;

	vwin->desktop = focused_desktop;
	vwin->autoconfigured = VWM_WIN_AUTOCONF_NONE;
	vwin->mapping = vwin->unmapping = vwin->configuring = 0;
	vwin->shelved = (focused_context == VWM_CONTEXT_FOCUS_SHELF);	/* if we're in the shelf when the window is created, the window is shelved */
	vwin->client = xwin->attrs;					/* remember whatever the current attributes are */

	VWM_TRACE("hints: flags=%lx x=%i y=%i w=%i h=%i minw=%i minh=%i maxw=%i maxh=%i winc=%i hinc=%i basew=%i baseh=%i grav=%x",
			vwin->hints->flags,
			vwin->hints->x, vwin->hints->y,
			vwin->hints->width, vwin->hints->height,
			vwin->hints->min_width, vwin->hints->min_height,
			vwin->hints->max_width, vwin->hints->max_height,
			vwin->hints->width_inc, vwin->hints->height_inc,
			vwin->hints->base_width, vwin->hints->base_height,
			vwin->hints->win_gravity);

	if((vwin->hints_supplied & (USSize | PSize))) {
		vwin->client.width = vwin->hints->base_width;
		vwin->client.height = vwin->hints->base_height;
	}

	/* put it on the global windows_mru list, if there's a focused window insert the new one after it */
	if(!list_empty(&windows_mru) && (focused = vwm_win_focused())) {
		/* insert the vwin immediately after the focused window, so Mod1+Tab goes to the new window */
		list_add(&vwin->windows_mru, &focused->windows_mru);
	} else {
		list_add(&vwin->windows_mru, &windows_mru);
	}

	/* always raise newly managed windows so we know about them. */
	XRaiseWindow(display, xwin->id);

	/* if the desktop has no focused window yet, automatically focus the newly managed one, provided we're on the desktop context */
	if(!focused_desktop->focused_window && focused_context == VWM_CONTEXT_FOCUS_DESKTOP) {
		VWM_TRACE("Mapped new window \"%s\" is alone on desktop \"%s\", focusing", xwin->name, focused_desktop->name);
		vwm_win_focus(vwin);
	}

	return vwin;

_fail:
	if(vwin) {
		if(vwin->hints) XFree(vwin->hints);
		free(vwin);
	}
	return NULL;
}


/* migrate a window to a new desktop, focuses the destination desktop as well */
static void vwm_win_migrate(vwm_window_t *vwin, vwm_desktop_t *desktop)
{
	vwm_win_unfocus(vwin);			/* go through the motions of unfocusing the window if it is focused */
	vwin->shelved = 0;			/* ensure not shelved */
	vwin->desktop = desktop;		/* assign the new desktop */
	vwm_desktop_focus(desktop);		/* currently we always focus the new desktop in a migrate */

	vwm_win_focus(vwin);			/* focus the window so borders get updated */
	vwm_win_mru(vwin); /* TODO: is this right? shouldn't the Mod1 release be what's responsible for this? I migrate so infrequently it probably doesn't matter */

	XRaiseWindow(display, vwin->xwindow->id); /* ensure the window is raised */
}


	/* compositing manager stuff */

/* add damage to the global combined damage queue where we accumulate damage between calls to paint_all() */
static void vwm_comp_damage_add(XserverRegion damage)
{
	if(combined_damage != None) {
		XFixesUnionRegion(display, combined_damage, combined_damage, damage);
		XFixesDestroyRegion(display, damage);
	} else {
		combined_damage = damage;
	}
}


/* take what regions of the damaged window have been damaged, subtract them from the per-window damage object, and add them to the combined damage */
static void vwm_comp_damage_event(XDamageNotifyEvent *ev)
{
	XserverRegion		region;
	vwm_xwindow_t		*xwin;

	xwin = vwm_xwin_lookup(ev->drawable);
	if(!xwin) {
		VWM_ERROR("damaged unknown drawable %x", (unsigned int)ev->drawable);
		return;
	}

 	region = XFixesCreateRegion(display, NULL, 0);
	XDamageSubtract(display, xwin->damage, None, region);
	XFixesTranslateRegion(display, region, xwin->attrs.x + xwin->attrs.border_width, xwin->attrs.y + xwin->attrs.border_width);
	vwm_comp_damage_add(region);
}


/* helper to damage an entire window including the border */
static void vwm_comp_damage_win(vwm_xwindow_t *xwin)
{
	XserverRegion	region;
	XRectangle	rect = { xwin->attrs.x,
				 xwin->attrs.y,
				 xwin->attrs.width + xwin->attrs.border_width * 2,
				 xwin->attrs.height + xwin->attrs.border_width * 2 };
	region = XFixesCreateRegion(display, &rect, 1);
	vwm_comp_damage_add(region);
}


/* throw away our double buffered root pictures so they get recreated on the next paint_all() */
/* used in response to screen configuration changes... */
static void vwm_comp_invalidate_root()
{
	if(root_picture) XRenderFreePicture(display, root_picture);
	root_picture = None;
	if(root_buffer) XRenderFreePicture(display, root_picture);
	root_buffer = None;
}


/* consume combined_damage by iterating the xwindows list from the top down, drawing visible windows as encountered, subtracting their area from combined_damage */
/* when compositing is active this is the sole function responsible for making things show up on the screen */
static void vwm_comp_paint_all()
{
	vwm_xwindow_t		*xwin;
	XRenderColor		bgcolor = {0x0000, 0x00, 0x00, 0xffff};
	Region			occluded = XCreateRegion();
	static XserverRegion	undamage_region = None;

	if(!undamage_region) undamage_region = XFixesCreateRegion(display, NULL, 0);

	/* (re)create the root picture from the root window and allocate a double buffer for the off-screen composition of the damaged contents */
	if(root_picture == None) {
		Pixmap	root_pixmap;

		XGetWindowAttributes(display, RootWindow(display, screen_num), &root_attrs);
		root_picture = XRenderCreatePicture(display, RootWindow(display, screen_num),
						    XRenderFindVisualFormat(display, DefaultVisual(display, screen_num)),
						    CPSubwindowMode, &pa_inferiors);
		root_pixmap = XCreatePixmap (display, RootWindow(display, screen_num), root_attrs.width, root_attrs.height, DefaultDepth (display, screen_num));
		root_buffer = XRenderCreatePicture (display, root_pixmap, XRenderFindVisualFormat (display, DefaultVisual (display, screen_num)), 0, 0);
		XFreePixmap(display, root_pixmap);
	}

	/* compose overlays for all visible windows up front in a separate pass (kind of lame, but it's simpler since compose_overlay() adds to combined_damage) */
	list_for_each_entry_prev(xwin, &xwindows, xwindows) {
		XRectangle	r;

		if(!vwm_xwin_is_visible(xwin)) continue;	/* if invisible skip */

		/* Everything "visible" next goes through an occlusion check.
		 * Since the composite extension stops delivery of VisibilityNotify events for redirected windows,
		 * (it assumes redirected windows should be treated as transparent, and provides no api to alter this assumption)
		 * we can't simply select the VisibilityNotify events on all windows and cache their visibility state in vwm_xwindow_t then skip
		 * xwin->state==VisibilityFullyObscured windows here to avoid the cost of pointlessly composing overlays and rendering fully obscured windows :(.
		 *
		 * Instead we accumulate an occluded region (starting empty) of painted windows from the top-down on every paint_all().
		 * Before we compose_overlay() a window, we check if the window's rectangle fits entirely within the occluded region.
		 * If it does, no compose_overlay() is performed.
		 * If it doesn't, compose_overlay() is called, and the window's rect is added to the occluded region.
		 * The occluded knowledge is also cached for the XRenderComposite() loop immediately following, where we skip the rendering of
		 * occluded windows as well.
		 * This does technically break SHAPE windows (xeyes, xmms), but only when monitoring is enabled which covers the with rectangular overlays anyways.
		 */
		r.x = xwin->attrs.x;
		r.y = xwin->attrs.y;
		r.width = xwin->attrs.width + xwin->attrs.border_width * 2;
		r.height = xwin->attrs.height + xwin->attrs.border_width * 2;
		if(XRectInRegion(occluded, r.x, r.y, r.width, r.height) != RectangleIn) {
			/* the window isn't fully occluded, compose it and add it to occluded */
			if(xwin->monitor && !xwin->attrs.override_redirect) compose_overlay(xwin);
			XUnionRectWithRegion(&r, occluded, occluded);
			xwin->occluded = 0;
		} else {
			xwin->occluded = 1;
			VWM_TRACE("window %#x occluded, skipping compose_overlay()", (int)xwin->id);
		}
	}
	XDestroyRegion(occluded);

	/* start with the clip regions set to the damaged area accumulated since the previous paint_all() */
	XFixesSetPictureClipRegion(display, root_buffer, 0, 0, combined_damage);	/* this is the double buffer where the in-flight screen contents are staged */
	XFixesSetPictureClipRegion(display, root_picture, 0, 0, combined_damage);	/* this is the visible root window */

	/* since translucent windows aren't supported in vwm, I can do this more efficiently */
	list_for_each_entry_prev(xwin, &xwindows, xwindows) {
		XRectangle		r;

		if(!vwm_xwin_is_visible(xwin) || xwin->occluded) continue;	/* if invisible or occluded skip */

		/* these coordinates + dimensions incorporate the border (since XCompositeNameWindowPixmap is being used) */
		r.x = xwin->attrs.x;
		r.y = xwin->attrs.y;
		r.width = xwin->attrs.width + xwin->attrs.border_width * 2;
		r.height = xwin->attrs.height + xwin->attrs.border_width * 2;

		/* render the redirected window contents into root_buffer, note root_buffer has the remaining combined_damage set as the clip region */
		XRenderComposite(display, PictOpSrc, xwin->picture, None, root_buffer,
				 0, 0, 0, 0, /* src x, y, mask x, y */
				 r.x, r.y, /* dst x, y */
				 r.width, r.height);

		if(xwin->monitor && !xwin->attrs.override_redirect && xwin->overlay.width) {
			/* draw the monitoring overlay atop the window, note we stay within the window borders here. */
			XRenderComposite(display, PictOpOver, xwin->overlay.picture, None, root_buffer,
					 0, 0, 0, 0,						/* src x,y, maxk x, y */
					 xwin->attrs.x + xwin->attrs.border_width,		/* dst x */
					 xwin->attrs.y + xwin->attrs.border_width,		/* dst y */ 
					 xwin->attrs.width, overlay_composed_height(xwin));	/* w, h */
		}

		/* subtract the region of the window from the combined damage and update the root_buffer clip region to reflect the remaining damage */
		XFixesSetRegion(display, undamage_region, &r, 1);
		XFixesSubtractRegion(display, combined_damage, combined_damage, undamage_region);
		XFixesSetPictureClipRegion(display, root_buffer, 0, 0, combined_damage);
	}

	/* at this point all of the visible windows have been subtracted from the clip region, so paint any root window damage (draw background) */
	XRenderFillRectangle(display, PictOpSrc, root_buffer, &bgcolor, 0, 0, root_attrs.width, root_attrs.height);

	/* discard the root_buffer clip region and copy root_buffer to root_picture, root_picture still has the combined damage as its clip region */
        XFixesSetPictureClipRegion(display, root_buffer, 0, 0, None);
        XRenderComposite(display, PictOpSrc, root_buffer, None, root_picture, 0, 0, 0, 0, 0, 0, root_attrs.width, root_attrs.height);

	/* fin */
	XFixesDestroyRegion(display, combined_damage);
	combined_damage = None;
}


/* toggle compositing/monitoring overlays on/off */
static void vwm_comp_toggle(void)
{
	vwm_xwindow_t	*xwin;

	XGrabServer(display);
	XSync(display, False);

	switch(compositing_mode) {
		case VWM_COMPOSITING_OFF:
			VWM_TRACE("enabling compositing");
			compositing_mode = VWM_COMPOSITING_MONITORS;
			XCompositeRedirectSubwindows(display, RootWindow(display, screen_num), CompositeRedirectManual);
			list_for_each_entry_prev(xwin, &xwindows, xwindows) {
				vwm_xwin_bind_namewindow(xwin);
				xwin->damage = XDamageCreate(display, xwin->id, XDamageReportNonEmpty);
			}
			/* damage everything */
			/* TODO: keep a list of rects reflecting all the current screens and create a region from that... */
			vwm_comp_damage_add(XFixesCreateRegionFromWindow(display, RootWindow(display, screen_num), WindowRegionBounding));
			break;

		case VWM_COMPOSITING_MONITORS: {
			XEvent	ev;

			VWM_TRACE("disabling compositing");
			compositing_mode = VWM_COMPOSITING_OFF;
			list_for_each_entry_prev(xwin, &xwindows, xwindows) {
				vwm_xwin_unbind_namewindow(xwin);
				XDamageDestroy(display, xwin->damage);
			}
			XCompositeUnredirectSubwindows(display, RootWindow(display, screen_num), CompositeRedirectManual);
			vwm_comp_invalidate_root();

			/* if there's any damage queued up discard it so we don't enter paint_all() until compositing is reenabled again. */
			if(combined_damage) {
				XFixesDestroyRegion(display, combined_damage);
				combined_damage = None;
			}
			while(XCheckTypedEvent(display, damage_event + XDamageNotify, &ev) != False);
			break;
		}
	}

	XUngrabServer(display);
}


	/* input event handling stuff */

/* simple little state machine for managing mouse click/drag/release sequences so we don't need another event loop */
typedef enum _vwm_adjust_mode_t {
	VWM_ADJUST_RESIZE,
	VWM_ADJUST_MOVE
} vwm_adjust_mode_t;

typedef struct _vwm_clickety_t {
	vwm_window_t		*vwin;
	vwm_adjust_mode_t	mode;
	XWindowAttributes	orig, lastrect;
	int			impetus_x, impetus_y, impetus_x_root, impetus_y_root;
} vwm_clickety_t;

/* helper function for resizing the window, how the motion is applied depends on where in the window the impetus event occurred */
static void compute_resize(vwm_clickety_t *clickety, XEvent *terminus, XWindowAttributes *new)
{
	vwm_window_t	*vwin;
	int		dw = (clickety->orig.width / 2);
	int		dh = (clickety->orig.height / 2);
	int		xdelta = (terminus->xbutton.x_root - clickety->impetus_x_root);
	int		ydelta = (terminus->xbutton.y_root - clickety->impetus_y_root);
	int		min_width = 0, min_height = 0;
	int		width_inc = 1, height_inc = 1;

	/* TODO: there's a problem here WRT border width, I should be considering it, I just haven't bothered to fix it since it doesn't seem to matter */
	if((vwin = clickety->vwin) && vwin->hints) {
		if((vwin->hints_supplied & PMinSize)) {
			min_width = vwin->hints->min_width;
			min_height = vwin->hints->min_height;
			VWM_TRACE("window size hints exist and minimum sizes are w=%i h=%i", min_width, min_height);
		}

		if((vwin->hints_supplied & PResizeInc)) {
			width_inc = vwin->hints->width_inc;
			height_inc = vwin->hints->height_inc;
			VWM_TRACE("window size hints exist and resize increments are w=%i h=%i", width_inc, height_inc);
			if(!width_inc) width_inc = 1;
			if(!height_inc) height_inc = 1;
		}
	}

	xdelta = xdelta / width_inc * width_inc;
	ydelta = ydelta / height_inc * height_inc;

	if(clickety->impetus_x < dw && clickety->impetus_y < dh) {
		/* grabbed top left */
		new->x = clickety->orig.x + xdelta;
		new->y = clickety->orig.y + ydelta;
		new->width = clickety->orig.width - xdelta;
		new->height = clickety->orig.height - ydelta;
	} else if(clickety->impetus_x > dw && clickety->impetus_y < dh) {
		/* grabbed top right */
		new->x = clickety->orig.x;
		new->y = clickety->orig.y + ydelta;
		new->width = clickety->orig.width + xdelta;
		new->height = clickety->orig.height - ydelta;
	} else if(clickety->impetus_x < dw && clickety->impetus_y > dh) {
		/* grabbed bottom left */
		new->x = clickety->orig.x + xdelta;
		new->y = clickety->orig.y;
		new->width = clickety->orig.width - xdelta;
		new->height = clickety->orig.height + ydelta;
	} else if(clickety->impetus_x > dw && clickety->impetus_y > dh) {
		/* grabbed bottom right */
		new->x = clickety->orig.x;
		new->y = clickety->orig.y;
		new->width = clickety->orig.width + xdelta;
		new->height = clickety->orig.height + ydelta;
	}

	/* constrain the width and height of the window according to the minimums */
	if(new->width < min_width) {
		if(clickety->orig.x != new->x) new->x -= (min_width - new->width);
		new->width = min_width;
	}

	if(new->height < min_height) {
		if(clickety->orig.y != new->y) new->y -= (min_height - new->height);
		new->height = min_height;
	}
}


static void vwm_clickety_motion(vwm_clickety_t *clickety, Window win, XMotionEvent *motion)
{
	XWindowChanges	changes = { .border_width = WINDOW_BORDER_WIDTH };

	if(!clickety->vwin) return;

	/* TODO: verify win matches clickety->vwin? */
	switch(clickety->mode) {
		case VWM_ADJUST_MOVE:
			changes.x = clickety->orig.x + (motion->x_root - clickety->impetus_x_root);
			changes.y = clickety->orig.y + (motion->y_root - clickety->impetus_y_root);
			XConfigureWindow(display, win, CWX | CWY | CWBorderWidth, &changes);
			break;

		case VWM_ADJUST_RESIZE: {
			XWindowAttributes	resized;

			/* XXX: it just so happens the XMotionEvent structure is identical to XButtonEvent in the fields
			 * needed by compute_resize... */
			compute_resize(clickety, (XEvent *)motion, &resized);
			/* TODO: this is probably broken with compositing active, but it doesn't seem to be too messed up in practice */
			/* erase the last rectangle */
			XDrawRectangle(display, RootWindow(display, screen_num), gc, clickety->lastrect.x, clickety->lastrect.y, clickety->lastrect.width, clickety->lastrect.height);
			/* draw a frame @ resized coordinates */
			XDrawRectangle(display, RootWindow(display, screen_num), gc, resized.x, resized.y, resized.width, resized.height);
			/* remember the last rectangle */
			clickety->lastrect = resized;
			break;
		}

		default:
			break;
	}
}


static void vwm_clickety_released(vwm_clickety_t *clickety, Window win, XButtonPressedEvent *terminus)
{
	XWindowChanges	changes = { .border_width = WINDOW_BORDER_WIDTH };

	if(!clickety->vwin) return;

	switch(clickety->mode) {
		case VWM_ADJUST_MOVE:
			changes.x = clickety->orig.x + (terminus->x_root - clickety->impetus_x_root);
			changes.y = clickety->orig.y + (terminus->y_root - clickety->impetus_y_root);
			XConfigureWindow(display, win, CWX | CWY | CWBorderWidth, &changes);
			break;

		case VWM_ADJUST_RESIZE: {
			XWindowAttributes	resized;
			compute_resize(clickety, (XEvent *)terminus, &resized);
			/* move and resize the window @ resized */
			XDrawRectangle(display, RootWindow(display, screen_num), gc, clickety->lastrect.x, clickety->lastrect.y, clickety->lastrect.width, clickety->lastrect.height);
			changes.x = resized.x;
			changes.y = resized.y;
			changes.width = resized.width;
			changes.height = resized.height;
			XConfigureWindow(display, win, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &changes);
			XUngrabServer(display);
			break;
		}

		default:
			break;
	}
	/* once you manipulate the window it's no longer fullscreened, simply hitting Mod1+Return once will restore fullscreened mode */
	clickety->vwin->autoconfigured = VWM_WIN_AUTOCONF_NONE;

	clickety->vwin = NULL; /* reset clickety */

	XFlush(display);
	XUngrabPointer(display, CurrentTime);
}


/* on pointer buttonpress we initiate a clickety sequence; setup clickety with the window and impetus coordinates.. */
static int vwm_clickety_pressed(vwm_clickety_t *clickety, Window win, XButtonPressedEvent *impetus)
{
	vwm_window_t	*vwin;

	/* verify the window still exists */
	if(!XGetWindowAttributes(display, win, &clickety->orig)) goto _fail;

	if(!(vwin = vwm_win_lookup(win))) goto _fail;

	if(impetus->state & WM_GRAB_MODIFIER) {

		/* always set the input focus to the clicked window, note if we allow this to happen on the root window, it enters sloppy focus mode
		 * until a non-root window is clicked, which is an interesting hybrid but not how I prefer it. */
		if(vwin != focused_desktop->focused_window && vwin->xwindow->id != RootWindow(display, screen_num)) {
			vwm_win_focus(vwin);
			vwm_win_mru(vwin);
		}

		switch(impetus->button) {
			case Button1:
				/* immediately raise the window if we're relocating,
				 * resizes are supported without raising (which also enables NULL resizes to focus without raising) */
				clickety->mode = VWM_ADJUST_MOVE;
				XRaiseWindow(display, win);
				break;

			case Button3:
				/* grab the server on resize for the xor rubber-banding's sake */
				XGrabServer(display);
				XSync(display, False);

				/* FIXME: none of the resize DrawRectangle() calls consider the window border. */
				XDrawRectangle(display, RootWindow(display, screen_num), gc, clickety->orig.x, clickety->orig.y, clickety->orig.width, clickety->orig.height);
				clickety->lastrect = clickety->orig;

				clickety->mode = VWM_ADJUST_RESIZE;
				break;

			default:
				goto _fail;
		}
		clickety->vwin = vwin;
		clickety->impetus_x_root = impetus->x_root;
		clickety->impetus_y_root = impetus->y_root;
		clickety->impetus_x = impetus->x;
		clickety->impetus_y = impetus->y;
	}

	return 1;

_fail:
	XUngrabPointer(display, CurrentTime);

	return 0;
}


/* Called in response to KeyRelease events, for now only interesting for detecting when Mod1 is released termintaing
 * window cycling for application of MRU on the focused window */
static void vwm_keyreleased(Window win, XEvent *keyrelease)
{
	vwm_window_t	*vwin;
	KeySym		sym;

	switch((sym = XLookupKeysym(&keyrelease->xkey, 0))) {
		case XK_Alt_R:
		case XK_Alt_L:	/* TODO: actually use the modifier mapping, for me XK_Alt_[LR] is Mod1.  XGetModifierMapping()... */
			VWM_TRACE("XK_Alt_[LR] released");
			/* make the focused window the most recently used */
			if((vwin = vwm_win_focused())) vwm_win_mru(vwin);

			/* make the focused desktop the most recently used */
			if(focused_context == VWM_CONTEXT_FOCUS_DESKTOP && focused_desktop) vwm_desktop_mru(focused_desktop);

			if(key_is_grabbed) {
				XUngrabKeyboard(display, CurrentTime);
				XFlush(display);
				key_is_grabbed = 0;
				fence_mask = 0;	/* reset the fence mask on release for VWM_FENCE_MASKED_VIOLATE */
			}
			break;

		default:
			VWM_TRACE("Unhandled keycode: %x", (unsigned int)sym);
			break;
	}
}


/* Called in response to KeyPress events, I currenly only grab Mod1 keypress events */
static void vwm_keypressed(Window win, XEvent *keypress)
{
	vwm_window_t				*vwin;
	KeySym					sym;
	static KeySym				last_sym;
	static typeof(keypress->xkey.state)	last_state;
	static int				repeat_cnt = 0;
	int					do_grab = 0;
	char					*quit_console_args[] = {"bash", "-c", "screen -dr " CONSOLE_SESSION_STRING " -X quit", NULL};

	sym = XLookupKeysym(&keypress->xkey, 0);

	/* detect repeaters, note repeaters do not span interrupted Mod1 sequences! */
	if(key_is_grabbed && sym == last_sym && keypress->xkey.state == last_state) {
		repeat_cnt++;
	} else {
		repeat_cnt = 0;
	}

	switch(sym) {

#define launcher(_sym, _label, _argv)\
		case _sym:	\
			{	\
			char	*args[] = {"bash", "-c", "screen -dr " CONSOLE_SESSION_STRING " -X screen bash -i -x -c \"" _argv " || sleep 86400\"", NULL};\
			vwm_launch(args, VWM_LAUNCH_MODE_BG);\
			break;	\
		}
#include "launchers.def"
#undef launcher

		case XK_grave: /* toggle shelf visibility */
			vwm_context_focus(VWM_CONTEXT_FOCUS_OTHER);
			break;

		case XK_Tab: /* cycle focused window */
			do_grab = 1; /* update MRU window on commit (Mod1 release) */

			/* focus the next window, note this doesn't affect MRU yet, that happens on Mod1 release */
			if((vwin = vwm_win_focused())) {
				if(keypress->xkey.state & ShiftMask) {
					vwm_win_focus_next(vwin, focused_context, VWM_FENCE_MASKED_VIOLATE);
				} else {
					vwm_win_focus_next(vwin, focused_context, VWM_FENCE_RESPECT);
				}
			}
			break;

		case XK_space: { /* cycle focused desktop utilizing MRU */
			vwm_desktop_t	*next_desktop = list_entry(focused_desktop->desktops_mru.next == &desktops_mru ? focused_desktop->desktops_mru.next->next : focused_desktop->desktops_mru.next, vwm_desktop_t, desktops_mru); /* XXX: note the sensitivity to the desktops_mru head here, we want to look past it. */

			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if(keypress->xkey.state & ShiftMask) {
				/* migrate the focused window with the desktop focus to the most recently used desktop */
				if((vwin = vwm_win_focused())) vwm_win_migrate(vwin, next_desktop);
			} else {
				vwm_desktop_focus(next_desktop);
			}
			break;
		}

		case XK_d: /* destroy focused */
			if((vwin = vwm_win_focused())) {
				if(keypress->xkey.state & ShiftMask) {  /* brutally destroy the focused window */
					XKillClient(display, vwin->xwindow->id);
				} else { /* kindly destroy the focused window */
					vwm_xwin_message(vwin->xwindow, wm_protocols_atom, wm_delete_atom);
				}
			} else if(focused_context == VWM_CONTEXT_FOCUS_DESKTOP) {
				/* destroy the focused desktop when destroy occurs without any windows */
				vwm_desktop_destroy(focused_desktop);
			}
			break;

		case XK_Escape: /* leave VWM rudely, after triple press */
			do_grab = 1;

			if(repeat_cnt == 2) {
				vwm_launch(quit_console_args, VWM_LAUNCH_MODE_FG);
				exit(42);
			}
			break;

		case XK_v: /* instantiate (and focus) a new (potentially empty, unless migrating) virtual desktop */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if(keypress->xkey.state & ShiftMask) {
				if((vwin = vwm_win_focused())) {
					/* migrate the focused window to a newly created virtual desktop, focusing the new desktop simultaneously */
					vwm_win_migrate(vwin, vwm_desktop_create(NULL));
				}
			} else {
				vwm_desktop_focus(vwm_desktop_create(NULL));
				vwm_desktop_mru(focused_desktop);
			}
			break;

		case XK_h: /* previous virtual desktop, if we're in the shelf context this will simply switch to desktop context */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if(keypress->xkey.state & ShiftMask) {
				if((vwin = vwm_win_focused()) && vwin->desktop->desktops.prev != &desktops) {
					/* migrate the focused window with the desktop focus to the previous desktop */
					vwm_win_migrate(vwin, list_entry(vwin->desktop->desktops.prev, vwm_desktop_t, desktops));
				}
			} else {
				if(focused_context == VWM_CONTEXT_FOCUS_SHELF) {
					/* focus the focused desktop instead of the shelf */
					vwm_context_focus(VWM_CONTEXT_FOCUS_DESKTOP);
				} else if(focused_desktop->desktops.prev != &desktops) {
					/* focus the previous desktop */
					vwm_desktop_focus(list_entry(focused_desktop->desktops.prev, vwm_desktop_t, desktops));
				}
			}
			break;

		case XK_l: /* next virtual desktop, if we're in the shelf context this will simply switch to desktop context */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if(keypress->xkey.state & ShiftMask) {
				if((vwin = vwm_win_focused()) && vwin->desktop->desktops.next != &desktops) {
					/* migrate the focused window with the desktop focus to the next desktop */
					vwm_win_migrate(vwin, list_entry(vwin->desktop->desktops.next, vwm_desktop_t, desktops));
				}
			} else {
				if(focused_context == VWM_CONTEXT_FOCUS_SHELF) {
					/* focus the focused desktop instead of the shelf */
					vwm_context_focus(VWM_CONTEXT_FOCUS_DESKTOP);
				} else if(focused_desktop->desktops.next != &desktops) {
					/* focus the next desktop */
					vwm_desktop_focus(list_entry(focused_desktop->desktops.next, vwm_desktop_t, desktops));
				}
			}
			break;

		case XK_k: /* raise or shelve the focused window */
			if((vwin = vwm_win_focused())) {
				if(keypress->xkey.state & ShiftMask) { /* shelf the window and focus the shelf */
					if(focused_context != VWM_CONTEXT_FOCUS_SHELF) {
						/* shelve the focused window while focusing the shelf */
						vwm_win_shelve(vwin);
						vwm_context_focus(VWM_CONTEXT_FOCUS_SHELF);
					}
				} else {
					do_grab = 1;

					XRaiseWindow(display, vwin->xwindow->id);

					if(repeat_cnt == 1) {
						/* double: reraise & fullscreen */
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
					} else if(repeat_cnt == 2) {
						 /* triple: reraise & fullscreen w/borders obscured by screen perimiter */
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_ALL);
					} else if(xinerama_screens_cnt > 1) {
						if(repeat_cnt == 3) {
							 /* triple: reraise & fullscreen across all screens */
							vwm_win_autoconf(vwin, VWM_SCREEN_REL_TOTAL, VWM_WIN_AUTOCONF_FULL);
						} else if(repeat_cnt == 4) {
							 /* quadruple: reraise & fullscreen w/borders obscured by screen perimiter */
							vwm_win_autoconf(vwin, VWM_SCREEN_REL_TOTAL, VWM_WIN_AUTOCONF_ALL);
						}
					}
					XFlush(display);
				}
			}
			break;

		case XK_j: /* lower or unshelve the focused window */
			if((vwin = vwm_win_focused())) {
				if(keypress->xkey.state & ShiftMask) { /* unshelf the window to the focused desktop, and focus the desktop */
					if(focused_context == VWM_CONTEXT_FOCUS_SHELF) {
						/* unshelve the focused window, focus the desktop it went to */
						vwm_win_migrate(vwin, focused_desktop);
					}
				} else {
					if(vwin->autoconfigured == VWM_WIN_AUTOCONF_ALL) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
					} else {
						XLowerWindow(display, vwin->xwindow->id);
					}
					XFlush(display);
				}
			}
			break;

		case XK_Return: /* (full-screen / restore) focused window */
			if((vwin = vwm_win_focused())) {
				if(vwin->autoconfigured) {
					vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_NONE);
				} else {
					vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
				}
			}
			break;

		case XK_s: /* shelve focused window */
			if((vwin = vwm_win_focused()) && !vwin->shelved) vwm_win_shelve(vwin);
			break;

		case XK_bracketleft:	/* reconfigure the focused window to occupy the left or top half of the screen or left quarters on repeat */
			if((vwin = vwm_win_focused())) {
				do_grab = 1;

				if(keypress->xkey.state & ShiftMask) {
					if(!repeat_cnt) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_TOP);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_TOP_LEFT);
					}
				} else {
					if(!repeat_cnt) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_LEFT);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_BOTTOM_LEFT);
					}
				}
			}
			break;

		case XK_bracketright:	/* reconfigure the focused window to occupy the right or bottom half of the screen or right quarters on repeat */
			if((vwin = vwm_win_focused())) {
				do_grab = 1;

				if(keypress->xkey.state & ShiftMask) {
					if(!repeat_cnt) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_BOTTOM);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_BOTTOM_RIGHT);
					}
				} else {
					if(!repeat_cnt) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_RIGHT);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_TOP_RIGHT);
					}
				}
			}
			break;

		case XK_semicolon:	/* toggle composited overlays */
			vwm_comp_toggle();
			break;

		case XK_apostrophe:	/* reset snowflakes of the focused window, suppressed when not compositing */
			if((vwin = vwm_win_focused()) && compositing_mode && vwin->xwindow->overlay.snowflakes_cnt) {
				vwin->xwindow->overlay.snowflakes_cnt = 0;
				vwm_comp_damage_win(vwin->xwindow);
			}
			break;

		case XK_Right:	/* increase sampling frequency */
			if(sampling_interval + 1 < sizeof(sampling_intervals) / sizeof(sampling_intervals[0])) sampling_interval++;
			break;

		case XK_Left:	/* decrease sampling frequency, -1 pauses */
			if(sampling_interval >= 0) sampling_interval--;
			break;
		default:
			VWM_TRACE("Unhandled keycode: %x", (unsigned int)sym);
			break;
	}

	/* if what we're doing requests a grab, if not already grabbed, grab keyboard */
	if(!key_is_grabbed && do_grab) {
		XGrabKeyboard(display, RootWindow(display, screen_num), False, GrabModeAsync, GrabModeAsync, CurrentTime);
		key_is_grabbed = 1;
	}

	/* remember the symbol for repeater detection */
	last_sym = sym;
	last_state = keypress->xkey.state;
}


	/* some randomish things called from main() */

/* manage all existing windows (for startup) */
static int vwm_manage_existing(void)
{
	Window		root, parent;
	Window		*children = NULL;
	unsigned int	n_children, i;

	XGrabServer(display);
	XSync(display, False);
	XQueryTree(display, RootWindow(display, screen_num), &root, &parent, &children, &n_children);

	for(i = 0; i < n_children; i++) {
		if(children[i] == None) continue;

		if((vwm_xwin_create(children[i], VWM_GRABBED) == NULL)) goto _fail_grabbed;
	}

	XUngrabServer(display);

	if(children) XFree(children);

	return 1;

_fail_grabbed:
	XUngrabServer(display);

	if(children) XFree(children);

	return 0;
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

static int errhandler(Display *display, XErrorEvent *err)
{
	/* TODO */
	return 1;
}

int main(int argc, char *argv[])
{
	int		err = 0;
	int		done = 0;
	XEvent		event;
	Cursor		pointer;
	struct pollfd	pfd;
	char		*console_args[] = {"xterm", "-class", CONSOLE_WM_CLASS, "-e", "bash", "-c", "screen -D -RR " CONSOLE_SESSION_STRING, NULL};
	Window		bitmask;
	vwm_clickety_t	clickety = { .vwin = NULL };

#define reterr_if(_cond, _fmt, _args...) \
	err++;\
	if(_cond) {\
		VWM_ERROR(_fmt, ##_args);\
		return err;\
	}

	/* open connection with the server */
	reterr_if((display = XOpenDisplay(NULL)) == NULL, "Cannot open display");

	/* prevent children from inheriting the X connection */
	reterr_if(fcntl(ConnectionNumber(display), F_SETFD, FD_CLOEXEC) < 0, "Cannot set FD_CLOEXEC on X connection");

	/* get our scheduling priority, clients are launched with a priority LAUNCHED_RELATIVE_PRIORITY nicer than this */
	reterr_if((priority = getpriority(PRIO_PROCESS, getpid())) == -1, "Cannot get scheduling priority");

	XSetErrorHandler(errhandler);

	screen_num = DefaultScreen(display);

	reterr_if(!XQueryExtension (display, COMPOSITE_NAME, &composite_opcode, &composite_event, &composite_error), "No composite extension available");
	reterr_if(!XDamageQueryExtension(display, &damage_event, &damage_error), "No damage extension available");
	if(XSyncQueryExtension(display, &sync_event, &sync_error)) {
		/* set the window manager to the maximum X client priority */
		XSyncSetPriority(display, RootWindow(display, screen_num), 0x7fffffff);
	}

	if(XineramaQueryExtension(display, &xinerama_event, &xinerama_error)) {
		xinerama_screens = XineramaQueryScreens(display, &xinerama_screens_cnt);
	}

	if(XRRQueryExtension(display, &randr_event, &randr_error)) {
		XRRSelectInput(display, RootWindow(display, screen_num), RRScreenChangeNotifyMask);
	}

	/* allocate colors, I make assumptions about the X server's color capabilities since I'll only use this on modern-ish computers... */
	cmap = DefaultColormap(display, screen_num);

#define color(_sym, _str) \
	XAllocNamedColor(display, cmap, _str, &_sym ## _color, &_sym ## _color);
#include "colors.def"
#undef color

	wm_delete_atom = XInternAtom(display, "WM_DELETE_WINDOW", False);
	wm_protocols_atom = XInternAtom(display, "WM_PROTOCOLS", False);
	wm_pid_atom = XInternAtom(display, "_NET_WM_PID", False);

	XSelectInput(display, RootWindow(display, screen_num),
		     PropertyChangeMask | SubstructureNotifyMask | SubstructureRedirectMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
	XGrabKey(display, AnyKey, WM_GRAB_MODIFIER, RootWindow(display, screen_num), False, GrabModeAsync, GrabModeAsync);

	XFlush(display);

	XSetInputFocus(display, RootWindow(display, screen_num), RevertToPointerRoot, CurrentTime);

	/* initialize libvmon */
	vmon_init(&vmon, VMON_FLAG_2PASS, VMON_WANT_SYS_STAT, (VMON_WANT_PROC_STAT | VMON_WANT_PROC_FOLLOW_CHILDREN | VMON_WANT_PROC_FOLLOW_THREADS));
	vmon.proc_ctor_cb = vmon_ctor_cb;
	vmon.proc_dtor_cb = vmon_dtor_cb;
	vmon.sample_cb = sample_callback;

	/* get all the text and graphics stuff setup for overlays */
        reterr_if(!(overlay_font = XLoadQueryFont(display, OVERLAY_FIXED_FONT)), "failed to open font: " OVERLAY_FIXED_FONT);

	/* create a GC for rendering the text using Xlib into the text overlay stencils */
	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, 1, OVERLAY_MASK_DEPTH);
	text_gc = XCreateGC(display, bitmask, 0, NULL);
	XSetForeground(display, text_gc, WhitePixel(display, screen_num));
	XFreePixmap(display, bitmask);

	/* create some repeating source fill pictures for drawing through the text and graph stencils */
	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, 1, 32);
	overlay_text_fill = XRenderCreatePicture(display, bitmask, XRenderFindStandardFormat(display, PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(display, PictOpSrc, overlay_text_fill, &overlay_visible_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, 1, 32);
	overlay_shadow_fill = XRenderCreatePicture(display, bitmask, XRenderFindStandardFormat(display, PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(display, PictOpSrc, overlay_shadow_fill, &overlay_shadow_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, OVERLAY_ROW_HEIGHT, 32);
	overlay_bg_fill = XRenderCreatePicture(display, bitmask, XRenderFindStandardFormat(display, PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(display, PictOpSrc, overlay_bg_fill, &overlay_bg_color, 0, 0, 1, OVERLAY_ROW_HEIGHT);
	XRenderFillRectangle(display, PictOpSrc, overlay_bg_fill, &overlay_div_color, 0, OVERLAY_ROW_HEIGHT - 1, 1, 1);

	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, 1, 32);
	overlay_snowflakes_text_fill = XRenderCreatePicture(display, bitmask, XRenderFindStandardFormat(display, PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(display, PictOpSrc, overlay_snowflakes_text_fill, &overlay_snowflakes_visible_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, 1, 32);
	overlay_grapha_fill = XRenderCreatePicture(display, bitmask, XRenderFindStandardFormat(display, PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(display, PictOpSrc, overlay_grapha_fill, &overlay_grapha_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, 1, 32);
	overlay_graphb_fill = XRenderCreatePicture(display, bitmask, XRenderFindStandardFormat(display, PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(display, PictOpSrc, overlay_graphb_fill, &overlay_graphb_color, 0, 0, 1, 1);

	bitmask = XCreatePixmap(display, RootWindow(display, screen_num), 1, 2, 32);
	overlay_finish_fill = XRenderCreatePicture(display, bitmask, XRenderFindStandardFormat(display, PictStandardARGB32), CPRepeat, &pa_repeat);
	XRenderFillRectangle(display, PictOpSrc, overlay_finish_fill, &overlay_visible_color, 0, 0, 1, 1);
	XRenderFillRectangle(display, PictOpSrc, overlay_finish_fill, &overlay_trans_color, 0, 1, 1, 1);

	/* create initial virtual desktop */
	vwm_desktop_focus(vwm_desktop_create(NULL));
	vwm_desktop_mru(focused_desktop);

	/* manage all preexisting windows */
	vwm_manage_existing();

	/* create GC for logo drawing and window rubber-banding */
	gc = XCreateGC(display, RootWindow(display, screen_num), 0, NULL);
	XSetSubwindowMode(display, gc, IncludeInferiors);
	XSetFunction(display, gc, GXxor);

	/* launch the console here so it's likely ready by the time the logo animation finishes (there's no need to synchronize with it currently) */
	vwm_launch(console_args, VWM_LAUNCH_MODE_BG);

	/* first the logo color is the foreground */
	XSetForeground(display, gc, logo_color.pixel);
	vwm_draw_logo();

	/* change to the rubber-banding foreground color */
	XSetForeground(display, gc, rubberband_color.pixel);

	XClearWindow(display, RootWindow(display, screen_num));

	/* set the pointer */
	pointer = XCreateFontCursor(display, XC_X_cursor);
	XDefineCursor(display, RootWindow(display, screen_num), pointer);

	pfd.events = POLLIN;
	pfd.revents = 0;
	pfd.fd = ConnectionNumber(display);

	gettimeofday(&this_sample, NULL);
	while(!done) {
		do {
			static int	sampling_paused = 0;
			static int	contiguous_drops = 0;
			float		this_delta;

			gettimeofday(&maybe_sample, NULL);
			if((sampling_interval == -1 && !sampling_paused) || /* XXX this is kind of a kludge to get the 0 Hz indicator drawn before pausing */
			   (sampling_interval != -1 && ((this_delta = delta(&maybe_sample, &this_sample)) >= sampling_intervals[sampling_interval]))) {
				vmon_sys_stat_t	*sys_stat;

				/* automatically lower the sample rate if we can't keep up with the current sample rate */
				if(sampling_interval != -1 && sampling_interval <= prev_sampling_interval &&
				   this_delta >= (sampling_intervals[sampling_interval] * 1.5)) {
					contiguous_drops++;
					/* require > 1 contiguous drops before lowering the rate, tolerates spurious one-off stalls */
					if(contiguous_drops > 2) sampling_interval--;
				} else contiguous_drops = 0;

				/* age the sys-wide sample data into "last" variables, before the new sample overwrites them. */
				last_sample = this_sample;
				this_sample = maybe_sample;
				if((sys_stat = vmon.stores[VMON_STORE_SYS_STAT])) {
					last_user_cpu = sys_stat->user;
					last_system_cpu = sys_stat->system;
					last_total = 	sys_stat->user +
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

			XFlush(display);

			if(!XPending(display)) {
				/* TODO: make some effort to compute how long to sleep, but this is perfectly fine for now. */
				if(poll(&pfd, 1, sampling_interval != -1 ? sampling_intervals[sampling_interval] * 300.0 : -1) == 0) break;
			}

			XNextEvent(display, &event);
			switch(event.type) {
				case KeyPress:
					VWM_TRACE("keypress");
					vwm_keypressed(event.xkey.window, &event);
					break;

				case KeyRelease:
					VWM_TRACE("keyrelease");
					vwm_keyreleased(event.xkey.window, &event);
					break;

				case ButtonPress:
					VWM_TRACE("buttonpresss");
					vwm_clickety_pressed(&clickety, event.xbutton.window, &event.xbutton);
					break;

				case MotionNotify:
					//VWM_TRACE("motionnotify");
					vwm_clickety_motion(&clickety, event.xmotion.window, &event.xmotion);
					break;

				case ButtonRelease:
					VWM_TRACE("buttonrelease");
					vwm_clickety_released(&clickety, event.xbutton.window, &event.xbutton);
					break;

				case CreateNotify:
					VWM_TRACE("createnotify");
					vwm_xwin_create(event.xcreatewindow.window, VWM_NOT_GRABBED);
					break;

				case DestroyNotify: {
					vwm_xwindow_t	*xwin;
					VWM_TRACE("destroynotify");
					if((xwin = vwm_xwin_lookup(event.xdestroywindow.window))) {
						vwm_xwin_destroy(xwin);
					}
					break;
				}

				case ConfigureRequest: {
					XWindowChanges	changes = {
								.x = event.xconfigurerequest.x,		/* TODO: for now I don't manipulate anything */
								.y = event.xconfigurerequest.y,
								.width = event.xconfigurerequest.width,
								.height = event.xconfigurerequest.height,
								.border_width = WINDOW_BORDER_WIDTH /* except I do override whatever the border width may be */
							};
					unsigned long	change_mask = (event.xconfigurerequest.value_mask & (CWX | CWY | CWWidth | CWHeight)) | CWBorderWidth;
					/* XXX: windows raising themselves is annoying, so discard CWSibling and CWStackMode. */
					VWM_TRACE("configurerequest x=%i y=%i w=%i h=%i", changes.x, changes.y, changes.width, changes.height);
					XConfigureWindow(display, event.xconfigurerequest.window, change_mask, &changes);
					break;
				}

				case ConfigureNotify: {
					vwm_xwindow_t	*xwin;
					VWM_TRACE("configurenotify");
					if((xwin = vwm_xwin_lookup(event.xconfigure.window))) {
						XWindowAttributes	attrs;
						vwm_xwin_restack(xwin, event.xconfigure.above);
						XGetWindowAttributes(display, event.xconfigure.window, &attrs);
						if(compositing_mode) {
							/* damage the old and new window areas */
							XserverRegion	region;
							XRectangle	rects[2] = {	{	xwin->attrs.x,
												xwin->attrs.y,
												xwin->attrs.width + xwin->attrs.border_width * 2,
												xwin->attrs.height + xwin->attrs.border_width * 2 },
											{	attrs.x,
												attrs.y,
												attrs.width + attrs.border_width * 2,
												attrs.height + attrs.border_width * 2 } };

							region = XFixesCreateRegion(display, rects, 2);
							vwm_comp_damage_add(region);
							vwm_xwin_unbind_namewindow(xwin);
							vwm_xwin_bind_namewindow(xwin);
						}
						VWM_TRACE("pre x=%i y=%i w=%i h=%i\n", xwin->attrs.x, xwin->attrs.y, xwin->attrs.width, xwin->attrs.height);
						xwin->attrs = attrs;
						VWM_TRACE("post x=%i y=%i w=%i h=%i\n", xwin->attrs.x, xwin->attrs.y, xwin->attrs.width, xwin->attrs.height);
					}
					break;
				}

				case UnmapNotify: {
					vwm_xwindow_t	*xwin;
					VWM_TRACE("unmapnotify");
					/* unlike MapRequest, we simply are notified when a window is unmapped. */
					if((xwin = vwm_xwin_lookup(event.xunmap.window))) {
						if(xwin->managed) {
							if(xwin->managed->unmapping) {
								VWM_TRACE("swallowed vwm-induced UnmapNotify");
								xwin->managed->unmapping = 0;
							} else {
								/* client requested unmap, demote the window and note the unmapped state */
								vwm_win_unmanage(xwin->managed);
								xwin->mapped = 0;
							}
						} else {
							/* if it's not managed, we can't have caused the map */
							xwin->mapped = 0;
						}

						if(compositing_mode) vwm_comp_damage_win(xwin);
					}
					break;
				}

				case MapNotify: {
					vwm_xwindow_t	*xwin;
					VWM_TRACE("mapnotify");
					if((xwin = vwm_xwin_lookup(event.xmap.window))) {
						if(xwin->managed && xwin->managed->mapping) {
							VWM_TRACE("swallowed vwm-induced MapNotify");
						} else {
							/* some windows like popup dialog boxes bypass MapRequest */
							xwin->mapped = 1;
						}

						if(compositing_mode) {
							vwm_comp_damage_win(xwin);
							vwm_xwin_unbind_namewindow(xwin);
							vwm_xwin_bind_namewindow(xwin);
						}
					}
					break;
				}

				case MapRequest: {
					vwm_xwindow_t	*xwin;
					vwm_window_t	*vwin = NULL;
					int		domap = 1;
					VWM_TRACE("maprequest");
					if((xwin = vwm_xwin_lookup(event.xmap.window)) &&
					   ((vwin = xwin->managed) || (vwin = vwm_win_manage_xwin(xwin)))) {
						XWindowAttributes	attrs;
						XWindowChanges		changes = {.x = 0, .y = 0};
						XClassHint		*classhint;
						const vwm_screen_t	*scr;

						xwin->mapped = 1;	/* note that the client mapped the window */

						/* figure out if the window is the console */
						if((classhint = XAllocClassHint())) {
							if(XGetClassHint(display, event.xmap.window, classhint) && !strcmp(classhint->res_class, CONSOLE_WM_CLASS)) {
								console = vwin;
								vwm_win_shelve(vwin);
								vwm_win_autoconf(vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
								domap = 0;
							}

							if(classhint->res_class) XFree(classhint->res_class);
							if(classhint->res_name) XFree(classhint->res_name);
							XFree(classhint);
						}

						/* TODO: this is a good place to hook in a window placement algo */

						/* on client-requested mapping we place the window */
						if(!vwin->shelved) {
							/* we place the window on the screen containing the the pointer only if that screen is empty,
							 * otherwise we place windows on the screen containing the currently focused window */
							/* since we query the geometry of windows in determining where to place them, a configuring
							 * flag is used to exclude the window being configured from those queries */
							scr = vwm_screen_find(VWM_SCREEN_REL_POINTER);
							vwin->configuring = 1;
							if(vwm_screen_is_empty(scr)) {
								/* focus the new window if it isn't already focused when it's going to an empty screen */
								VWM_TRACE("window \"%s\" is alone on screen \"%i\", focusing", vwin->xwindow->name, scr->screen_number);
								vwm_win_focus(vwin);
							} else {
								scr = vwm_screen_find(VWM_SCREEN_REL_XWIN, focused_desktop->focused_window->xwindow);
							}
							vwin->configuring = 0;

							changes.x = scr->x_org;
							changes.y = scr->y_org;
						} else if(focused_context == VWM_CONTEXT_FOCUS_SHELF) {
							scr = vwm_screen_find(VWM_SCREEN_REL_XWIN, focused_shelf->xwindow);
							changes.x = scr->x_org;
							changes.y = scr->y_org;
						}

						/* XXX TODO: does this belong here? */
						XGetWMNormalHints(display, event.xmap.window, vwin->hints, &vwin->hints_supplied);
						XGetWindowAttributes(display, event.xmap.window, &attrs);

						vwin->client.x = changes.x;
						vwin->client.y = changes.y;

						vwin->client.height = attrs.height;
						vwin->client.width = attrs.width;

						XConfigureWindow(display, event.xmap.window, (CWX | CWY), &changes);
					}
				
					if(domap) {
						XMapWindow(display, event.xmap.window);
						if(vwin && vwin->desktop->focused_window == vwin) {
							XSync(display, False);
							XSetInputFocus(display, vwin->xwindow->id, RevertToPointerRoot, CurrentTime);
						}
					}
					break;
				}

				case PropertyNotify: {
					vwm_xwindow_t	*xwin;
					VWM_TRACE("property notify");
					if((xwin = vwm_xwin_lookup(event.xproperty.window)) &&
					   event.xproperty.atom == wm_pid_atom &&
					   event.xproperty.state == PropertyNewValue) vwm_xwin_monitor(xwin);
					break;
				}

				case MappingNotify:
					VWM_TRACE("mapping notify");
					XRefreshKeyboardMapping(&event.xmapping);
					break;

				case Expose:
					VWM_TRACE("expose");
					break;
				case GravityNotify:
					VWM_TRACE("gravitynotify");
					break;
				case ReparentNotify:
					VWM_TRACE("reparentnotify");
					break;
				default:
					if(event.type == randr_event + RRScreenChangeNotify) {
						VWM_TRACE("rrscreenchangenotify");
						if(xinerama_screens) XFree(xinerama_screens);
						xinerama_screens = XineramaQueryScreens(display, &xinerama_screens_cnt);

						if(compositing_mode) vwm_comp_invalidate_root();
					} else if(event.type == damage_event + XDamageNotify) {
						//VWM_TRACE("damagenotify");
						vwm_comp_damage_event((XDamageNotifyEvent *)&event);
					} else {
						VWM_ERROR("Unhandled X op %i", event.type);
					}
					break;
			}
		} while(QLength(display));

		if(combined_damage != None) { /* if there's damage to repaint, do it, this only happens when compositing for overlays is enabled */
			vwm_comp_paint_all();
			XSync(display, False);
		}
	}

	/* close connection to server */
	XFlush(display);
	XCloseDisplay(display);
 
	return 0;
}
