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
	/* bare X windows stuff, there's a distinction between bare xwindows and the vwm managed windows */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdlib.h>

#include "composite.h"
#include "list.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

#define HONOR_OVERRIDE_REDIRECT

/* send a client message to a window (currently used for WM_DELETE) */
void vwm_xwin_message(vwm_t *vwm, vwm_xwindow_t *xwin, Atom type, long foo)
{
	XEvent	event;

	memset(&event, 0, sizeof(event));
	event.xclient.type = ClientMessage;
	event.xclient.window = xwin->id;
	event.xclient.message_type = type;
	event.xclient.format = 32;
	event.xclient.data.l[0] = foo;
	event.xclient.data.l[1] = CurrentTime;	/* XXX TODO: is CurrentTime actually correct to use for this purpose? */

	XSendEvent(vwm->display, xwin->id, False, 0, &event);
}


/* look up the X window in the global xwindows list (includes unmanaged windows like override_redirect/popup menus) */
vwm_xwindow_t * vwm_xwin_lookup(vwm_t *vwm, Window win)
{
	vwm_xwindow_t	*tmp, *xwin = NULL;

	list_for_each_entry(tmp, &vwm->xwindows, xwindows) {
		if (tmp->id == win) {
			xwin = tmp;
			break;
		}
	}

	return xwin;
}


/* determine if a window is mapped (vwm-mapped) according to the current context */
int vwm_xwin_is_mapped(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	int ret = 0;

	if (!xwin->mapped) return 0;

	if (xwin->managed) {
		switch (vwm->focused_context) {
			case VWM_CONTEXT_SHELF:
				if (vwm->focused_shelf == xwin->managed) ret = xwin->mapped;
				break;

			case VWM_CONTEXT_DESKTOP:
				if (vwm->focused_desktop == xwin->managed->desktop && !xwin->managed->shelved) ret = xwin->mapped;
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


/* creates and potentially manages a new window (called in response to CreateNotify events, and during startup for all existing windows) */
/* if the window is already mapped and not an override_redirect window, it becomes managed here. */
vwm_xwindow_t * vwm_xwin_create(vwm_t *vwm, Window win, vwm_grab_mode_t grabbed)
{
	XWindowAttributes	attrs;
	vwm_xwindow_t		*xwin = NULL;

	VWM_TRACE("creating %#x", (unsigned int)win);

	/* prevent races */
	if (!grabbed) {
		XGrabServer(vwm->display);
		XSync(vwm->display, False);
	}

	/* verify the window still exists */
	if (!XGetWindowAttributes(vwm->display, win, &attrs)) goto _out_grabbed;

	/* don't create InputOnly windows */
	if (attrs.class == InputOnly) goto _out_grabbed;

	if (!(xwin = (vwm_xwindow_t *)malloc(sizeof(vwm_xwindow_t)))) {
		VWM_PERROR("Failed to allocate xwin");
		goto _out_grabbed;
	}

	xwin->id = win;
	xwin->attrs = attrs;
	xwin->managed = NULL;
	xwin->name = NULL;
	XFetchName(vwm->display, win, &xwin->name);

	xwin->monitor = NULL;
	xwin->overlay.width = xwin->overlay.height = xwin->overlay.phase = 0;
	xwin->overlay.gen_last_composed = -1;

	/* This is so we get the PropertyNotify event and can get the pid when it's set post-create,
	 * with my _NET_WM_PID patch the property is immediately available */
	XSelectInput(vwm->display, win, PropertyChangeMask);

	/* we must track the mapped-by-client state of the window independent of managed vs. unmanaged because
	 * in the case of override_redirect windows they may be unmapped (invisible) or mapped (visible) like menus without being managed.
	 * otherwise we could just use !xwin.managed to indicate unmapped, which is more vwm2-like, but insufficient when compositing. */
	xwin->mapped = (attrs.map_state != IsUnmapped);

	vwm_overlay_xwin_create(vwm, xwin);
	vwm_composite_xwin_create(vwm, xwin);

	list_add_tail(&xwin->xwindows, &vwm->xwindows);	/* created windows are always placed on the top of the stacking order */

#ifdef HONOR_OVERRIDE_REDIRECT
	if (!attrs.override_redirect && xwin->mapped) vwm_win_manage_xwin(vwm, xwin);
#else
	if (xwin->mapped) vwm_win_manage_xwin(vwm, xwin);
#endif
_out_grabbed:
	if (!grabbed) XUngrabServer(vwm->display);

	return xwin;
}


/* destroy a window, called in response to DestroyNotify events */
/* if the window is also managed it will be unmanaged first */
void vwm_xwin_destroy(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	XGrabServer(vwm->display);
	XSync(vwm->display, False);

	if (xwin->managed) vwm_win_unmanage(vwm, xwin->managed);

	list_del(&xwin->xwindows);

	if (xwin->name) XFree(xwin->name);

	vwm_overlay_xwin_destroy(vwm, xwin);
	vwm_composite_xwin_destroy(vwm, xwin);

	free(xwin);

	XUngrabServer(vwm->display);
}


/* maintain the stack-ordered xwindows list, when new_above is != None xwin is to be placed above new_above, when == None xwin goes to the bottom. */
void vwm_xwin_restack(vwm_t *vwm, vwm_xwindow_t *xwin, Window new_above)
{
	Window		old_above;
#ifdef TRACE
	vwm_xwindow_t	*tmp;
	fprintf(stderr, "restack of %#x new_above=%#x\n", (unsigned int)xwin->id, (unsigned int)new_above);
	fprintf(stderr, "restack pre:");
	list_for_each_entry(tmp, &vwm->xwindows, xwindows) {
		fprintf(stderr, " %#x", (unsigned int)tmp->id);
	}
	fprintf(stderr, "\n");
#endif
	if (xwin->xwindows.prev != &vwm->xwindows) {
		old_above = list_entry(xwin->xwindows.prev, vwm_xwindow_t, xwindows)->id;
	} else {
		old_above = None;
	}

	if (old_above != new_above) {
		vwm_xwindow_t	*new;

		if (new_above == None) {				/* to the bottom of the stack, so just above the &xwindows head */
			list_move(&xwin->xwindows, &vwm->xwindows);
		} else if ((new = vwm_xwin_lookup(vwm, new_above))) {	/* to just above new_above */
			list_move(&xwin->xwindows, &new->xwindows);
		}
	}
#ifdef TRACE
	fprintf(stderr, "restack post:");
	list_for_each_entry(tmp, &vwm->xwindows, xwindows) {
		fprintf(stderr, " %#x", (unsigned int)tmp->id);
	}
	fprintf(stderr, "\n\n");
#endif
}

/* create xwindows for all existing windows (for startup) */
int vwm_xwin_create_existing(vwm_t *vwm)
{
	Window		root, parent;
	Window		*children = NULL;
	unsigned int	n_children, i;

	/* TODO FIXME I don't think this is right anymore, not since we went compositing and split managed vs. bare xwindows... */
	XGrabServer(vwm->display);
	XSync(vwm->display, False);
	XQueryTree(vwm->display, VWM_XROOT(vwm), &root, &parent, &children, &n_children);

	for (i = 0; i < n_children; i++) {
		if (children[i] == None) continue;

		if ((vwm_xwin_create(vwm, children[i], VWM_GRABBED) == NULL)) goto _fail_grabbed;
	}

	XUngrabServer(vwm->display);

	if (children) XFree(children);

	return 1;

_fail_grabbed:
	XUngrabServer(vwm->display);

	if (children) XFree(children);

	return 0;
}