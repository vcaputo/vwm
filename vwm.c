/*
 *  VWM - Vito's Window Manager, a minimal, non-reparenting X11 window manager
 *
 *  Copyright (C) 2012  Vito Caputo - <vcaputo@gnugeneration.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
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

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "vwm.h"

#define WINDOW_BORDER_WIDTH			1
#define WINDOW_CASCADE_DELTA			100

typedef enum _vwm_context_focus_t {
	VWM_CONTEXT_FOCUS_OTHER = 0,
	VWM_CONTEXT_FOCUS_DESKTOP,
	VWM_CONTEXT_FOCUS_SHELF
} vwm_context_focus_t;


static LIST_HEAD(desktops);							/* global list of all (virtual) desktops in spatial created-in order */
static LIST_HEAD(desktops_mru);							/* global list of all (virtual) desktops in MRU order */
static LIST_HEAD(windows);							/* global list of all managed windows kept in MRU order */
static vwm_desktop_t		*focused_desktop = NULL;			/* currently focused (virtual) desktop */
static vwm_window_t		*focused_shelf = NULL;				/* currently focused shelved window */
static vwm_context_focus_t	focused_context = VWM_CONTEXT_FOCUS_DESKTOP;	/* currently focused context */ 
static int			key_is_grabbed = 0;				/* flag for tracking keyboard grab state */

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

static void vwm_win_focus(vwm_window_t *vwin);
static void vwm_keypressed(Window win, XEvent *keypress);

/* animated vwm logo done with simple XOR'd lines, a display of the WM being started and ready */
typedef struct _vwm_point_t {
	float	x, y;
} vwm_point_t;

vwm_point_t	vwm_logo[] = {	{0.0, 0.0},
				{0.170731, 1.0},
				{0.341463, 0.0},
				{0.463414, 1.0},
				{0.536585, 0.6},
				{0.609756, 1.0},
				{0.731707, 0.0},
				{0.804878, 0.4},
				{0.878048, 0.0},
				{1.0, 1.0} };

static void vwm_draw_logo(void)
{
	Window		root;
	int		x, y, i;
	unsigned int	width, height, border_width, depth, yoff, xoff;
	XPoint		points[sizeof(vwm_logo) / sizeof(vwm_point_t)];

	XGrabServer(display);

	/* learn the dimensions of the root window */
	XGetGeometry(display, RootWindow(display, screen_num), &root, &x, &y, &width, &height, &border_width, &depth);

	yoff = ((float)height * .375);
	xoff = 0;
	height /= 4;

	/* the logo gets shrunken vertically until it's essentially a flat line */
	while(height--) {
		/* scale and center the points to the screen size */
		for(i = 0; i < sizeof(points) / sizeof(XPoint); i++) {
			points[i].x = xoff + (vwm_logo[i].x * (float)width);
			points[i].y = (vwm_logo[i].y * (float)height) + yoff;
		}

		XDrawLines(display, RootWindow(display, screen_num), gc, points, sizeof(points) / sizeof(XPoint), CoordModeOrigin);
		XFlush(display);
		usleep(3333);
		XDrawLines(display, RootWindow(display, screen_num), gc, points, sizeof(points) / sizeof(XPoint), CoordModeOrigin);

		/* the width is shrunken as well, but only by as much as it is tall */
		yoff++;
		width -= 4;
		xoff += 2;
	}

	XUngrabServer(display);
}


/* XXX: for now just double forks to avoid having to collect return statuses of the long-running process */
static void vwm_launch(char **argv)
{
	if(!fork()) {
		if(!fork()) {
			/* child */
			execvp(argv[0], argv);
		}
		exit(0);
	}
	wait(NULL);
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
			XUnmapWindow(display, focused_shelf->window);
			XFlush(display); /* for a more responsive feel */

			/* map the focused desktop */
			list_for_each_entry(vwin, &windows, windows) {
				if(vwin->desktop == focused_desktop && !vwin->shelved) {
					VWM_TRACE("Mapping desktop window \"%s\"", vwin->name);
					XMapWindow(display, vwin->window);
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
						XUnmapWindow(display, vwin->window);
					}
				}

				XFlush(display); /* for a more responsive feel */

				VWM_TRACE("Mapping shelf window \"%s\"", focused_shelf->name);
				XMapWindow(display, focused_shelf->window);
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
	list_move(&d->desktops_mru, &desktops_mru);
}


