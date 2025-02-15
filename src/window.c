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
	/* vwm "managed" windows (vwm_window_t) (which are built upon the "core" X windows (vwm_xwindow_t)) */

#include <X11/Xlib.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "desktop.h"
#include "list.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

/* unmap the specified window and set the unmapping-in-progress flag so we can discard vwm-generated UnmapNotify events */
void vwm_win_unmap(vwm_t *vwm, vwm_window_t *vwin)
{
	if (!vwin->xwindow->client_mapped) {
		VWM_TRACE("inhibited unmap of \"%s\", not mapped by client", vwin->xwindow->name);
		return;
	}

	VWM_TRACE("Unmapping \"%s\"", vwin->xwindow->name);
	vwin->unmapping = 1;
	XUnmapWindow(VWM_XDISPLAY(vwm), vwin->xwindow->id);
}


/* map the specified window and set the mapping-in-progress flag so we can discard vwm-generated MapNotify events */
void vwm_win_map(vwm_t *vwm, vwm_window_t *vwin)
{
	if (!vwin->xwindow->client_mapped) {
		VWM_TRACE("inhibited map of \"%s\", not mapped by client", vwin->xwindow->name);
		return;
	}

	VWM_TRACE("Mapping \"%s\"", vwin->xwindow->name);
	vwin->mapping = 1;
	XMapWindow(VWM_XDISPLAY(vwm), vwin->xwindow->id);
}


/* make the specified window the most recently used one */
vwm_window_t * vwm_win_mru(vwm_t *vwm, vwm_window_t *vwin)
{
	list_move(&vwin->windows_mru, &vwm->windows_mru);

	return vwin;
}


/* look up the X window in the global managed windows list */
vwm_window_t * vwm_win_lookup(vwm_t *vwm, Window win)
{
	vwm_window_t	*tmp;

	list_for_each_entry(tmp, &vwm->windows_mru, windows_mru) {
		if (tmp->xwindow->id == win)
			return tmp;
	}

	return NULL;
}


/* return the currently focused window, may return NULL */
vwm_window_t * vwm_win_get_focused(const vwm_t *vwm)
{
	return vwm->focused_desktop->focused_window;
}


/* set the currently focused window */
/* this sets our internal state of which window is focused, and updates window border color as needed */
/* note X input focus is not affected by this. */
void vwm_win_set_focused(vwm_t *vwm, vwm_window_t *vwin)
{
	/* update the border color accordingly */
	if (vwin->desktop->focused_window)
		/* set the border of the previously focused window on the same desktop to the unfocused color */
		XSetWindowBorder(VWM_XDISPLAY(vwm), vwin->desktop->focused_window->xwindow->id, vwm->colors.unfocused_window_border_color.pixel);

	/* set the border of the newly focused window to the focused color */
	XSetWindowBorder(VWM_XDISPLAY(vwm), vwin->xwindow->id, vwm->context_colors[vwin->desktop->context->color].pixel);

	/* persist this on a per-desktop basis so it can be restored on desktop switches */
	vwin->desktop->focused_window = vwin;
}


/* Using the supplied rect and screen dimensions, discover "allscreen" windows. */
/* If scr is NULL, the screen best fit to the supplied rect is used. */
void vwm_win_autoconf_magic(vwm_t *vwm, vwm_window_t *vwin, const vwm_screen_t *scr, int x, int y, int width, int height)
{
	vwin->autoconfigured = VWM_WIN_AUTOCONF_NONE;

	if (!scr)
		scr = vwm_screen_find(vwm, VWM_SCREEN_REL_RECT, x, y, width, height);

	if (scr &&
	    width == scr->width &&
	    height == scr->height) {
		VWM_TRACE_WIN(vwin->xwindow->id, "auto-allscreened window");
		vwin->autoconfigured = VWM_WIN_AUTOCONF_ALL;
	}
}


