/*
 *  vwm - Vito's Window Manager, a minimal, non-reparenting X11 window manager
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

/* Work started 8/3/2012, in the form of research into creating WM's using xcb,
 * clearly I decided to just use Xlib instead at some point, and here we are.
 */

/* 4/25/2013 - Incorporated use of a screen session as a console/launcher.
 * This introduces a shelved-at-startup window containing an xterm running
 * a screen where all vwm-launched processes are executed with output
 * captured.  Most WM's I've used don't provide a way to access the
 * stdout/stderr of WM-launched applications.
 *
 * vwm now depends on GNU screen as a result.
 */

/* 6/09/2014 - After receiving a Xinerama-enabling patch from Philip Freeman,
 * Xinerama support was added using his patch as a basis.  There are still some
 * rough edges though as I don't generally work at a multihead desk.
 * See XINERAMA in the TODO file.
 */


#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/extensions/sync.h>	/* SYNC extension, enables us to give vwm the highest X client priority, helps keep vwm responsive at all times */
#include <X11/extensions/Xinerama.h>	/* XINERAMA extension, facilitates easy multihead awareness */
#include <X11/extensions/Xrandr.h>	/* RANDR extension, facilitates display configuration change awareness */
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
#define QUIT_CONSOLE_ON_EXIT							/* instruct the console screen session to quit @ exit */

typedef enum _vwm_context_focus_t {
	VWM_CONTEXT_FOCUS_OTHER = 0,						/* focus the other context relative to the current one */
	VWM_CONTEXT_FOCUS_DESKTOP,						/* focus the desktop context */
	VWM_CONTEXT_FOCUS_SHELF							/* focus the shelf context */
} vwm_context_focus_t;

typedef XineramaScreenInfo vwm_screen_t;					/* conveniently reuse the xinerama type for describing screens */

static LIST_HEAD(desktops);							/* global list of all (virtual) desktops in spatial created-in order */
static LIST_HEAD(desktops_mru);							/* global list of all (virtual) desktops in MRU order */
static LIST_HEAD(windows);							/* global list of all managed windows kept in MRU order */
static vwm_window_t		*console = NULL;				/* the console window */
static vwm_desktop_t		*focused_desktop = NULL;			/* currently focused (virtual) desktop */
static vwm_window_t		*focused_shelf = NULL;				/* currently focused shelved window */
static vwm_context_focus_t	focused_context = VWM_CONTEXT_FOCUS_DESKTOP;	/* currently focused context */ 
static int			key_is_grabbed = 0;				/* flag for tracking keyboard grab state */
static int			priority;					/* scheduling priority of the vwm process, launcher nices relative to this */
static unsigned long		fence_mask = 0;					/* global mask state for vwm_win_focus_next(... VWM_FENCE_MASKED_VIOLATE),
										 * if you use vwm on enough screens to overflow this, pics or it didn't happen. */

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
static int			sync_event, sync_error;
static int			xinerama_event, xinerama_error;
static int			randr_event, randr_error;
static XineramaScreenInfo	*xinerama_screens = NULL;
static int			xinerama_screens_cnt;

static void vwm_win_unmap(vwm_window_t *vwin);
static void vwm_win_map(vwm_window_t *vwin);
static void vwm_win_focus(vwm_window_t *vwin);
static void vwm_keypressed(Window win, XEvent *keypress);

#define MIN(_a, _b) ((_a) < (_b) ? (_a) : (_b))
#define MAX(_a, _b) ((_a) > (_b) ? (_a) : (_b))


/* helper for returning what fraction (0.0-1.0) of vwin overlaps with scr */
static float vwm_screen_overlaps_win(const vwm_screen_t *scr, vwm_window_t *vwin)
{
	float	pct = 0, xover = 0, yover = 0;

	if(scr->x_org + scr->width < vwin->config.x || scr->x_org > vwin->config.x + vwin->config.width ||
	   scr->y_org + scr->height < vwin->config.y || scr->y_org > vwin->config.y + vwin->config.height)
	   	goto _out;

	/* they overlap, by how much? */
	xover = MIN(scr->x_org + scr->width, vwin->config.x + vwin->config.width) - MAX(scr->x_org, vwin->config.x);
	yover = MIN(scr->y_org + scr->height, vwin->config.y + vwin->config.height) - MAX(scr->y_org, vwin->config.y);

	pct = (xover * yover) / (vwin->config.width * vwin->config.height);
_out:
	VWM_TRACE("xover=%f yover=%f width=%i height=%i pct=%.4f", xover, yover, vwin->config.width, vwin->config.height, pct);
	return pct;
}


