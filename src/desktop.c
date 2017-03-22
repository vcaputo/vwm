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
	/* virtual desktops */

#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "context.h"
#include "desktop.h"
#include "vwm.h"
#include "xwindow.h"

/* make the specified desktop the most recently used one */
void vwm_desktop_mru(vwm_t *vwm, vwm_desktop_t *desktop)
{
	VWM_TRACE("MRU desktop: %p", desktop);
	list_move(&desktop->desktops_mru, &vwm->desktops_mru);
}


/* focus a virtual desktop */
/* this switches to the desktop context if necessary, maps and unmaps windows accordingly if necessary */
int vwm_desktop_focus(vwm_t *vwm, vwm_desktop_t *desktop)
{
	XGrabServer(VWM_XDISPLAY(vwm));
	XSync(VWM_XDISPLAY(vwm), False);

	/* if the context switched and the focused desktop is the desired desktop there's nothing else to do */
	if ((vwm_context_focus(vwm, VWM_CONTEXT_DESKTOP) && vwm->focused_desktop != desktop) || vwm->focused_desktop != desktop) {
		vwm_xwindow_t	*xwin;
		vwm_window_t	*vwin;

		/* unmap the windows on the currently focused desktop, map those on the newly focused one */
		list_for_each_entry(xwin, &vwm->xwindows, xwindows) {
			if (!(vwin = xwin->managed) || vwin->shelved)
				continue;

			if (vwin->desktop == vwm->focused_desktop)
				vwm_win_unmap(vwm, vwin);
		}

		XFlush(VWM_XDISPLAY(vwm));

		list_for_each_entry_prev(xwin, &vwm->xwindows, xwindows) {
			if (!(vwin = xwin->managed) || vwin->shelved)
				continue;

			if (vwin->desktop == desktop)
				vwm_win_map(vwm, vwin);
		}

		vwm->focused_desktop = desktop;
	}

	/* directly focus the desktop's focused window if there is one, we don't use vwm_win_focus() intentionally XXX */
	if (vwm->focused_desktop->focused_window) {
		VWM_TRACE("Focusing \"%s\"", vwm->focused_desktop->focused_window->xwindow->name);
		XSetInputFocus(VWM_XDISPLAY(vwm), vwm->focused_desktop->focused_window->xwindow->id, RevertToPointerRoot, CurrentTime);
	}

	XUngrabServer(VWM_XDISPLAY(vwm));

	return 1;
}

/* return next MRU desktop relative to the supplied desktop */
vwm_desktop_t * vwm_desktop_next_mru(vwm_t *vwm, vwm_desktop_t *desktop)
{
	list_head_t	*next;

	/* this dance is necessary because the list head @ vwm->desktops_mru has no vwm_desktop_t container,
	 * and we're exploiting the circular nature of the doubly linked lists, so we need to take care to skip
	 * past the container-less head.
	 */
	if (desktop->desktops_mru.next == &vwm->desktops_mru) {
		next = desktop->desktops_mru.next->next;
	} else {
		next = desktop->desktops_mru.next;
	}

	return list_entry(next, vwm_desktop_t, desktops_mru);
}

/* return next desktop spatially relative to the supplied desktop, no wrap-around */
vwm_desktop_t * vwm_desktop_next(vwm_t *vwm, vwm_desktop_t *desktop)
{
	if (desktop->desktops.next != &vwm->desktops)
		desktop = list_entry(desktop->desktops.next, vwm_desktop_t, desktops);

	return desktop;
}


/* return previous desktop spatially relative to the supplied desktop, no wrap-around */
vwm_desktop_t * vwm_desktop_prev(vwm_t *vwm, vwm_desktop_t *desktop)
{
	if (desktop->desktops.prev != &vwm->desktops)
		desktop = list_entry(desktop->desktops.prev, vwm_desktop_t, desktops);

	return desktop;
}

/* create a virtual desktop */
vwm_desktop_t * vwm_desktop_create(vwm_t *vwm, char *name)
{
	vwm_desktop_t	*desktop;

	desktop = malloc(sizeof(vwm_desktop_t));
	if (desktop == NULL) {
		VWM_PERROR("Failed to allocate desktop");
		goto _fail;
	}

	desktop->name = name == NULL ? name : strdup(name);
	desktop->focused_window = NULL;

	list_add_tail(&desktop->desktops, &vwm->desktops);
	list_add_tail(&desktop->desktops_mru, &vwm->desktops_mru);

	return desktop;

_fail:
	return NULL;
}


/* destroy a virtual desktop */
void vwm_desktop_destroy(vwm_t *vwm, vwm_desktop_t *desktop)
{
	/* silently refuse to destroy a desktop having windows (for now) */
	/* there's _always_ a focused window on a desktop having mapped windows */
	/* also silently refuse to destroy the last desktop (for now) */
	if (desktop->focused_window || (desktop->desktops.next == desktop->desktops.prev))
		return;

	/* focus the MRU desktop that isn't this one if we're the focused desktop */
	if (desktop == vwm->focused_desktop) {
		vwm_desktop_t	*next_desktop;

		list_for_each_entry(next_desktop, &vwm->desktops_mru, desktops_mru) {
			if (next_desktop != desktop) {
				vwm_desktop_focus(vwm, next_desktop);
				break;
			}
		}
	}

	list_del(&desktop->desktops);
	list_del(&desktop->desktops_mru);
}