/* "autoconfigure" windows (configuration shortcuts like fullscreen/halfscreen/quarterscreen) and restoring the window */
void vwm_win_autoconf(vwm_t *vwm, vwm_window_t *vwin, vwm_screen_rel_t rel, vwm_win_autoconf_t conf, ...)
{
	XWindowChanges		changes = { .border_width = WINDOW_BORDER_WIDTH };
	const vwm_screen_t	*scr;
	va_list			ap;

	/* remember the current configuration as the "client" configuration if it's not an autoconfigured one. */
	/* XXX: this isn't perfect, and makes me wonder if there should be two slots you can restore back to:
	 * 1. the OG window configuration from the X client, or
	 * 2. the last non-autoconf configuration, which may or may not reflect the OG one
	 * Right now, the OG one gets overwritten by #2, so you can only restore back to the last
	 * explicit window configuration with Mod1-Enter.
	 */
	if (conf != VWM_WIN_AUTOCONF_NONE && vwin->autoconfigured == VWM_WIN_AUTOCONF_NONE)
		vwin->client = vwin->xwindow->attrs;

	scr = vwm_screen_find(vwm, rel, vwin->xwindow); /* XXX FIXME: this becomes a bug when vwm_screen_find() uses non-xwin va_args */
	va_start(ap, conf);
	switch (conf) {
		case VWM_WIN_AUTOCONF_QUARTER: {
			vwm_corner_t corner = va_arg(ap, vwm_corner_t);
			changes.width = scr->width / 2 - (WINDOW_BORDER_WIDTH * 2);
			changes.height = scr->height / 2 - (WINDOW_BORDER_WIDTH * 2);
			switch (corner) {
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
			vwin->autoconfigured_param = corner;
			break;
		}

		case VWM_WIN_AUTOCONF_HALF: {
			vwm_side_t side = va_arg(ap, vwm_side_t);
			switch (side) {
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
			vwin->autoconfigured_param = side;
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
			if (vwin->autoconfigured == VWM_WIN_AUTOCONF_NONE) {
				if (rel != VWM_SCREEN_REL_XWIN) {
					const vwm_screen_t	*from_scr;

					/* For now let's just try adjusting the x/y to scr,
					 * which may or may not change anything at all, but
					 * it /appears/ we might be trying to switch screens.
					 */
					from_scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, vwin->xwindow);
					if (from_scr != scr) {
						changes.x = vwin->xwindow->attrs.x - from_scr->x_org + scr->x_org;
						changes.y = vwin->xwindow->attrs.y - from_scr->y_org + scr->y_org;
						/* XXX: undecided on if w/h should get scaled proportionally, leaving alone for now */
						changes.width = vwin->xwindow->attrs.width;
						changes.height = vwin->xwindow->attrs.height;
					}
				}
			} else {
				changes.width = vwin->client.width;
				changes.height = vwin->client.height;
				changes.x = vwin->client.x;
				changes.y = vwin->client.y;
			}

			break;
	}
	va_end(ap);

	XConfigureWindow(VWM_XDISPLAY(vwm), vwin->xwindow->id, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &changes);
	vwin->autoconfigured = conf;
}


/* focus a window */
void vwm_win_focus(vwm_t *vwm, vwm_window_t *vwin)
{
	VWM_TRACE("focusing: %#x", (unsigned int)vwin->xwindow->id);

	if (vwm_xwin_is_mapped(vwm, vwin->xwindow))	/* note X only allows mapped windows to have input focus */
		XSetInputFocus(VWM_XDISPLAY(vwm), vwin->xwindow->id, RevertToPointerRoot, CurrentTime);

	vwm_win_set_focused(vwm, vwin);
}


/* focus the next window on a virtual desktop relative to the supplied window, in the specified context, respecting screen boundaries according to fence. */
vwm_window_t * vwm_win_focus_next(vwm_t *vwm, vwm_window_t *vwin, vwm_direction_t direction, vwm_fence_t fence)
{
	const vwm_screen_t	*scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, vwin->xwindow), *next_scr = NULL;
	vwm_window_t		*next;
	unsigned long		visited_mask;

_retry:
	visited_mask = 0;

	switch (direction) {
	case VWM_DIRECTION_FORWARD:
		list_for_each_entry(next, &vwin->windows_mru, windows_mru) {
			/* searching for the next mapped window in this context, using vwin->windows as the head */
			if (&next->windows_mru == &vwm->windows_mru)
				continue;	/* XXX: skip the containerless head, we're leveraging the circular list implementation */

			if ( ((next->desktop == vwin->desktop) &&
			     (fence == VWM_FENCE_IGNORE ||
			      ((fence == VWM_FENCE_RESPECT || fence == VWM_FENCE_TRY_RESPECT) && vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, next->xwindow) == scr) ||
			      (fence == VWM_FENCE_VIOLATE && vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, next->xwindow) != scr) ||
			      (fence == VWM_FENCE_MASKED_VIOLATE && (next_scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, next->xwindow)) != scr &&
			       !((1UL << next_scr->screen_number) & vwm->fence_mask))
			   )))
				break;

			if (fence == VWM_FENCE_MASKED_VIOLATE && next_scr && next_scr != scr)
				visited_mask |= (1UL << next_scr->screen_number);
		}
		break;

	case VWM_DIRECTION_REVERSE:
		list_for_each_entry_prev(next, &vwin->windows_mru, windows_mru) {
			/* searching for the next mapped window in this context, using vwin->windows as the head */
			if (&next->windows_mru == &vwm->windows_mru)
				continue;	/* XXX: skip the containerless head, we're leveraging the circular list implementation */

		/* TODO: move most of this into a function shared by both direction cases */
			if ( ((next->desktop == vwin->desktop) &&
			     (fence == VWM_FENCE_IGNORE ||
			      ((fence == VWM_FENCE_RESPECT || fence == VWM_FENCE_TRY_RESPECT) && vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, next->xwindow) == scr) ||
			      (fence == VWM_FENCE_VIOLATE && vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, next->xwindow) != scr) ||
			      (fence == VWM_FENCE_MASKED_VIOLATE && (next_scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, next->xwindow)) != scr &&
			       !((1UL << next_scr->screen_number) & vwm->fence_mask))
			   )))
				break;

			if (fence == VWM_FENCE_MASKED_VIOLATE && next_scr && next_scr != scr)
				visited_mask |= (1UL << next_scr->screen_number);
		}
		break;

	default:
		assert(0);
	}

	if (fence == VWM_FENCE_TRY_RESPECT && next == vwin) {
		/* if we tried to respect the fence and failed to find a next, fallback to ignoring the fence and try again */
		fence = VWM_FENCE_IGNORE;
		goto _retry;
	} else if (fence == VWM_FENCE_MASKED_VIOLATE && next_scr) {
		/* if we did a masked violate update the mask with the potentially new screen number */
		if (next == vwin && visited_mask) {
			/* if we failed to find a next window on a different screen but had candidates we've exhausted screens and need to reset the mask then retry */
			VWM_TRACE("all candidate screens masked @ 0x%lx, resetting mask", vwm->fence_mask);
			vwm->fence_mask = 0;
			goto _retry;
		}
		vwm->fence_mask |= (1UL << next_scr->screen_number);
		VWM_TRACE("VWM_FENCE_MASKED_VIOLATE fence_mask now: 0x%lx\n", vwm->fence_mask);
	}

	if (next != next->desktop->focused_window) {
		vwm_win_focus(vwm, next);
		XRaiseWindow(VWM_XDISPLAY(vwm), next->xwindow->id);
	}

	VWM_TRACE("vwin=%p xwin=%p name=\"%s\"", next, next->xwindow, next->xwindow->name);

	return next;
}