/* helper for returning the correct screen, don't use the return value across event loops. */
typedef enum _vwm_screen_rel_t {
	VWM_SCREEN_REL_WINDOW,	/* return the screen the supplied window most resides in */
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
		case VWM_SCREEN_REL_WINDOW: {
			va_list		ap;
			vwm_window_t	*vwin;
			float		best_pct = 0, this_pct;

			va_start(ap, rel);
			vwin = va_arg(ap, vwm_window_t *);
			va_end(ap);

			for_each_screen(scr) {
				this_pct = vwm_screen_overlaps_win(scr, vwin);
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


/* helper for determining if a screen contains any windows (assuming the current desktop) */
static int vwm_screen_is_empty(const vwm_screen_t *scr)
{
	vwm_window_t	*vwin;
	int		is_empty = 1;

	list_for_each_entry(vwin, &windows, windows) {
		if(vwin->desktop == focused_desktop && !vwin->shelved && !vwin->configuring) {
			/* XXX: it may make more sense to see what %age of the screen is overlapped by windows, and consider it empty if < some % */
			/*      This is just seeing if any window is predominantly within the specified screen, the rationale being if you had a focusable
			 *      window on the screen you would have used the keyboard to make windows go there; this function is only used in determining
			 *      wether a new window should go where the pointer is or not. */
			if(vwm_screen_overlaps_win(scr, vwin) >= 0.05) {
				is_empty = 0;
				break;
			}
		}
	}

	return is_empty;
}


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


/* XXX: for now just double forks to avoid having to collect return statuses of the long-running process */
typedef enum _vwm_launch_mode_t {
	VWM_LAUNCH_MODE_FG,
	VWM_LAUNCH_MODE_BG,
} vwm_launch_mode_t;

static void vwm_launch(char **argv, vwm_launch_mode_t mode)
{
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


/* switch to the desired context if it isn't already the focused one, inform caller if anything happened */
static int vwm_context_focus(vwm_context_focus_t desired_context)
{
	vwm_context_focus_t	entry_context = focused_context;

	switch(focused_context) {
		vwm_window_t		*vwin;

		case VWM_CONTEXT_FOCUS_SHELF:
			if(desired_context == VWM_CONTEXT_FOCUS_SHELF) break;

			/* desired == DESKTOP && focused == SHELF */

			VWM_TRACE("unmapping shelf window \"%s\"", focused_shelf->name);
			vwm_win_unmap(focused_shelf);
			XFlush(display); /* for a more responsive feel */

			/* map the focused desktop */
			list_for_each_entry(vwin, &windows, windows) {
				if(vwin->desktop == focused_desktop && !vwin->shelved) {
					VWM_TRACE("Mapping desktop window \"%s\"", vwin->name);
					vwm_win_map(vwin);
				}
			}

			if(focused_desktop->focused_window) {
				VWM_TRACE("Focusing \"%s\"", focused_desktop->focused_window->name);
				XSetInputFocus(display, focused_desktop->focused_window->window, RevertToPointerRoot, CurrentTime);
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
				list_for_each_entry(vwin, &windows, windows) {
					if(vwin->desktop == focused_desktop) {
						VWM_TRACE("Unmapping desktop window \"%s\"", vwin->name);
						vwm_win_unmap(vwin);
					}
				}

				XFlush(display); /* for a more responsive feel */

				VWM_TRACE("Mapping shelf window \"%s\"", focused_shelf->name);
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


/* make the specified desktop the most recently used one */
static void vwm_desktop_mru(vwm_desktop_t *d)
{
	VWM_TRACE("MRU desktop: %p", d);
	list_move(&d->desktops_mru, &desktops_mru);
}


/* focus a virtual desktop */
/* this switches to the desktop context if necessary, maps and unmaps windows accordingly if necessary */
static int vwm_desktop_focus(vwm_desktop_t *d)
{
	XGrabServer(display);
	XSync(display, False);

	/* if the context switched and the focused desktop is the desired desktop there's nothing else to do */
	if((vwm_context_focus(VWM_CONTEXT_FOCUS_DESKTOP) && focused_desktop != d) || focused_desktop != d) {
		vwm_window_t	*w;

		/* unmap the windows on the currently focused desktop, map those on the newly focused one */
		list_for_each_entry(w, &windows, windows) {
			if(w->shelved) continue;
			if(w->desktop == focused_desktop) vwm_win_unmap(w);
		}

		XFlush(display);

		list_for_each_entry(w, &windows, windows) {
			if(w->shelved) continue;
			if(w->desktop == d) vwm_win_map(w);
		}

		focused_desktop = d;
	}

	/* directly focus the desktop's focused window if there is one, we don't use vwm_win_focus() intentionally XXX */
	if(focused_desktop->focused_window) {
		VWM_TRACE("Focusing \"%s\"", focused_desktop->focused_window->name);
		XSetInputFocus(display, focused_desktop->focused_window->window, RevertToPointerRoot, CurrentTime);
	}

	XUngrabServer(display);

	return 1;
}


/* create a virtual desktop */
static vwm_desktop_t * vwm_desktop_create(char *name)
{
	vwm_desktop_t	*d;

	d = malloc(sizeof(vwm_desktop_t));
	if(d == NULL) {
		VWM_PERROR("Failed to allocate desktop");
		goto _fail;
	}

	d->name = name == NULL ? name : strdup(name);
	d->focused_window = NULL;

	list_add_tail(&d->desktops, &desktops);
	list_add_tail(&d->desktops_mru, &desktops_mru);

	return d;

_fail:
	return NULL;
}


/* destroy a virtual desktop */
static void vwm_desktop_destroy(vwm_desktop_t *d)
{
	/* silently refuse to destroy a desktop having windows (for now) */
	/* there's _always_ a focused window on a desktop having mapped windows */
	/* also silently refuse to destroy the last desktop (for now) */
	if(d->focused_window || (d->desktops.next == d->desktops.prev)) return;

	/* focus the MRU desktop that isn't this one if we're the focused desktop */
	if(d == focused_desktop) {
		vwm_desktop_t	*next_desktop;

		list_for_each_entry(next_desktop, &desktops_mru, desktops_mru) {
			if(next_desktop != d) {
				vwm_desktop_focus(next_desktop);
				break;
			}
		}
	}

	list_del(&d->desktops);
	list_del(&d->desktops_mru);
}


/* unmap the specified window and set the unmapping-in-progress flag so we can discard vwm-generated UnmapNotify events */
static void vwm_win_unmap(vwm_window_t *vwin)
{
	VWM_TRACE("Unmapping \"%s\"", vwin->name);
	vwin->unmapping = 1;
	XUnmapWindow(display, vwin->window);
}


/* map the specified window */
static void vwm_win_map(vwm_window_t *vwin)
{
	VWM_TRACE("Mapping \"%s\"", vwin->name);
	XMapWindow(display, vwin->window);
}


/* make the specified window the most recently used one */
static void vwm_win_mru(vwm_window_t *vwin)
{
	list_move(&vwin->windows, &windows);
}


/* send a client message to a window (currently used for WM_DELETE) */
static void vwm_win_message(vwm_window_t *vwin, Atom type, long foo)
{
	XEvent	event;

	memset(&event, 0, sizeof(event));
	event.xclient.type = ClientMessage;
	event.xclient.window = vwin->window;
	event.xclient.message_type = type;
	event.xclient.format = 32;
	event.xclient.data.l[0] = foo;
	event.xclient.data.l[1] = CurrentTime;	/* XXX TODO: is CurrentTime actually correct to use for this purpose? */

	XSendEvent(display, vwin->window, False, 0, &event);
}


/* looks up the window in the global window list */
static vwm_window_t * vwm_win_lookup(Window win)
{
	vwm_window_t	*tmp, *vwin = NULL;

	list_for_each_entry(tmp, &windows, windows) {
		if(tmp->window == win) {
			vwin = tmp;
			break;
		}
	}

	return vwin;
}


/* helper for returning the currently focused window (considers current context...), may return NULL */
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
	if(vwin->autoconfigured == VWM_WIN_AUTOCONF_NONE) vwin->client = vwin->config;

	scr = vwm_screen_find(rel, vwin);
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

	XConfigureWindow(display, vwin->window, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &changes);
	vwin->autoconfigured = conf;
}


/* focus a window */
/* this updates window border color as needed and the X input focus */
static void vwm_win_focus(vwm_window_t *vwin)
{
	VWM_TRACE("focusing: %#x", (unsigned int)vwin->window);

	/* change the focus to the new window */
	XSetInputFocus(display, vwin->window, RevertToPointerRoot, CurrentTime);

	/* update the border color accordingly */
	if(vwin->shelved) {
		/* set the border of the newly focused window to the shelved color */
		XSetWindowBorder(display, vwin->window, vwin == console ? shelved_console_border_color.pixel : shelved_window_border_color.pixel);
		/* fullscreen windows in the shelf when focused, since we don't intend to overlap there */
		vwm_win_autoconf(vwin, VWM_SCREEN_REL_POINTER, VWM_WIN_AUTOCONF_FULL);	/* XXX TODO: for now the shelf follows the pointer, it's simple. */
	} else {
		if(vwin->desktop == focused_desktop && focused_desktop->focused_window) {
			/* if we've changed focus within the same desktop, set the currently focused window border to the
			 * unfocused color.  Otherwise, we want to leave the focused color on the old window on the old desktop */
			XSetWindowBorder(display, focused_desktop->focused_window->window, unfocused_window_border_color.pixel);
		}

		/* set the border of the newly focused window to the focused color */
		XSetWindowBorder(display, vwin->window, focused_window_border_color.pixel);

		/* persist this on a per-desktop basis so it can be restored on desktop switches */
		focused_desktop->focused_window = vwin;
	}
}


/* focus the next window on a virtual desktop relative to the supplied window, in the specified context, respecting screen boundaries according to fence. */
typedef enum _vwm_fence_t {
	VWM_FENCE_IGNORE = 0,		/* behave as if screen boundaries don't exist (like the pre-Xinerama code) */
	VWM_FENCE_RESPECT,		/* confine operation to within the screen */
	VWM_FENCE_TRY_RESPECT,		/* confine operation to within the screen, unless no options exist. */
	VWM_FENCE_VIOLATE,		/* leave the screen for any other*/
	VWM_FENCE_MASKED_VIOLATE	/* leave the screen for any other not masked */
} vwm_fence_t;

static vwm_window_t * vwm_win_focus_next(vwm_window_t *vwin, vwm_context_focus_t context, vwm_fence_t fence)
{
	const vwm_screen_t	*scr = vwm_screen_find(VWM_SCREEN_REL_WINDOW, vwin), *next_scr = NULL;
	vwm_window_t		*next;
	unsigned long		visited_mask;

_retry:
	visited_mask = 0;
	list_for_each_entry(next, &vwin->windows, windows) {
		/* searching for the next mapped window in this context, using vwin->windows as the head */
		if(&next->windows == &windows) continue;	/* XXX: skip the containerless head, we're leveraging the circular list implementation */

		if((context == VWM_CONTEXT_FOCUS_SHELF && next->shelved) ||
		   ((context == VWM_CONTEXT_FOCUS_DESKTOP && !next->shelved && next->desktop == focused_desktop) &&
		    (fence == VWM_FENCE_IGNORE ||
		     ((fence == VWM_FENCE_RESPECT || fence == VWM_FENCE_TRY_RESPECT) && vwm_screen_find(VWM_SCREEN_REL_WINDOW, next) == scr) ||
		     (fence == VWM_FENCE_VIOLATE && vwm_screen_find(VWM_SCREEN_REL_WINDOW, next) != scr) ||
		     (fence == VWM_FENCE_MASKED_VIOLATE && (next_scr = vwm_screen_find(VWM_SCREEN_REL_WINDOW, next)) != scr &&
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
				XRaiseWindow(display, next->window);
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

	VWM_TRACE("\"%s\"", next->name);

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


typedef enum _vwm_grab_mode_t {
	VWM_NOT_GRABBED = 0,
	VWM_GRABBED
} vwm_grab_mode_t;

/* manages a mapped window (called in response to MapRequest events, and during startup to manage existing windows) */
static vwm_window_t * vwm_win_manage(Window win, vwm_grab_mode_t grabbed)
{
	XWindowAttributes	attrs;
	vwm_window_t		*vwin, *focused;

	/* prevent races */
	if(!grabbed) {
		XGrabServer(display);
		XSync(display, False);
	}

	/* verify the window still exists */
	if(!XGetWindowAttributes(display, win, &attrs)) goto _fail_grabbed;

	/* honor overrides, don't manage InputOnly windows */
#ifdef HONOR_OVERRIDE_REDIRECT
	if(attrs.override_redirect || attrs.class == InputOnly) goto _fail_grabbed;
#else
	if(attrs.class == InputOnly) goto _fail_grabbed;
#endif

	VWM_TRACE("Managing %#x", (unsigned int)win);

	/* allocate and initialize our per-managed-window structure and get it on the global windows list */
	vwin = (vwm_window_t *)malloc(sizeof(vwm_window_t));
	if(vwin == NULL) {
		VWM_PERROR("Failed to allocate vwin");
		goto _fail_grabbed;
	}

        XUngrabButton(display, AnyButton, AnyModifier, win);
	XGrabButton(display, AnyButton, WM_GRAB_MODIFIER, win, False, (PointerMotionMask | ButtonPressMask | ButtonReleaseMask), GrabModeAsync, GrabModeAsync, None, None);
	XGrabKey(display, AnyKey, WM_GRAB_MODIFIER, win, False, GrabModeAsync, GrabModeAsync);
	XSetWindowBorder(display, win, unfocused_window_border_color.pixel);

	vwin->name = NULL;
	XFetchName(display, win, &vwin->name);

	vwin->hints = XAllocSizeHints();
	if(!vwin->hints) {
		VWM_PERROR("Failed to allocate WM hints");
		goto _fail_grabbed;
	}
	XGetWMNormalHints(display, win, vwin->hints, &vwin->hints_supplied);

	vwin->desktop = focused_desktop;
	vwin->window = win;
	vwin->autoconfigured = VWM_WIN_AUTOCONF_NONE;
	vwin->unmapping = 0;
	vwin->configuring = 0;
	vwin->shelved = (focused_context == VWM_CONTEXT_FOCUS_SHELF);	/* if we're in the shelf when the window is created, the window is shelved */

	/* remember whatever the current attributes are */
	vwin->client.x = attrs.x;
	vwin->client.y = attrs.y;
	vwin->client.width = attrs.width;
	vwin->client.height = attrs.height;
	vwin->config = vwin->client;

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

	/* put it on the global windows list, if there's a focused window insert the new one after it */
	if(!list_empty(&windows) && (focused = vwm_win_focused())) {
		/* insert the vwin immediately after the focused window, so Mod1+Tab goes to the new window */
		list_add(&vwin->windows, &focused->windows);
	} else {
		list_add(&vwin->windows, &windows);
	}

	/* always raise newly managed windows so we know about them. */
	XRaiseWindow(display, vwin->window);

	/* if the desktop has no focused window yet, automatically focus the newly managed one, provided we're on the desktop context */
	if(!focused_desktop->focused_window && focused_context == VWM_CONTEXT_FOCUS_DESKTOP) {
		VWM_TRACE("Mapped new window \"%s\" is alone on desktop \"%s\", focusing", vwin->name, focused_desktop->name);
		vwm_win_focus(vwin);
	}

	if(!grabbed) XUngrabServer(display);

	return vwin;

_fail_grabbed:
	if(!grabbed) XUngrabServer(display);

	return NULL;
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


/* migrate a window to a new desktop, focuses the destination desktop as well */
static void vwm_win_migrate(vwm_window_t *vwin, vwm_desktop_t *desktop)
{
	/* go through the motions of unfocusing the window if it is focused */
	vwm_win_unfocus(vwin);

	/* ensure not shelved */
	vwin->shelved = 0;

	/* assign the new desktop */
	vwin->desktop = desktop;

	/* currently we always focus the new desktop in a migrate */
	vwm_desktop_focus(desktop);

	/* focus the window so borders get updated */
	vwm_win_focus(vwin);
	vwm_win_mru(vwin); /* TODO: is this right? shouldn't the Mod1 release be what's responsible for this? I migrate so infrequently it probably doesn't matter */

	/* ensure the window is raised */
	XRaiseWindow(display, vwin->window);
}


/* unmanages a managed window (called in response to DestroyNotify) */
static void vwm_win_unmanage(vwm_window_t *vwin)
{
#if 0
/*
I'm not sure how I feel about this yet, I like the flow of new windows queueing up as the next most recently used and on top of the stack, where I can
switch to the first one with Mod1-Tab, then destroy them all in sequence, never losing visibility of the next one.
When this is enabled, that doesn't happen, I see them all stacked on top, switch to the top one and destroy it, then my origin window gets raised and
focused as it's the truly most recently used window (the last one I committed on and was interacting with before giving attention to the new stack).
Without this change, it's a nice flow.
But without this change, odd edge cases emerge where a clearly not MRU window is raised after I destroy something, like the xterm running an mplayer command
when I switched from the movie window to a new floater xterm momentarily and destroyed it immediately without releasing Mod1.  What happens is the floater xterm
gets unmanaged on destroy, and the movie window should get focused, but it doesn't, the window _after_ the movie window does, which is the xterm running mplayer,
because the unfocus() of the floater xterm looked at whatever was after it, since the new floater was inserted immediately after the movie window, it's between
the movie window and the xterm running mplayer, so the xterm running mplayer gets focused + raised.
Needs more thought.
*/
	vwm_win_mru(vwin); /* bump vwin to the mru head before unfocusing so we always move focus to the current head on unmanage of the focused window */
#endif
	vwm_win_unfocus(vwin);
	list_del(&vwin->windows);

	if(vwin == console) console = NULL;

	if(vwin->name) XFree(vwin->name);
	free(vwin);
}


/* manage all existing windows (for startup) */
static int vwm_manage_existing(void)
{
	Window		root, parent;
	Window		*children = NULL;
	unsigned int	n_children, i;
	vwm_window_t	*vwin;

	XGrabServer(display);
	XSync(display, False);
	XQueryTree(display, RootWindow(display, screen_num), &root, &parent, &children, &n_children);

	for(i = 0; i < n_children; i++) {
		if(children[i] == None) continue;

		if((vwin = vwm_win_manage(children[i], VWM_GRABBED)) == NULL) goto _fail_grabbed;
	}

	XUngrabServer(display);

	if(children) XFree(children);

	return 1;

_fail_grabbed:
	XUngrabServer(display);

	if(children) XFree(children);

	return 0;
}


/* helper function for resizing a window, how the motion is applied depends on where in the window the impetus event occurred */
static void compute_resize(vwm_window_t *vwin, XWindowAttributes *attrs, XEvent *impetus, XEvent *terminus, XWindowAttributes *new)
{
	int	dw = (attrs->width / 2);
	int	dh = (attrs->height / 2);
	int	xdelta = (terminus->xbutton.x_root - impetus->xbutton.x_root);
	int	ydelta = (terminus->xbutton.y_root - impetus->xbutton.y_root);
	int	min_width = 0, min_height = 0;
	int	width_inc = 1, height_inc = 1;

	/* TODO: there's probably a problem here WRT border width, I probably should be considering it. */

	if(vwin && vwin->hints) {
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

	if(impetus->xbutton.x < dw && impetus->xbutton.y < dh) {
		/* grabbed top left */
		new->x = attrs->x + xdelta;
		new->y = attrs->y + ydelta;
		new->width = attrs->width - xdelta;
		new->height = attrs->height - ydelta;
	} else if(impetus->xbutton.x > dw && impetus->xbutton.y < dh) {
		/* grabbed top right */
		new->x = attrs->x;
		new->y = attrs->y + ydelta;
		new->width = attrs->width + xdelta;
		new->height = attrs->height - ydelta;
	} else if(impetus->xbutton.x < dw && impetus->xbutton.y > dh) {
		/* grabbed bottom left */
		new->x = attrs->x + xdelta;
		new->y = attrs->y;
		new->width = attrs->width - xdelta;
		new->height = attrs->height + ydelta;
	} else if(impetus->xbutton.x > dw && impetus->xbutton.y > dh) {
		/* grabbed bottom right */
		new->x = attrs->x;
		new->y = attrs->y;
		new->width = attrs->width + xdelta;
		new->height = attrs->height + ydelta;
	}

	/* constrain the width and height of the window according to the minimums */
	if(new->width < min_width) {
		if(attrs->x != new->x) new->x -= (min_width - new->width);
		new->width = min_width;
	}

	if(new->height < min_height) {
		if(attrs->y != new->y) new->y -= (min_height - new->height);
		new->height = min_height;
	}
}


typedef enum _vwm_adjust_mode_t {
	VWM_ADJUST_RESIZE,
	VWM_ADJUST_MOVE
} vwm_adjust_mode_t;

/* XXX: impetus is currently assumed to be an xbutton event */
static int vwm_clicked(Window win, XEvent *impetus)
{
	XWindowAttributes	orig, lastrect;
	XWindowChanges		changes = { .border_width = WINDOW_BORDER_WIDTH };
	vwm_window_t		*vwin;

	/* verify the window still exists */
	if(!XGetWindowAttributes(display, win, &orig)) goto _fail;

	if(!(vwin = vwm_win_lookup(win))) goto _fail;

	if(impetus->xbutton.state & WM_GRAB_MODIFIER) {
		int			finished = 0;
		vwm_adjust_mode_t	mode;

		/* always set the input focus to the clicked window, note if we allow this to happen on the root window, it enters sloppy focus mode
		 * until a non-root window is clicked, which is an interesting hybrid but not how I prefer it. */
		if(vwin != focused_desktop->focused_window && vwin->window != RootWindow(display, screen_num)) {
			vwm_win_focus(vwin);
			vwm_win_mru(vwin);
		}

		switch(impetus->xbutton.button) {
			case Button1:
				/* immediately raise the window if we're relocating,
				 * resizes are supported without raising (which also enables NULL resizes to focus without raising) */
				mode = VWM_ADJUST_MOVE;
				XRaiseWindow(display, win);
				break;

			case Button3:
				/* grab the server on resize for the xor rubber-banding's sake */
				XGrabServer(display);
				XSync(display, False);

				/* FIXME: none of the resize DrawRectangle() calls consider the window border. */
				XDrawRectangle(display, RootWindow(display, screen_num), gc, orig.x, orig.y, orig.width, orig.height);
				lastrect = orig;

				mode = VWM_ADJUST_RESIZE;
				break;

			default:
				goto _fail;
		}

		/* apply Motion events until the button is released, move vs. move+resize is performed depending on the supplied mode */
		while(!finished) {
			XEvent			event;
			XWindowAttributes	resized;

			XWindowEvent(display, win, ButtonReleaseMask | PointerMotionMask, &event);
			switch(event.type) {
				case ButtonRelease:
					switch(mode) {
						case VWM_ADJUST_MOVE:
							changes.x = orig.x + (event.xmotion.x_root - impetus->xbutton.x_root);
							changes.y = orig.y + (event.xmotion.y_root - impetus->xbutton.y_root);
							XConfigureWindow(display, win, CWX | CWY | CWBorderWidth, &changes);
							break;

						case VWM_ADJUST_RESIZE:
							compute_resize(vwin, &orig, impetus, &event, &resized);
							/* move and resize the window @ resized */
							XDrawRectangle(display, RootWindow(display, screen_num), gc, lastrect.x, lastrect.y, lastrect.width, lastrect.height);
							changes.x = resized.x;
							changes.y = resized.y;
							changes.width = resized.width;
							changes.height = resized.height;
							XConfigureWindow(display, win, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &changes);
							XUngrabServer(display);

							break;

						default:
							break;
					}
					/* once you manipulate the window it's no longer fullscreened, simply hitting Mod1+Return once will restore fullscreened mode */
					vwin->autoconfigured = VWM_WIN_AUTOCONF_NONE;
					finished = 1;
					break;

				case MotionNotify:
					switch(mode) {
						case VWM_ADJUST_MOVE:
							changes.x = orig.x + (event.xmotion.x_root - impetus->xbutton.x_root);
							changes.y = orig.y + (event.xmotion.y_root - impetus->xbutton.y_root);
							XConfigureWindow(display, win, CWX | CWY | CWBorderWidth, &changes);
							break;

						case VWM_ADJUST_RESIZE:
							/* XXX: it just so happens the XMotionEvent structure is identical to XButtonEvent in the fields
							 * needed by compute_resize... */
							compute_resize(vwin, &orig, impetus, &event, &resized);
							/* erase the last rectangle */
							XDrawRectangle(display, RootWindow(display, screen_num), gc, lastrect.x, lastrect.y, lastrect.width, lastrect.height);
							/* draw a frame @ resized coordinates */
							XDrawRectangle(display, RootWindow(display, screen_num), gc, resized.x, resized.y, resized.width, resized.height);
							/* remember the last rectangle */
							lastrect = resized;
							break;

						default:
							break;
					}
					break;

				default:
					VWM_ERROR("unexpected event during window adjust %i", event.type);
					break;
			}
		}
	}

	XFlush(display);
	XUngrabPointer(display, CurrentTime);

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
#ifdef QUIT_CONSOLE_ON_EXIT
	char					*quit_console_args[] = {"bash", "-c", "screen -dr " CONSOLE_SESSION_STRING " -X quit", NULL};
#endif

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
					XKillClient(display, vwin->window);
				} else { /* kindly destroy the focused window */
					vwm_win_message(vwin, wm_protocols_atom, wm_delete_atom);
				}
			} else if(focused_context == VWM_CONTEXT_FOCUS_DESKTOP) {
				/* destroy the focused desktop when destroy occurs without any windows */
				vwm_desktop_destroy(focused_desktop);
			}
			break;

		case XK_Escape: /* leave VWM rudely, after triple press */
			do_grab = 1;

			if(repeat_cnt == 2) {
#ifdef QUIT_CONSOLE_ON_EXIT
				vwm_launch(quit_console_args, VWM_LAUNCH_MODE_FG);
#endif
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

					XRaiseWindow(display, vwin->window);

					if(repeat_cnt == 1) {
						/* double: reraise & fullscreen */
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_FULL);
					} else if(repeat_cnt == 2) {
						 /* triple: reraise & fullscreen w/borders obscured by screen perimiter */
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_ALL);
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
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_FULL);
					} else {
						XLowerWindow(display, vwin->window);
					}
					XFlush(display);
				}
			}
			break;

		case XK_Return: /* (full-screen / restore) focused window */
			if((vwin = vwm_win_focused())) {
				if(vwin->autoconfigured) {
					vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_NONE);
				} else {
					vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_FULL);
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
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_TOP);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_TOP_LEFT);
					}
				} else {
					if(!repeat_cnt) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_LEFT);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_BOTTOM_LEFT);
					}
				}
			}
			break;

		case XK_bracketright:	/* reconfigure the focused window to occupy the right or bottom half of the screen or right quarters on repeat */
			if((vwin = vwm_win_focused())) {
				do_grab = 1;

				if(keypress->xkey.state & ShiftMask) {
					if(!repeat_cnt) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_BOTTOM);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_BOTTOM_RIGHT);
					}
				} else {
					if(!repeat_cnt) {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_RIGHT);
					} else {
						vwm_win_autoconf(vwin, VWM_SCREEN_REL_WINDOW, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_TOP_RIGHT);
					}
				}
			}
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


