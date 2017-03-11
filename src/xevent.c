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

#include "X11/Xlib.h"

#include "key.h"
#include "clickety.h"
#include "composite.h"
#include "desktop.h"
#include "screen.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

/* this forms the glue between the X event loop in vwm.c and the rest of the
 * code making up vwm */

void vwm_xevent_handle_key_press(vwm_t *vwm, XKeyPressedEvent *ev)
{
	vwm_key_pressed(vwm,  ev->window, ev);
}


void vwm_xevent_handle_key_release(vwm_t *vwm, XKeyReleasedEvent *ev)
{
	vwm_key_released(vwm, ev->window, ev);
}


void vwm_xevent_handle_button_press(vwm_t *vwm, XButtonPressedEvent *ev)
{
	vwm_clickety_pressed(vwm, ev->window, ev);
}


void vwm_xevent_handle_motion_notify(vwm_t *vwm, XMotionEvent *ev)
{
	vwm_clickety_motion(vwm, ev->window, ev);
}


void vwm_xevent_handle_button_release(vwm_t *vwm, XButtonReleasedEvent *ev)
{
	vwm_clickety_released(vwm, ev->window, ev);
}


void vwm_xevent_handle_create_notify(vwm_t *vwm, XCreateWindowEvent *ev)
{
	vwm_xwin_create(vwm, ev->window, VWM_NOT_GRABBED);
}


void vwm_xevent_handle_destroy_notify(vwm_t *vwm, XDestroyWindowEvent *ev)
{
	vwm_xwindow_t	*xwin;

	if ((xwin = vwm_xwin_lookup(vwm, ev->window))) {
		vwm_xwin_destroy(vwm, xwin);
	}
}


void vwm_xevent_handle_configure_request(vwm_t *vwm, XConfigureRequestEvent *ev)
{
	XWindowChanges	changes = {
				.x = ev->x,		/* TODO: for now I don't manipulate anything */
				.y = ev->y,
				.width = ev->width,
				.height = ev->height,
				.border_width = WINDOW_BORDER_WIDTH /* except I do override whatever the border width may be */
			};
	unsigned long	change_mask = (ev->value_mask & (CWX | CWY | CWWidth | CWHeight)) | CWBorderWidth;
	vwm_xwindow_t	*xwin;

	/* XXX: windows raising themselves is annoying, so discard CWSibling and CWStackMode. */

	if ((xwin = vwm_xwin_lookup(vwm, ev->window)) &&
	    xwin->managed &&
	    xwin->managed->autoconfigured == VWM_WIN_AUTOCONF_ALL) {
		/* this is to allow auto-allscreen to succeed in getting a borderless window configured */
		change_mask &= ~CWBorderWidth;
	}

	XConfigureWindow(VWM_XDISPLAY(vwm), ev->window, change_mask, &changes);
}


void vwm_xevent_handle_configure_notify(vwm_t *vwm, XConfigureEvent *ev)
{
	vwm_xwindow_t	*xwin;

	if ((xwin = vwm_xwin_lookup(vwm, ev->window))) {
		XWindowAttributes	attrs;
		vwm_xwin_restack(vwm, xwin, ev->above);
		XGetWindowAttributes(VWM_XDISPLAY(vwm), ev->window, &attrs);
		vwm_composite_handle_configure(vwm, xwin, &attrs);
		if (xwin->overlay) vwm_overlay_set_visible_size(vwm->overlays, xwin->overlay, attrs.width, attrs.height);

		VWM_TRACE("pre x=%i y=%i w=%i h=%i\n", xwin->attrs.x, xwin->attrs.y, xwin->attrs.width, xwin->attrs.height);
		xwin->attrs = attrs;
		VWM_TRACE("post x=%i y=%i w=%i h=%i\n", xwin->attrs.x, xwin->attrs.y, xwin->attrs.width, xwin->attrs.height);
	}
}


void vwm_xevent_handle_unmap_notify(vwm_t *vwm, XUnmapEvent *ev)
{
	vwm_xwindow_t	*xwin;

	/* unlike MapRequest, we simply are notified when a window is unmapped. */
	if ((xwin = vwm_xwin_lookup(vwm, ev->window))) {
		if (xwin->managed) {
			if (xwin->managed->unmapping) {
				VWM_TRACE("swallowed vwm-induced UnmapNotify");
				xwin->managed->unmapping = 0;
			} else {
				/* client requested unmap, demote the window and note the unmapped state */
				vwm_win_unmanage(vwm, xwin->managed);
				xwin->mapped = 0;
			}
		} else {
			/* if it's not managed, we can't have caused the map */
			xwin->mapped = 0;
		}

		vwm_composite_damage_win(vwm, xwin);
	}
}