/* "shelves" a window, if the window is focused we focus the next one (if exists) */
/* originally there was a special shelf context having different semantics of a fullscreen
 * window at a time, this evolved into generic contexts containing virtual
 * desktops, and now shelving is just the process of sending a window to the bottom/first
 * context into a newly created desktop there, in an unattended fashion (like an unattended migrate,
 * to an assumed bottom destination context created for this purpose at startup, and into its own
 * desktop there).
 */
void vwm_win_shelve(vwm_t *vwm, vwm_window_t *vwin)
{
	vwm_context_t	*shelf = list_entry(vwm->contexts.next, vwm_context_t, contexts);
	vwm_desktop_t	*desktop;

	/* already in the first, AKA "shelf" context, NOOP */
	if (&vwin->desktop->context->contexts == vwm->contexts.next)
		return;

	/* shelving focused window, focus the next window */
	if (vwin == vwin->desktop->focused_window)
		vwm_win_mru(vwm, vwm_win_focus_next(vwm, vwin, VWM_DIRECTION_FORWARD, VWM_FENCE_RESPECT));

	/* vwin appears to be alone */
	if (vwin == vwin->desktop->focused_window)
		vwin->desktop->focused_window = NULL;

	/* TODO: ^^^ there should probably be a helper for withdrawing a window
	 * from a desktop which handles the above focus next -> lone window
	 * nonsense, and hands back an orphan window to do whatever with.
	 */

	/* shelved windows always get an empty desktop in the shelf context,
	 * look for an empty one and only create a new one if there is none
	 * to use.
	 */
	vwin->desktop = NULL;
	list_for_each_entry(desktop, &vwm->desktops_mru, desktops_mru) {
		if (desktop->context == shelf && !desktop->focused_window) {
			vwin->desktop = desktop;
			break;
		}
	}

	if (!vwin->desktop)
		vwin->desktop = vwm_desktop_create(vwm, shelf);

	/* always leave the newly shelved window's desktop focused */
	vwin->desktop->context->focused_desktop = vwin->desktop;
	vwm_win_set_focused(vwm, vwin);
	vwm_win_mru(vwm, vwin);
	vwm_win_unmap(vwm, vwin);
}