static int errhandler(Display *display, XErrorEvent *err)
{
	/* TODO */
	return 1;
}

int main(int argc, char *argv[])
{
	int	err = 0;
	int	done = 0;
	XEvent	event;
	Cursor	pointer;
	char	*console_args[] = {"xterm", "-class", CONSOLE_WM_CLASS, "-e", "bash", "-c", "screen -D -RR " CONSOLE_SESSION_STRING, NULL};

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

	XSelectInput(display, RootWindow(display, screen_num),
		     SubstructureNotifyMask | SubstructureRedirectMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
	XGrabKey(display, AnyKey, WM_GRAB_MODIFIER, RootWindow(display, screen_num), False, GrabModeAsync, GrabModeAsync);

	XFlush(display);

	XSetInputFocus(display, RootWindow(display, screen_num), RevertToPointerRoot, CurrentTime);

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

	/* event loop */
	while(!done) {
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
				vwm_clicked(event.xbutton.window, &event);
				break;


			case DestroyNotify: {
				vwm_window_t	*vwin;
				VWM_TRACE("destroynotify");
				if((vwin = vwm_win_lookup(event.xdestroywindow.window))) {
					vwm_win_unmanage(vwin);
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
				vwm_window_t	*vwin;
				VWM_TRACE("configurenotify");
				if((vwin = vwm_win_lookup(event.xconfigure.window))) {
					vwin->config.x = event.xconfigure.x;
					vwin->config.y = event.xconfigure.y;
					vwin->config.width = event.xconfigure.width;
					vwin->config.height = event.xconfigure.height;
				}
				break;
			}

			case UnmapNotify: {
				vwm_window_t	*vwin;
				VWM_TRACE("unmapnotify");
				/* unlike MapRequest, we simply are notified when a window is unmapped. */
				if((vwin = vwm_win_lookup(event.xunmap.window))) {
					if(vwin->unmapping) {
						VWM_TRACE("swallowed vwm-induced UnmapNotify");
						vwin->unmapping = 0;
					} else {
						vwm_win_unmanage(vwin);
					}
				}
				break;
			}

			case MapNotify: {
				vwm_window_t	*vwin;
				VWM_TRACE("mapnotify");
				if(!(vwin = vwm_win_lookup(event.xmap.window))) {
					/* unmanaged windows becoming mapped arrive here, popups/menus and the like, if they
					 * don't want to be managed they'll set override_redirect, which will be ignored or honored depending on
					 * HONOR_OVERRIDE_REDIRECT, if we honor it this generally becomes a noop. */
					vwm_win_manage(event.xmap.window, VWM_NOT_GRABBED);
				} else {
					VWM_TRACE("swallowed vwm-induced MapNotify");
				}
				break;
			}

			case MapRequest: {
				vwm_window_t	*vwin;
				int		domap = 1;
				VWM_TRACE("maprequest");
				if((vwin = vwm_win_lookup(event.xmap.window)) || (vwin = vwm_win_manage(event.xmap.window, VWM_NOT_GRABBED))) {
					XWindowAttributes	attrs;
					XWindowChanges		changes = {.x = 0, .y = 0};
					XClassHint		*classhint;
					const vwm_screen_t	*scr;

					/* figure out if the window is the console */
					if((classhint = XAllocClassHint())) {
						if(XGetClassHint(display, event.xmap.window, classhint) && !strcmp(classhint->res_class, CONSOLE_WM_CLASS)) {
							console = vwin;
							vwm_win_shelve(vwin);
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
							VWM_TRACE("window \"%s\" is alone on screen \"%i\", focusing", vwin->name, scr->screen_number);
							vwm_win_focus(vwin);
						} else {
							scr = vwm_screen_find(VWM_SCREEN_REL_WINDOW, focused_desktop->focused_window);
						}
						vwin->configuring = 0;

						changes.x = scr->x_org;
						changes.y = scr->y_org;
					} else if(focused_context == VWM_CONTEXT_FOCUS_SHELF) {
						scr = vwm_screen_find(VWM_SCREEN_REL_WINDOW, focused_shelf);
						changes.x = scr->x_org;
						changes.y = scr->y_org;
					}

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
						XSetInputFocus(display, vwin->window, RevertToPointerRoot, CurrentTime);
					}
				}
				break;
			}

			case MappingNotify:
				VWM_TRACE("mapping notify");
				XRefreshKeyboardMapping(&event.xmapping);
				break;

			case ButtonRelease:
				VWM_TRACE("buttonrelease");
				break;
			case CirculateNotify:
				VWM_TRACE("circulatenotify");
				break;
			case CreateNotify:
				VWM_TRACE("createnotify");
				break;
			case Expose:
				VWM_TRACE("expose");
				break;
			case GravityNotify:
				VWM_TRACE("gravitynotify");
				break;
			case MotionNotify:
				VWM_TRACE("motionnotify");
				break;
			case ReparentNotify:
				VWM_TRACE("reparentnotify");
				break;
			default:
				if(event.type == randr_event + RRScreenChangeNotify) {
					VWM_TRACE("rrscreenchangenotify");
					if(xinerama_screens) XFree(xinerama_screens);
					xinerama_screens = XineramaQueryScreens(display, &xinerama_screens_cnt);
				} else {
					VWM_ERROR("Unhandled X op %i", event.type);
				}
				break;
		}
	}

	/* close connection to server */
	XFlush(display);
	XCloseDisplay(display);
 
	return 0;
}
