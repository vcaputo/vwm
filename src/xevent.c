/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2017  Vito Caputo - <vcaputo@pengaru.com>
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

#include "charts.h"
#include "clickety.h"
#include "composite.h"
#include "desktop.h"
#include "key.h"
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

	if ((xwin = vwm_xwin_lookup(vwm, ev->window)))
		vwm_xwin_destroy(vwm, xwin);
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

	if ((xwin = vwm_xwin_lookup(vwm, ev->window)) && xwin->managed) {
		if (change_mask & CWWidth && change_mask & CWHeight)
			vwm_win_autoconf_magic(vwm, xwin->managed, NULL, ev->x, ev->y, ev->width, ev->height);

		if (xwin->managed->autoconfigured == VWM_WIN_AUTOCONF_ALL)
			changes.border_width = 0;
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
		if (xwin->chart)
			vwm_chart_set_visible_size(vwm->charts, xwin->chart, attrs.width, attrs.height);

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
				xwin->client_mapped = 0;
			}
		} else {
			/* if it's not managed, we can't have caused the map */
			xwin->client_mapped = 0;
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
		if (!xwin->managed && !xwin->attrs.override_redirect)
			xwin->managed = vwm_win_manage_xwin(vwm, xwin);

		if (xwin->managed && xwin->managed->mapping) {
			VWM_TRACE("swallowed vwm-induced MapNotify");
			xwin->managed->mapping = 0;
		} else {
			/* some windows like popup dialog boxes bypass MapRequest */
			xwin->client_mapped = 1;
		}

		vwm_composite_handle_map(vwm, xwin);
	}
}


void vwm_xevent_handle_map_request(vwm_t *vwm, XMapRequestEvent *ev)
{
	vwm_xwindow_t	*xwin;
	vwm_window_t	*vwin = NULL;
	int		domap = 1;

	xwin = vwm_xwin_lookup(vwm, ev->window);
	if (xwin) {
		xwin->client_mapped = 1;

		if (!(vwin = xwin->managed)) {
			/* Basically all managed windows become managed on the map request,
			 * even previously managed ones are unmanaged on unmap, then remanaged
			 * on subsequent map request.  Exceptions are preexisting windows that
			 * are already mapped at create time, those won't generate map requests
			 * but are managed at create.
			 */
			vwin = vwm_win_manage_xwin(vwm, xwin);
			VWM_TRACE("managed xwin \"%s\" at map request", xwin->name);
		}

		/* XXX: note that _normally_ both xwin and vwin should be non-NULL here, and
		 * the care being taken WRT !xwin or !vwin is purely defensive to permit the
		 * default of simply mapping windows on request when things are broken.
		 */

		domap = vwm_xwin_is_mapped(vwm, xwin);
	}

	if (domap) {
		if (vwin) {
			vwm_win_map(vwm, vwin);

			/* XSetInputFocus() must to happen after XMapWindow(), so do it here. */
			if (vwm_win_get_focused(vwm) == vwin)
				XSetInputFocus(VWM_XDISPLAY(vwm), vwin->xwindow->id, RevertToPointerRoot, CurrentTime);
		} else {
			/* this is unexpected */
			XMapWindow(VWM_XDISPLAY(vwm), ev->window);
			VWM_BUG("handled map request of unmanaged window vwin=%p xwin=%p id=%u", vwin, xwin, (unsigned int)ev->window);
		}
	}
}


void vwm_xevent_handle_property_notify(vwm_t *vwm, XPropertyEvent *ev)
{
	vwm_xwindow_t	*xwin;

	if ((xwin = vwm_xwin_lookup(vwm, ev->window)) &&
	    ev->atom == vwm->wm_pid_atom &&
	    ev->state == PropertyNewValue)
		vwm_xwin_setup_chart(vwm, xwin);
}


void vwm_xevent_handle_focusin(vwm_t *vwm, XFocusInEvent *ev)
{
	vwm_window_t	*vwin;

	if (ev->mode != NotifyNormal)
		return;

	if ((vwin = vwm_win_lookup(vwm, ev->window)) &&
	    (vwin != vwm_win_get_focused(vwm)))
		vwm_win_set_focused(vwm, vwin);
}


void vwm_xevent_handle_mapping_notify(vwm_t *vwm, XMappingEvent *ev)
{
	XRefreshKeyboardMapping(ev);
}