/* helper for (idempotently) unfocusing a window */
void vwm_win_unfocus(vwm_t *vwm, vwm_window_t *vwin)
{
	/* if we're the focused window cycle the focus to the next window on the desktop if possible */
	if (vwin->desktop->focused_window == vwin) {
		VWM_TRACE("unfocusing focused window");
		vwm_win_focus_next(vwm, vwin, VWM_DIRECTION_FORWARD, VWM_FENCE_TRY_RESPECT);
	}

	if (vwin->desktop->focused_window == vwin) {
		VWM_TRACE("unfocused last window on desktop");
		vwin->desktop->focused_window = NULL;
	}
}


/* demote an managed window to an unmanaged one */
vwm_xwindow_t * vwm_win_unmanage(vwm_t *vwm, vwm_window_t *vwin)
{
	vwm_win_mru(vwm, vwin); /* bump vwin to the mru head before unfocusing so we always move focus to the current head on unmanage of the focused window */
	vwm_win_unfocus(vwm, vwin);
	list_del(&vwin->windows_mru);

	if (vwin == vwm->console)
		vwm->console = NULL;

	if (vwin == vwm->focused_origin)
		vwm->focused_origin = NULL;

	vwin->xwindow->managed = NULL;

	return vwin->xwindow;
}


/* helper for determining if a window is the console window */
static int win_is_console(vwm_t *vwm, Window win)
{
	XClassHint	*classhint;
	int		ret = 0;

	/* figure out if the window is the console */
	if ((classhint = XAllocClassHint())) {
		if (XGetClassHint(VWM_XDISPLAY(vwm), win, classhint) &&
		    !strcmp(classhint->res_class, CONSOLE_WM_CLASS))
			ret = 1;

		if (classhint->res_class)
			XFree(classhint->res_class);

		if (classhint->res_name)
			XFree(classhint->res_name);

		XFree(classhint);
	}

	return ret;
}


/* helper for doing the classification/placement of a window becoming managed */
static void vwm_win_assimilate(vwm_t *vwm, vwm_window_t *vwin)
{
	vwm_xwindow_t		*xwin = vwin->xwindow;
	XWindowAttributes	attrs;
	XWindowChanges		changes = { .border_width = WINDOW_BORDER_WIDTH };
	const vwm_screen_t	*scr = NULL;

	if (win_is_console(vwm, xwin->id)) {
		vwm->console = vwin;
		vwm_win_shelve(vwm, vwin);
		vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
	}

	/* TODO: this is a good place to hook in a window placement algo */

	/* on client-requested mapping we place the window */
	/* we place the window on the screen containing the the pointer only if that screen is empty,
	 * otherwise we place windows on the screen containing the currently focused window */
	scr = vwm_screen_find(vwm, VWM_SCREEN_REL_POINTER);
	if (vwm_screen_is_empty(vwm, scr, vwin->xwindow)) {
		/* focus the new window if it isn't already focused when it's going to an empty screen */
		VWM_TRACE("window \"%s\" is alone on screen \"%i\", focusing", vwin->xwindow->name, scr->screen_number);
		vwm_win_focus(vwm, vwin);
	} else {
		/* FIXME TODO: there's some situation where we get here but focused_desktop->focused_window == NULL,
		 * which shouldn't be possible; for there to be a non-empty screen, the focused_desktop must have a focused_window.
		 */
		if (vwm->focused_desktop->focused_window)
			scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, vwm->focused_desktop->focused_window->xwindow);
		else
			VWM_BUG("Screen is not empty, but vwm->focused_desktop->focused_window is NULL!");
	}

	changes.x = scr->x_org;
	changes.y = scr->y_org;

	/* XXX TODO: does this belong here? */
	XGetWMNormalHints(VWM_XDISPLAY(vwm), xwin->id, vwin->hints, &vwin->hints_supplied);
	XGetWindowAttributes(VWM_XDISPLAY(vwm), xwin->id, &attrs);

	vwm_win_autoconf_magic(vwm, vwin, scr, changes.x, changes.y, attrs.width, attrs.height);

	if (vwin->autoconfigured == VWM_WIN_AUTOCONF_ALL)
		changes.border_width = 0;

	vwin->client.x = changes.x;
	vwin->client.y = changes.y;
	vwin->client.height = attrs.height;
	vwin->client.width = attrs.width;
	vwin->client.border_width = changes.border_width;

	XConfigureWindow(VWM_XDISPLAY(vwm), xwin->id, (CWX | CWY | CWBorderWidth), &changes);
}