/* focus a virtual desktop */
/* this switches to the desktop context if necessary, maps and unmaps windows accordingly if necessary */
static int vwm_desktop_focus(vwm_desktop_t *d)
{
	XGrabServer(display);
	XSync(display, False);

	/* if the context switched and the focused desktop is the desired desktop there's nothing else to do */
	if( (vwm_context_focus(VWM_CONTEXT_FOCUS_DESKTOP) && focused_desktop != d) || focused_desktop != d) {
		vwm_window_t	*w;

		/* unmap the windows on the currently focused desktop, map those on the newly focused one */
		list_for_each_entry(w, &windows, windows) {
			if(w->shelved) continue;

			if(w->desktop == focused_desktop) {
				VWM_TRACE("Unmapping \"%s\"", w->name);
				XUnmapWindow(display, w->window);
			}
		}

		XFlush(display);

		list_for_each_entry(w, &windows, windows) {
			if(w->shelved) continue;

			if(w->desktop == d) {
				VWM_TRACE("Mapping \"%s\"", w->name);
				XMapWindow(display, w->window);
			}
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

	/* focus another desktop if we're the focused desktop */
	if(d == focused_desktop) {
		/* we just look for one that is different from ours, relative to ours, looking at the next one then the previous */
		if(d->desktops.next != &desktops) {
			vwm_desktop_focus(list_entry(d->desktops.next, vwm_desktop_t, desktops));
		} else if(d->desktops.prev != &desktops) {
			vwm_desktop_focus(list_entry(d->desktops.prev, vwm_desktop_t, desktops));
		}
	}

	list_del(&d->desktops);
	list_del(&d->desktops_mru);
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

/* fullscreen a window with borders obscured "allscreen" */
static void vwm_win_allscreen(vwm_window_t *vwin)
{
	Window		root;
	int		x, y;
	unsigned int	width, height, border_width, depth;

	if(vwin->fullscreened == 2) return;

	XGetGeometry(display, RootWindow(display, screen_num), &root, &x, &y, &width, &height, &border_width, &depth);
	XGetGeometry(display, vwin->window, &root, &vwin->client.x, &vwin->client.y, &vwin->client.width, &vwin->client.height, &border_width, &depth);
	XMoveResizeWindow(display, vwin->window, -border_width, -border_width, width, height);
	vwin->fullscreened = 2;
}

/* fullscreen a window */
static void vwm_win_fullscreen(vwm_window_t *vwin)
{
	Window		root;
	int		x, y;
	unsigned int	width, height, border_width, depth;

	if(vwin->fullscreened == 1) return;

	XGetGeometry(display, RootWindow(display, screen_num), &root, &x, &y, &width, &height, &border_width, &depth);
	/* TODO: do not remember the current geometry if we're going from allscreen to fullscreen! */
	XGetGeometry(display, vwin->window, &root, &vwin->client.x, &vwin->client.y, &vwin->client.width, &vwin->client.height, &border_width, &depth);
	XMoveResizeWindow(display, vwin->window, 0, 0, width - (border_width * 2), height - (border_width * 2));
	vwin->fullscreened = 1;
}


/* restore a window to its non-fullscreened size and coordinates */
static void vwm_win_restore(vwm_window_t *vwin)
{
	if(!vwin->fullscreened) return;

	XMoveResizeWindow(display, vwin->window, vwin->client.x, vwin->client.y, vwin->client.width, vwin->client.height);
	vwin->fullscreened = 0;
}


/* focus a window */
/* this updates window borders as needed and the X input focus */
static void vwm_win_focus(vwm_window_t *vwin)
{
	VWM_TRACE("focusing: %#x", (unsigned int)vwin->window);

	/* change the focus to the new window */
	XSetInputFocus(display, vwin->window, RevertToPointerRoot, CurrentTime);

	/* update the border color accordingly */
	if(vwin->shelved) {
		/* set the border of the newly focused window to the shelved color */
		XSetWindowBorder(display, vwin->window, shelved_window_border_color.pixel);
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


/* focus the next window on a virtual desktop relative to the supplied window */
static vwm_window_t * vwm_win_focus_next(vwm_window_t *vwin, vwm_context_focus_t context)
{
	vwm_window_t	*next;

	list_for_each_entry(next, &vwin->windows, windows) {
		/* searching for the next mapped window in this context, using vwin->windows as the head */
		if(&next->windows == &windows) continue;	/* XXX: skip the containerless head, we're leveraging the circular list implementation */

		if(next->mapped && ((context == VWM_CONTEXT_FOCUS_SHELF && next->shelved) || (context == VWM_CONTEXT_FOCUS_DESKTOP && !next->shelved && next->desktop == focused_desktop))) break;
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
				VWM_TRACE("Unmapping window \"%s\"", focused_shelf->name);
				XUnmapWindow(display, focused_shelf->window);

				XFlush(display);

				VWM_TRACE("Mapping window \"%s\"", next->name);
				XMapWindow(display, next->window);
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
		vwm_win_mru(vwm_win_focus_next(vwin, VWM_CONTEXT_FOCUS_DESKTOP));
	}

	if(vwin == vwin->desktop->focused_window) {
		/* TODO: we can probably put this into vwm_win_focus_next() and have it always handled there... */
		vwin->desktop->focused_window = NULL;
	}

	vwin->shelved = 1;
	vwm_win_mru(vwin);

	/* newly shelved windows always become the focused shelf */
	focused_shelf = vwin;

	VWM_TRACE("Unmapping \"%s\"", vwin->name);
	XUnmapWindow(display, vwin->window);
}


typedef enum _vwm_grab_mode_t {
	VWM_NOT_GRABBED = 0,
	VWM_GRABBED
} vwm_grab_mode_t;

/* manages a new window (called in response to CreateNotify events, and during startup to manage existing windows) */
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
	if(attrs.override_redirect || attrs.class == InputOnly) goto _fail_grabbed;

	VWM_TRACE("Managing %#x", (unsigned int)win);

	/* if no desktops exist, implicitly create one */
	if(!focused_desktop) {
		/* TODO: interactively name desktops */
		vwm_desktop_focus(vwm_desktop_create(NULL));
		vwm_desktop_mru(focused_desktop);
	}

	/* allocate and initialize our per-managed-window structure and get it on the global windows list */
	vwin = (vwm_window_t *)malloc(sizeof(vwm_window_t));
	if(vwin == NULL) {
		VWM_PERROR("Failed to allocate vwin");
		goto _fail_grabbed;
	}

        XUngrabButton(display, AnyButton, AnyModifier, win);
	XGrabButton(display, AnyButton, Mod1Mask, win, False, (PointerMotionMask | ButtonPressMask | ButtonReleaseMask), GrabModeAsync, GrabModeAsync, None, None);
	XGrabKey(display, AnyKey, Mod1Mask, win, False, GrabModeAsync, GrabModeAsync);
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
	vwin->fullscreened = 0;
	vwin->shelved = (focused_context == VWM_CONTEXT_FOCUS_SHELF);	/* if we're in the shelf when the window is created, the window is shelved */
	vwin->mapped = (attrs.map_state != IsUnmapped);

	/* remember whatever the current attributes are */
	vwin->client.x = attrs.x;
	vwin->client.y = attrs.y;
	vwin->client.width = attrs.width;
	vwin->client.height = attrs.height;

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

	/* if the desktop has no focused window yet, automatically focus the newly managed one */
	/* TODO: how does this interact with the shelf? */
	if(!focused_desktop->focused_window) {
		VWM_TRACE("Mapped window \"%s\" is alone on desktop \"%s\", focusing", vwin->name, focused_desktop->name);
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
	/* TODO: what if it's not mapped but goes away???  we don't want to try take the InputFocus it when not mapped... XXX TODO */
	if(vwin == focused_shelf) {
		VWM_TRACE("unfocusing focused shelf");
		vwm_win_focus_next(vwin, VWM_CONTEXT_FOCUS_SHELF);

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
		vwm_win_focus_next(vwin, VWM_CONTEXT_FOCUS_DESKTOP);
	}

	if(vwin->desktop->focused_window == vwin) {
		VWM_TRACE("desktop empty");
		vwin->desktop->focused_window = NULL;
	}
}


/* migrate a window to a new desktop, focuses the destionation desktop as well */
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
	vwm_win_mru(vwin);

	/* ensure the window is raised */
	XRaiseWindow(display, vwin->window);
}


/* unmanages a managed window (called in response to DestroyNotify) */
static void vwm_win_unmanage(Window win)
{
	vwm_window_t		*vwin;

	VWM_TRACE("unmanaging %x", (unsigned int)win);

	XGrabServer(display);
	XSync(display, False);

	/* find the window */
	vwin = vwm_win_lookup(win);

	if(!vwin) {
		VWM_TRACE("window %x not managed", (unsigned int)win);
		goto _ungrab;
	}

	/* unfocus the window */
	vwm_win_unfocus(vwin);

	/* remove the window from the global windows list */
	list_del(&vwin->windows);

	if(vwin->name) XFree(vwin->name);

	free(vwin);

_ungrab:
	XUngrabServer(display);
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
		if (children[i] == None) continue;

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
		if(attrs->x != new->x) {
			new->x -= (min_width - new->width);
		}

		new->width = min_width;
	}

	if(new->height < min_height) {
		if(attrs->y != new->y) {
			new->y -= (min_height - new->height);
		}

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
	vwm_window_t		*vwin;

	/* verify the window still exists */
	if(!XGetWindowAttributes(display, win, &orig)) goto _fail;

	if(!(vwin = vwm_win_lookup(win))) goto _fail;

	if(impetus->xbutton.state & Mod1Mask) {
		int			finished = 0;
		vwm_adjust_mode_t	mode;

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

			XNextEvent(display, &event);
			switch(event.type) {
				case ButtonRelease:
					switch(mode) {
						case VWM_ADJUST_MOVE:
							XMoveWindow(display, win,
									orig.x + (event.xbutton.x_root - impetus->xbutton.x_root),
									orig.y + (event.xbutton.y_root - impetus->xbutton.y_root));
							break;

						case VWM_ADJUST_RESIZE:
							compute_resize(vwin, &orig, impetus, &event, &resized);
							/* move and resize the window @ resized */
							XDrawRectangle(display, RootWindow(display, screen_num), gc, lastrect.x, lastrect.y, lastrect.width, lastrect.height);
							XMoveResizeWindow(display, win, resized.x, resized.y, resized.width, resized.height);
							XUngrabServer(display);

							break;

						default:
							break;
					}
					/* once you manipulate the window it's no longer fullscreened, simply hitting Mod1+Return once will restore fullscreened mode */
					vwin->fullscreened = 0;
					finished = 1;
					break;

				case MotionNotify:
					switch(mode) {
						case VWM_ADJUST_MOVE:
							XMoveWindow(display, win,
									orig.x + (event.xmotion.x_root - impetus->xbutton.x_root),
									orig.y + (event.xmotion.y_root - impetus->xbutton.y_root));
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

	/* always set the input focus to the clicked window, note if we allow this to happen on the root window, it enters sloppy focus mode
	 * until a non-root window is clicked, which is an interesting hybrid but not how I prefer it. */
	if(vwin != focused_desktop->focused_window && vwin->window != RootWindow(display, screen_num)) {
		vwm_win_focus(vwin);
		vwm_win_mru(vwin);
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
			if((vwin = vwm_win_focused())) {
				vwm_win_mru(vwin);
			}

			/* make the focused desktop the most recently used */
			if(focused_context == VWM_CONTEXT_FOCUS_DESKTOP && focused_desktop) {
				vwm_desktop_mru(focused_desktop);
			}

			if(key_is_grabbed) {
				XUngrabKeyboard(display, CurrentTime);
				XFlush(display);
				key_is_grabbed = 0;
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
	vwm_window_t	*vwin;
	KeySym		sym;
	static KeySym	last_sym;
	static int	repeat_cnt = 0;
	int		do_grab = 0;

	sym = XLookupKeysym(&keypress->xkey, 0);

	/* detect repeaters, note repeaters do not span interrupted Mod1 sequences! */
	if(key_is_grabbed && sym == last_sym) {
		repeat_cnt++;
	} else {
		repeat_cnt = 0;
	}

	switch(sym) {

#define launcher(_sym, _argv...)\
		case _sym:	\
			{	\
			char	*args[] = {_argv, NULL};\
			vwm_launch(args);\
			break;	\
		}
#include "launchers.def"
#undef launcher

		case XK_grave: /* toggle shelf visibility */
			vwm_context_focus(VWM_CONTEXT_FOCUS_OTHER);
			break;

		case XK_Tab: /* cycle focused window */
			do_grab = 1; /* grab the keyboard so we can respond to the Mod1 release */

			/* focus the next window, note this doesn't affect MRU, that happens on Mod1 release */
			if((vwin = vwm_win_focused())) {
				vwm_win_focus_next(vwin, focused_context);
			}
			break;

		case XK_space: /* cycle focused desktop utilizing MRU */
			do_grab = 1; /* grab the keyboard so we can respond to the Mod1 release for MRU updating */

			/* XXX: note the sensitivity to the desktops_mru head here, we want to look past it. */
			vwm_desktop_focus(list_entry(focused_desktop->desktops_mru.next == &desktops_mru ? focused_desktop->desktops_mru.next->next : focused_desktop->desktops_mru.next, vwm_desktop_t, desktops_mru));
			break;

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

		case XK_Escape: /* leave VWM rudely */
			exit(42);

		case XK_v: /* instantiate (and focus) a new (potentially empty, unless migrating) virtual desktop */
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
			do_grab = 1; /* grab the keyboard so we can respond to the Mod1 release for MRU updating */

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
			do_grab = 1; /* grab the keyboard so we can respond to the Mod1 release for MRU updating */

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
						vwm_win_fullscreen(vwin);
					} else if(repeat_cnt == 2) {
						 /* triple: reraise & fullscreen w/borders obscured by screen perimiter */
						vwm_win_allscreen(vwin);
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
					if(vwin->fullscreened == 2) {
						vwm_win_fullscreen(vwin);
					} else {
						XLowerWindow(display, vwin->window);
					}
					XFlush(display);
				}
			}
			break;

		case XK_Return: /* (full-screen / restore) focused window */
			if((vwin = vwm_win_focused())) {
				if(vwin->fullscreened) {
					vwm_win_restore(vwin);
				} else {
					vwm_win_fullscreen(vwin);
				}
			}
			break;

		case XK_s: /* shelve focused window */
			if((vwin = vwm_win_focused()) && !vwin->shelved) {
				vwm_win_shelve(vwin);
			}
			break;

		default:
			VWM_TRACE("Unhandled keycode: %x", (unsigned int)sym);
			break;
	}

	/* if what we're doing requests a grab if not already grabbed, grab keyboard */
	if(!key_is_grabbed && do_grab) {
		XGrabKeyboard(display, RootWindow(display, screen_num), False, GrabModeAsync, GrabModeAsync, CurrentTime);
		key_is_grabbed = 1;
	}

	/* remember the symbol for repeater detection */
	last_sym = sym;
}


static int errhandler(Display *display, XErrorEvent *err)
{
	/* TODO */
	return 1;
}
 
int main(int argc, char *argv[])
{
	int			done = 0;
	XEvent			event;
	Cursor			pointer;

	/* open connection with the server */
	if((display = XOpenDisplay(NULL)) == NULL) {
		VWM_ERROR("Cannot open display");
		return 1;
	}

	/* prevent children from inheriting the X connection */
	if(fcntl(ConnectionNumber(display), F_SETFD, FD_CLOEXEC) < 0) {
		VWM_ERROR("Cannot set FD_CLOEXEC on X connection");
		return 2;
	}

	XSetErrorHandler(errhandler);

	screen_num = DefaultScreen(display);

	/* allocate colors, I make assumptions about the X server's color capabilities since I'll only use this on modern-ish computers... */
	cmap = DefaultColormap(display, screen_num);

#define color(_sym, _str) \
	XAllocNamedColor(display, cmap, _str, &_sym ## _color, &_sym ## _color);
#include "colors.def"
#undef color

	wm_delete_atom = XInternAtom(display, "WM_DELETE_WINDOW", False);
	wm_protocols_atom = XInternAtom(display, "WM_PROTOCOLS", False);

	XSelectInput(display, RootWindow(display, screen_num), SubstructureNotifyMask | SubstructureRedirectMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
	XGrabKey(display, AnyKey, Mod1Mask, RootWindow(display, screen_num), False, GrabModeAsync, GrabModeAsync);

	XFlush(display);

	XSetInputFocus(display, RootWindow(display, screen_num), RevertToPointerRoot, CurrentTime);

	/* manage all preexisting windows */
	vwm_manage_existing();

	/* create GC for logo drawing and window rubber-banding */
	gc = XCreateGC(display, RootWindow(display, screen_num), 0, NULL);
	XSetSubwindowMode(display, gc, IncludeInferiors);
	XSetFunction(display, gc, GXxor);

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
	while (!done) {
		XNextEvent(display, &event);
		switch (event.type) {
			case Expose:
				VWM_TRACE("expose");
				break;

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

			case ButtonRelease:
				VWM_TRACE("buttonrelease");
				break;

			case DestroyNotify:
				VWM_TRACE("destroynotify");
				vwm_win_unmanage(event.xdestroywindow.window);
				break;

			case CirculateNotify:
				VWM_TRACE("circulatenotify");
				break;

			case ConfigureNotify:
				VWM_TRACE("configurenotify");
				break;

			case UnmapNotify:
				VWM_TRACE("unmapnotify");
				break;

			case CreateNotify:
				VWM_TRACE("createnotify");
				vwm_win_manage(event.xcreatewindow.window, VWM_NOT_GRABBED);
				break;

			case GravityNotify:
				VWM_TRACE("gravitynotify");
				break;

			case MapNotify:
				VWM_TRACE("mapnotify");
				break;

			case ReparentNotify:
				VWM_TRACE("reparentnotify");
				break;

			case MotionNotify:
				VWM_TRACE("motionnotify");
				break;

			case ConfigureRequest: {
				XWindowChanges	changes = {
							.x = event.xconfigurerequest.x,		/* TODO: for now I don't manipulate anything */
							.y = event.xconfigurerequest.y,
							.width = event.xconfigurerequest.width,
							.height = event.xconfigurerequest.height,
							.border_width = WINDOW_BORDER_WIDTH /* except I do override whatever the border width may be */
						};

				VWM_TRACE("configurerequest x=%i y=%i w=%i h=%i", changes.x, changes.y, changes.width, changes.height);
				XConfigureWindow(display, event.xconfigurerequest.window, (event.xconfigurerequest.value_mask | CWBorderWidth), &changes);
				break;
			}

			case MapRequest: {
				vwm_window_t	*vwin;

				VWM_TRACE("maprequest");

				vwin = vwm_win_lookup(event.xmap.window);
				if(vwin && !vwin->mapped) {
					int			x = 0, y = 0;
					XWindowAttributes	attrs;
					XWindowChanges		changes;

					/* on initial mapping of a window we discover the coordinates and dimensions of the window */
					changes.x = x;
					changes.y = y;

					vwin->mapped = 1;
					XGetWMNormalHints(display, event.xmap.window, vwin->hints, &vwin->hints_supplied);
					XGetWindowAttributes(display, event.xmap.window, &attrs);

					/* TODO: this is a good place to hook in a window placement algo */
					vwin->client.x = x;
					vwin->client.y = y;

					vwin->client.height = attrs.height;
					vwin->client.width = attrs.width;

					XConfigureWindow(display, event.xmap.window, (CWX | CWY), &changes);
				}
				
				XMapWindow(display, event.xmap.window);
				if(vwin->desktop->focused_window == vwin) {
					XSync(display, False);
					XSetInputFocus(display, vwin->window, RevertToPointerRoot, CurrentTime);
				}
				break;
			}

			case MappingNotify:
				VWM_TRACE("mapping notify");
				XRefreshKeyboardMapping(&event.xmapping);
				break;

			default:
				VWM_ERROR("Unhandled X op %i", event.type);
				break;
		}
	}

	/* close connection to server */
	XFlush(display);
	XCloseDisplay(display);
 
	return 0;
}
