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
	/* bare X windows stuff, there's a distinction between bare xwindows and the vwm managed windows */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>

#include "charts.h"
#include "composite.h"
#include "list.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"


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

	XSendEvent(VWM_XDISPLAY(vwm), xwin->id, False, 0, &event);
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


/* determine if a window is mapped (vwm-mapped) according to the focused context */
int vwm_xwin_is_mapped(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	vwm_window_t	*vwin = xwin->managed;

	if (!xwin->client_mapped || !vwin)
		return xwin->client_mapped;

	return (vwm->focused_desktop == vwin->desktop);
}


/* helper to get the client pid for a window */
static int vwm_xwin_get_pid(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	Atom		type;
	int		fmt;
	unsigned long	nitems;
	unsigned long	nbytes;
	long		*foo = NULL;
	int		pid;

	if (XGetWindowProperty(VWM_XDISPLAY(vwm), xwin->id, vwm->wm_pid_atom, 0, 1, False, XA_CARDINAL,
			       &type, &fmt, &nitems, &nbytes, (unsigned char **)&foo) != Success || !foo)
		return -1;

	pid = *foo;
	XFree(foo);

	return pid;
}


/* establishes an chart on xwin if appropriate and the pid is available */
void vwm_xwin_setup_chart(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	/* XXX FIXME: handle getting called multiple times on the same xwin */

	/* for regular windows create a monitoring chart */
	if (!xwin->attrs.override_redirect) {
		int	pid = vwm_xwin_get_pid(vwm, xwin);

		if (pid != -1)
			xwin->chart = vwm_chart_create(vwm->charts, pid, xwin->attrs.width, xwin->attrs.height, NULL); /* TODO: windows have names, incorporate it if present. */
	}
}

/* override_redirect windows typically should not be managed, and it'd be nice if we could
 * just blindly respect that, but X is a dumpster fire and for multiple reasons I'm going
 * to use some heuristics to only not manage override_redirect windows when they're substantially
 * smaller than the size of the display (popup/popover type shit).
 *
 * When any old X client can create a fullscreen override_redirect window it not only makes
 * fullscreen games and shit not explicitly focusable/managable from a vwm perspective, it
 * also creates a real potential security issue.
 */
int vwm_xwin_should_manage(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	const vwm_screen_t	*scr;

	if (!xwin->attrs.override_redirect)
		return 1;

	scr = vwm_screen_find(vwm, VWM_SCREEN_REL_XWIN, xwin);
	if (!scr)
		return 1;

	/* TODO: for now just using an >= fullscreen heuristic, but should really
	 * trigger for > XX% coverage.  This suffices for managing annoying
	 * override_redirect fullscreen windows.
	 */
	if (xwin->attrs.width >= scr->width && xwin->attrs.height >= scr->height)
		return 1;

	return 0;
}

/* creates and potentially manages a new window (called in response to CreateNotify events, and during startup for all existing windows) */
/* if the window is already mapped and not an override_redirect window, it becomes managed here. */
vwm_xwindow_t * vwm_xwin_create(vwm_t *vwm, Window win, vwm_grab_mode_t grabbed)
{
	vwm_xwindow_t		*xwin = NULL;
	XWindowAttributes	attrs;

	VWM_TRACE_WIN(win, "creating");

	/* prevent races */
	if (!grabbed) {
		XGrabServer(VWM_XDISPLAY(vwm));
		XSync(VWM_XDISPLAY(vwm), False);
	}

	/* verify the window still exists */
	if (!XGetWindowAttributes(VWM_XDISPLAY(vwm), win, &attrs))
		goto _out_grabbed;

	/* don't create InputOnly windows */
	if (attrs.class == InputOnly)
		goto _out_grabbed;

	if (!(xwin = (vwm_xwindow_t *)calloc(1, sizeof(vwm_xwindow_t)))) {
		VWM_PERROR("Failed to allocate xwin");
		goto _out_grabbed;
	}

	xwin->id = win;
	xwin->attrs = attrs;
	XFetchName(VWM_XDISPLAY(vwm), win, &xwin->name);

	/* This is so we get the PropertyNotify event and can get the pid when it's set post-create,
	 * with my _NET_WM_PID patch the property is immediately available.
	 * FocusChangeMask is needed to notice when clients call XSetInputFocus().
	 */
	XSelectInput(VWM_XDISPLAY(vwm), win, PropertyChangeMask | FocusChangeMask);

	/* we must track the mapped-by-client state of the window independent of managed vs. unmanaged because
	 * in the case of override_redirect windows they may be unmapped (invisible) or mapped (visible) like menus without being managed.
	 * otherwise we could just use !xwin.managed to indicate unmapped, which is more vwm2-like, but insufficient when compositing.
	 */
	xwin->client_mapped = (attrs.map_state != IsUnmapped);

	vwm_xwin_setup_chart(vwm, xwin);
	vwm_composite_xwin_create(vwm, xwin);

	list_add_tail(&xwin->xwindows, &vwm->xwindows);	/* created windows are always placed on the top of the stacking order */

	VWM_TRACE_WIN(win, "name=\"%s\" override_redirect=%i client_mapped=%i\n",
		xwin->name, (int)attrs.override_redirect, (int)xwin->client_mapped);

	if (xwin->client_mapped && vwm_xwin_should_manage(vwm, xwin))
		vwm_win_manage_xwin(vwm, xwin);

_out_grabbed:
	if (!grabbed)
		XUngrabServer(VWM_XDISPLAY(vwm));

	return xwin;
}


/* destroy a window, called in response to DestroyNotify events */
/* if the window is also managed it will be unmanaged first */
void vwm_xwin_destroy(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	XGrabServer(VWM_XDISPLAY(vwm));
	XSync(VWM_XDISPLAY(vwm), False);

	if (xwin->managed)
		vwm_win_unmanage(vwm, xwin->managed);

	list_del(&xwin->xwindows);

	if (xwin->name)
		XFree(xwin->name);

	if (xwin->chart)
		vwm_chart_destroy(vwm->charts, xwin->chart);

	vwm_composite_xwin_destroy(vwm, xwin);

	free(xwin);

	XUngrabServer(VWM_XDISPLAY(vwm));
}


/* maintain the stack-ordered xwindows list, when new_above is != None xwin is to be placed above new_above, when == None xwin goes to the bottom. */
void vwm_xwin_restack(vwm_t *vwm, vwm_xwindow_t *xwin, Window new_above)
{
	Window		old_above;
#ifdef TRACE
	vwm_xwindow_t	*tmp;
	fprintf(stderr, "restack of %#x new_above=%#x\n", (unsigned int)xwin->id, (unsigned int)new_above);
	fprintf(stderr, "restack pre:");
	list_for_each_entry(tmp, &vwm->xwindows, xwindows)
		fprintf(stderr, " %#x", (unsigned int)tmp->id);
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
	list_for_each_entry(tmp, &vwm->xwindows, xwindows)
		fprintf(stderr, " %#x", (unsigned int)tmp->id);
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
	XGrabServer(VWM_XDISPLAY(vwm));
	XSync(VWM_XDISPLAY(vwm), False);
	XQueryTree(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), &root, &parent, &children, &n_children);

	for (i = 0; i < n_children; i++) {
		if (children[i] == None)
			continue;

		if ((vwm_xwin_create(vwm, children[i], VWM_GRABBED) == NULL))
			goto _fail_grabbed;
	}

	XUngrabServer(VWM_XDISPLAY(vwm));

	if (children)
		XFree(children);

	return 1;

_fail_grabbed:
	XUngrabServer(VWM_XDISPLAY(vwm));

	if (children)
		XFree(children);

	return 0;
}