/* promote an unmanaged window to a managed one */
vwm_window_t * vwm_win_manage_xwin(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	vwm_window_t	*focused, *vwin = NULL;

	VWM_TRACE_WIN(xwin->id, "managing");

	if (xwin->managed) {
		VWM_TRACE_WIN(xwin->id, "suppressed re-management of xwin=%p", xwin);
		goto _fail;
	}

	if (!(vwin = (vwm_window_t *)calloc(1, sizeof(vwm_window_t)))) {
		VWM_PERROR("Failed to allocate vwin");
		goto _fail;
	}

	XUngrabButton(VWM_XDISPLAY(vwm), AnyButton, AnyModifier, xwin->id);
	XGrabButton(VWM_XDISPLAY(vwm), AnyButton, WM_GRAB_MODIFIER, xwin->id, False, (PointerMotionMask | ButtonPressMask | ButtonReleaseMask), GrabModeAsync, GrabModeAsync, None, None);
	XGrabKey(VWM_XDISPLAY(vwm), AnyKey, WM_GRAB_MODIFIER, xwin->id, False, GrabModeAsync, GrabModeAsync);
	XSetWindowBorder(VWM_XDISPLAY(vwm), xwin->id, vwm->colors.unfocused_window_border_color.pixel);

	vwin->hints = XAllocSizeHints();
	if (!vwin->hints) {
		VWM_PERROR("Failed to allocate WM hints");
		goto _fail;
	}
	XGetWMNormalHints(VWM_XDISPLAY(vwm), xwin->id, vwin->hints, &vwin->hints_supplied);

	xwin->managed = vwin;
	vwin->xwindow = xwin;

	vwin->desktop = vwm->focused_desktop;
	vwin->autoconfigured = VWM_WIN_AUTOCONF_NONE;
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

	if ((vwin->hints_supplied & (USSize | PSize))) {
		vwin->client.width = vwin->hints->base_width;
		vwin->client.height = vwin->hints->base_height;
	}

	/* put it on the global windows_mru list, if there's a focused window insert the new one after it */
	if (!list_empty(&vwm->windows_mru) && (focused = vwm_win_get_focused(vwm))) {
		/* insert the vwin immediately after the focused window, so Mod1+Tab goes to the new window */
		list_add(&vwin->windows_mru, &focused->windows_mru);
	} else {
		list_add(&vwin->windows_mru, &vwm->windows_mru);
	}

	vwm_win_assimilate(vwm, vwin);

	/* always raise newly managed windows so we know about them. */
	XRaiseWindow(VWM_XDISPLAY(vwm), xwin->id);

	/* if the desktop has no focused window yet, automatically focus the newly managed one */
	if (!vwm->focused_desktop->focused_window) {
		VWM_TRACE("Mapped new window \"%s\" is alone on desktop \"%p\", focusing", xwin->name, vwm->focused_desktop);
		vwm_win_focus(vwm, vwin);
	}

	return vwin;

_fail:
	if (vwin) {
		if (vwin->hints)
			XFree(vwin->hints);
		free(vwin);
	}

	return NULL;
}


/* migrate a window to a new desktop, focuses the destination desktop as well */
void vwm_win_migrate(vwm_t *vwm, vwm_window_t *vwin, vwm_desktop_t *desktop)
{
	vwm_win_unfocus(vwm, vwin);			/* go through the motions of unfocusing the window if it is focused */
	vwin->desktop = desktop;			/* assign the new desktop */
	vwm_desktop_focus(vwm, desktop);		/* currently we always focus the new desktop in a migrate */

	vwm_win_focus(vwm, vwin);			/* focus the window so borders get updated */
	vwm_win_mru(vwm, vwin); /* TODO: is this right? shouldn't the Mod1 release be what's responsible for this? I migrate so infrequently it probably doesn't matter */

	XRaiseWindow(VWM_XDISPLAY(vwm), vwin->xwindow->id); /* ensure the window is @ top of stack */
}


/* "send" a window to another desktop, no desktop/context switching occurs. */
void vwm_win_send(vwm_t *vwm, vwm_window_t *vwin, vwm_desktop_t *desktop)
{
	if (desktop == vwin->desktop)
		return;

	vwm_win_unfocus(vwm, vwin);
	vwm_win_unmap(vwm, vwin);
	vwin->desktop = desktop;

	/* XXX: only focus the destination desktop when not the focused context, as
	 * it creates an awkward disconnect for the focused context's focused desktop
	 * to become updated to something else while looking at it without actually
	 * realizing that focus change like a migrate does.
	 */
	if (vwm->focused_desktop->context != desktop->context)
		desktop->context->focused_desktop = desktop;

	vwm_win_set_focused(vwm, vwin);
	XRaiseWindow(VWM_XDISPLAY(vwm), vwin->xwindow->id); /* ensure the window is @ top of stack */
}