void vwm_xevent_handle_map_notify(vwm_t *vwm, XMapEvent *ev)
{
	vwm_xwindow_t	*xwin;

	if ((xwin = vwm_xwin_lookup(vwm, ev->window))) {

		/* XXX: in some circumstances (randomly mplayer -fs) it we see an event sequence for a window like:
		 * create_notify->map_request (manage)->configure_request->unmap_notify (unmanage)->configure_notify->map_notify (unmanaged!)
		 * which unless the window's an override_redirect is incorrect.
		 * So implicitly manage the window if it's not managed and !override_redirect, since it's now mapped.
		 */
		if (!xwin->managed && !xwin->attrs.override_redirect) {
			xwin->managed = vwm_win_manage_xwin(vwm, xwin);
		}

		if (xwin->managed && xwin->managed->mapping) {
			VWM_TRACE("swallowed vwm-induced MapNotify");
			xwin->managed->mapping = 0;
		} else {
			/* some windows like popup dialog boxes bypass MapRequest */
			xwin->mapped = 1;
		}

		vwm_composite_handle_map(vwm, xwin);
	}
}


void vwm_xevent_handle_map_request(vwm_t *vwm, XMapRequestEvent *ev)
{
	vwm_xwindow_t	*xwin;
	vwm_window_t	*vwin = NULL;
	int		domap = 1;

	/* FIXME TODO: this is a fairly spuriously open-coded mess, this stuff
	 * needs to be factored and moved elsewhere */

	if ((xwin = vwm_xwin_lookup(vwm, ev->window)) &&
	    ((vwin = xwin->managed) || (vwin = vwm_win_manage_xwin(vwm, xwin)))) {
		XWindowAttributes	attrs;
		XWindowChanges		changes = {.x = 0, .y = 0};
		unsigned		changes_mask = (CWX | CWY);
		XClassHint		*classhint;
		const vwm_screen_t	*scr = NULL;

		xwin->mapped = 1;	/* note that the client mapped the window */

		/* figure out if the window is the console */
		if ((classhint = XAllocClassHint())) {
			if (XGetClassHint(VWM_XDISPLAY(vwm), ev->window, classhint) && !strcmp(classhint->res_class, CONSOLE_WM_CLASS)) {
				vwm->console = vwin;
				vwm_win_shelve(vwm, vwin);
				vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
				domap = 0;
			}

			if (classhint->res_class) XFree(classhint->res_class);
			if (classhint->res_name) XFree(classhint->res_name);
			XFree(classhint);
		}

		/* TODO: this is a good place to hook in a window placement algo */

		/* on client-requested mapping we place the window */
		if (!vwin->shelved) {
			/* we place the window on the screen containing the the pointer only if that screen is empty,
			 * otherwise we place windows on the screen containing the currently focused window */
			/* since we query the geometry of windows in determining where to place them, a configuring
			 * flag is used to exclude the window being configured from those queries */
			scr = vwm_screen_find(vwm, VWM_SCREEN_REL_POINTER);
			vwin->configuring = 1;
			if (vwm_screen_is_empty(vwm, scr)) {
				/* focus the new window if it isn't already focused when it's going to an empty screen */
				VWM_TRACE("window \"%s\" is alone on screen \"%i\", focusing", vwin->xwindow->name, scr->screen_number);
				vwm_win_focus(vwm, vwin);
			} else {
				scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, vwm->focused_desktop->focused_window->xwindow);
			}
			vwin->configuring = 0;

			changes.x = scr->x_org;
			changes.y = scr->y_org;
		} else if (vwm->focused_context == VWM_CONTEXT_SHELF) {
			scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, vwm->focused_shelf->xwindow);
			changes.x = scr->x_org;
			changes.y = scr->y_org;
		}

		/* XXX TODO: does this belong here? */
		XGetWMNormalHints(VWM_XDISPLAY(vwm), ev->window, vwin->hints, &vwin->hints_supplied);
		XGetWindowAttributes(VWM_XDISPLAY(vwm), ev->window, &attrs);


		/* if the window size is precisely the screen size then directly "allscreen" the window right here */
		if (!vwin->shelved && scr &&
		    attrs.width == scr->width &&
		    attrs.height == scr->height) {
			VWM_TRACE("auto-allscreened window \"%s\"", vwin->xwindow->name);
			changes.border_width = 0;
			changes_mask |= CWBorderWidth;
			vwin->autoconfigured = VWM_WIN_AUTOCONF_ALL;
		}

		vwin->client.x = changes.x;
		vwin->client.y = changes.y;
		vwin->client.height = attrs.height;
		vwin->client.width = attrs.width;

		XConfigureWindow(VWM_XDISPLAY(vwm), ev->window, changes_mask, &changes);
	}

	if (domap) {
		XMapWindow(VWM_XDISPLAY(vwm), ev->window);
		if (vwin && vwin->desktop->focused_window == vwin) {
			XSync(VWM_XDISPLAY(vwm), False);
			XSetInputFocus(VWM_XDISPLAY(vwm), vwin->xwindow->id, RevertToPointerRoot, CurrentTime);
		}
	}
}


void vwm_xevent_handle_property_notify(vwm_t *vwm, XPropertyEvent *ev)
{
	vwm_xwindow_t	*xwin;

	if ((xwin = vwm_xwin_lookup(vwm, ev->window)) &&
	    ev->atom == vwm->wm_pid_atom &&
	    ev->state == PropertyNewValue) vwm_xwin_setup_overlay(vwm, xwin);
}


void vwm_xevent_handle_mapping_notify(vwm_t *vwm, XMappingEvent *ev)
{
	XRefreshKeyboardMapping(ev);
}
