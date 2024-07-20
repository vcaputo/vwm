/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2018  Vito Caputo - <vcaputo@pengaru.com>
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
	/* virtual desktops */

#include <assert.h>
#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "context.h"
#include "desktop.h"
#include "vwm.h"
#include "xwindow.h"

/* make the specified desktop the most recently used one */
vwm_desktop_t * vwm_desktop_mru(vwm_t *vwm, vwm_desktop_t *desktop)
{
	VWM_TRACE("MRU desktop: %p", desktop);
	list_move(&desktop->desktops_mru, &vwm->desktops_mru);
	vwm_context_mru(vwm, desktop->context);

	return desktop;
}


/* focus a virtual desktop */
/* this updates the focused context if necessary, maps and unmaps windows accordingly if necessary */
int vwm_desktop_focus(vwm_t *vwm, vwm_desktop_t *desktop)
{
	XGrabServer(VWM_XDISPLAY(vwm));
	XSync(VWM_XDISPLAY(vwm), False);

	if (vwm->focused_desktop != desktop) {
		vwm_xwindow_t	*xwin;
		vwm_window_t	*vwin;

		/* unmap the windows on the currently focused desktop, map those on the newly focused one */
		list_for_each_entry(xwin, &vwm->xwindows, xwindows) {
			vwin = xwin->managed;
			if (!vwin)
				continue;

			if (vwin->desktop == vwm->focused_desktop)
				vwm_win_unmap(vwm, vwin);
		}

		XFlush(VWM_XDISPLAY(vwm));

		list_for_each_entry_prev(xwin, &vwm->xwindows, xwindows) {
			vwin = xwin->managed;
			if (!vwin)
				continue;

			if (vwin->desktop == desktop)
				vwm_win_map(vwm, vwin);
		}

		vwm->focused_desktop = desktop;
		desktop->context->focused_desktop = desktop;
	}

	/* directly focus the desktop's focused window if there is one, we don't use vwm_win_focus() intentionally XXX */
	if (vwm->focused_desktop->focused_window) {
		VWM_TRACE("Focusing \"%s\"", vwm->focused_desktop->focused_window->xwindow->name);
		XSetInputFocus(VWM_XDISPLAY(vwm), vwm->focused_desktop->focused_window->xwindow->id, RevertToPointerRoot, CurrentTime);
	}

	XUngrabServer(VWM_XDISPLAY(vwm));

	return 1;
}


/* return next MRU desktop within the same context relative to the supplied desktop, wraps-around */
vwm_desktop_t * vwm_desktop_next_mru(vwm_t *vwm, vwm_desktop_t *desktop, vwm_direction_t direction)
{
	vwm_desktop_t	*next = desktop;

	do {
		/* this dance is necessary because the list head @ vwm->desktops_mru has no vwm_desktop_t container,
		 * and we're exploiting the circular nature of the doubly linked lists, so we need to take care to skip
		 * past the container-less head.
		 */
		switch (direction) {
		case VWM_DIRECTION_FORWARD:
			if (next->desktops_mru.next == &vwm->desktops_mru) {
				next = list_entry(next->desktops_mru.next->next, vwm_desktop_t, desktops_mru);
			} else {
				next = list_entry(next->desktops_mru.next, vwm_desktop_t, desktops_mru);
			}
			break;

		case VWM_DIRECTION_REVERSE:
			if (next->desktops_mru.prev == &vwm->desktops_mru) {
				next = list_entry(next->desktops_mru.prev->prev, vwm_desktop_t, desktops_mru);
			} else {
				next = list_entry(next->desktops_mru.prev, vwm_desktop_t, desktops_mru);
			}
			break;

		default:
			assert(0);
		}
	} while (next->context != desktop->context);

	return next;
}


/* return next in-context desktop spatially relative to the supplied desktop, no wrap-around */
vwm_desktop_t * vwm_desktop_next(vwm_t *vwm, vwm_desktop_t *desktop, vwm_direction_t direction)
{
	switch (direction) {
	case VWM_DIRECTION_FORWARD: {
		vwm_desktop_t	*next = desktop;

		while (next->desktops.next != &vwm->desktops) {
			next = list_entry(next->desktops.next, vwm_desktop_t, desktops);
			if (next->context == desktop->context)
				return next;
		}
		break;
	}

	case VWM_DIRECTION_REVERSE: {
		vwm_desktop_t	*next = desktop;

		while (next->desktops.prev != &vwm->desktops) {
			next = list_entry(next->desktops.prev, vwm_desktop_t, desktops);
			if (next->context == desktop->context)
				return next;
		}
		break;
	}

	default:
		assert(0);
	}

	return desktop;
}


/* TODO: when "sending" windows to contexts, I currently always create a
 * new desktop with this function.  There should really be a "create if needed"
 * variant which just returns an empty desktop in the target context, creating
 * only if no empty one already exists, for "sending" purposes.  The current
 * approach tends to litter empty desktops unnecessarily.
 */
/* create a virtual desktop on the supplied context,
 * if context is NULL a new one is created
 * the desktop becomes the context's focused desktop if there isn't already one
 */
vwm_desktop_t * vwm_desktop_create(vwm_t *vwm, vwm_context_t *context)
{
	vwm_desktop_t	*desktop;

	desktop = calloc(1, sizeof(vwm_desktop_t));
	if (desktop == NULL) {
		VWM_PERROR("Failed to allocate desktop");
		return NULL;
	}

	if (!context)
		context = vwm_context_create(vwm, -1, desktop);

	if (!context->focused_desktop)
		context->focused_desktop = desktop;

	desktop->context = context;

	list_add_tail(&desktop->desktops, &vwm->desktops);
	list_add_tail(&desktop->desktops_mru, &vwm->desktops_mru);

	return desktop;
}


/* destroy a virtual desktop */
void vwm_desktop_destroy(vwm_t *vwm, vwm_desktop_t *desktop)
{
	/* silently refuse to destroy a desktop having windows (for now) */
	/* there's _always_ a focused window on a desktop having mapped windows */
	/* also silently refuse to destroy the last desktop (for now) */
	if (desktop->focused_window || (desktop->desktops.next == desktop->desktops.prev))
		return;

	/* focus the desktop context's MRU desktop that isn't this one,
	 * if desktop is the context's focused desktop */
	if (desktop == desktop->context->focused_desktop) {
		vwm_desktop_t	*next_desktop;

		list_for_each_entry(next_desktop, &vwm->desktops_mru, desktops_mru) {
			if (next_desktop != desktop && next_desktop->context == desktop->context) {
				desktop->context->focused_desktop = next_desktop;
				break;
			}
		}

		/* if *still* the context's focused desktop, the context is finished.
		 * find a desktop from the next MRU context to focus if this desktop
		 * was the vwm focused one before destroying the context
		 */
		if (desktop == desktop->context->focused_desktop) {
			if (desktop == vwm->focused_desktop)
				next_desktop = vwm_context_next_mru(vwm, desktop->context, VWM_DIRECTION_FORWARD)->focused_desktop;

			vwm_context_destroy(vwm, desktop->context);
		}

		if (desktop == vwm->focused_desktop)
			vwm_desktop_focus(vwm, next_desktop);
	}

	list_del(&desktop->desktops);
	list_del(&desktop->desktops_mru);
}
