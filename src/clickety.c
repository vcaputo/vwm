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
	/* input event handling stuff */

#include <X11/Xlib.h>

#include "clickety.h"
#include "desktop.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

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

static vwm_clickety_t clickety;

/* helper function for resizing the window, how the motion is applied depends on where in the window the impetus event occurred */
static void compute_resize(XEvent *terminus, XWindowAttributes *new)
{
	vwm_window_t	*vwin;
	int		dw = (clickety.orig.width / 2);
	int		dh = (clickety.orig.height / 2);
	int		xdelta = (terminus->xbutton.x_root - clickety.impetus_x_root);
	int		ydelta = (terminus->xbutton.y_root - clickety.impetus_y_root);
	int		min_width = 0, min_height = 0;
	int		width_inc = 1, height_inc = 1;

	/* TODO: there's a problem here WRT border width, I should be considering it, I just haven't bothered to fix it since it doesn't seem to matter */
	if ((vwin = clickety.vwin) && vwin->hints) {
		if ((vwin->hints_supplied & PMinSize)) {
			min_width = vwin->hints->min_width;
			min_height = vwin->hints->min_height;
			VWM_TRACE("window size hints exist and minimum sizes are w=%i h=%i", min_width, min_height);
		}

		if ((vwin->hints_supplied & PResizeInc)) {
			width_inc = vwin->hints->width_inc;
			height_inc = vwin->hints->height_inc;
			VWM_TRACE("window size hints exist and resize increments are w=%i h=%i", width_inc, height_inc);
			if (!width_inc)
				width_inc = 1;

			if (!height_inc)
				height_inc = 1;
		}
	}

	xdelta = xdelta / width_inc * width_inc;
	ydelta = ydelta / height_inc * height_inc;

	if (clickety.impetus_x < dw && clickety.impetus_y < dh) {
		/* grabbed top left */
		new->x = clickety.orig.x + xdelta;
		new->y = clickety.orig.y + ydelta;
		new->width = clickety.orig.width - xdelta;
		new->height = clickety.orig.height - ydelta;
	} else if (clickety.impetus_x > dw && clickety.impetus_y < dh) {
		/* grabbed top right */
		new->x = clickety.orig.x;
		new->y = clickety.orig.y + ydelta;
		new->width = clickety.orig.width + xdelta;
		new->height = clickety.orig.height - ydelta;
	} else if (clickety.impetus_x < dw && clickety.impetus_y > dh) {
		/* grabbed bottom left */
		new->x = clickety.orig.x + xdelta;
		new->y = clickety.orig.y;
		new->width = clickety.orig.width - xdelta;
		new->height = clickety.orig.height + ydelta;
	} else if (clickety.impetus_x > dw && clickety.impetus_y > dh) {
		/* grabbed bottom right */
		new->x = clickety.orig.x;
		new->y = clickety.orig.y;
		new->width = clickety.orig.width + xdelta;
		new->height = clickety.orig.height + ydelta;
	}

	/* constrain the width and height of the window according to the minimums */
	if (new->width < min_width) {
		if (clickety.orig.x != new->x)
			new->x -= (min_width - new->width);

		new->width = min_width;
	}

	if (new->height < min_height) {
		if (clickety.orig.y != new->y)
			new->y -= (min_height - new->height);

		new->height = min_height;
	}
}


void vwm_clickety_motion(vwm_t *vwm, Window win, XMotionEvent *motion)
{
	XWindowChanges	changes = { .border_width = WINDOW_BORDER_WIDTH };

	if (!clickety.vwin)
		return;

	/* TODO: verify win matches clickety.vwin? */
	switch (clickety.mode) {
		case VWM_ADJUST_MOVE:
			changes.x = clickety.orig.x + (motion->x_root - clickety.impetus_x_root);
			changes.y = clickety.orig.y + (motion->y_root - clickety.impetus_y_root);
			XConfigureWindow(VWM_XDISPLAY(vwm), win, CWX | CWY | CWBorderWidth, &changes);
			break;

		case VWM_ADJUST_RESIZE: {
			XWindowAttributes	resized;

			/* XXX: it just so happens the XMotionEvent structure is identical to XButtonEvent in the fields
			 * needed by compute_resize... */
			compute_resize((XEvent *)motion, &resized);
			/* TODO: this is probably broken with compositing active, but it doesn't seem to be too messed up in practice */
			/* erase the last rectangle */
			XDrawRectangle(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), VWM_XGC(vwm), clickety.lastrect.x, clickety.lastrect.y, clickety.lastrect.width, clickety.lastrect.height);
			/* draw a frame @ resized coordinates */
			XDrawRectangle(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), VWM_XGC(vwm), resized.x, resized.y, resized.width, resized.height);
			/* remember the last rectangle */
			clickety.lastrect = resized;
			break;
		}

		default:
			break;
	}
}


void vwm_clickety_released(vwm_t *vwm, Window win, XButtonPressedEvent *terminus)
{
	XWindowChanges	changes = { .border_width = WINDOW_BORDER_WIDTH };

	if (!clickety.vwin)
		return;

	switch (clickety.mode) {
		case VWM_ADJUST_MOVE:
			changes.x = clickety.orig.x + (terminus->x_root - clickety.impetus_x_root);
			changes.y = clickety.orig.y + (terminus->y_root - clickety.impetus_y_root);
			XConfigureWindow(VWM_XDISPLAY(vwm), win, CWX | CWY | CWBorderWidth, &changes);
			break;

		case VWM_ADJUST_RESIZE: {
			XWindowAttributes	resized;
			compute_resize((XEvent *)terminus, &resized);
			/* move and resize the window @ resized */
			XDrawRectangle(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), VWM_XGC(vwm), clickety.lastrect.x, clickety.lastrect.y, clickety.lastrect.width, clickety.lastrect.height);
			changes.x = resized.x;
			changes.y = resized.y;
			changes.width = resized.width;
			changes.height = resized.height;
			XConfigureWindow(VWM_XDISPLAY(vwm), win, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &changes);
			XUngrabServer(VWM_XDISPLAY(vwm));
			break;
		}

		default:
			break;
	}
	/* once you manipulate the window it's no longer fullscreened, simply hitting Mod1+Return once will restore fullscreened mode */
	clickety.vwin->autoconfigured = VWM_WIN_AUTOCONF_NONE;

	clickety.vwin = NULL; /* reset clickety */

	XFlush(VWM_XDISPLAY(vwm));
	XUngrabPointer(VWM_XDISPLAY(vwm), CurrentTime);
}


/* on pointer buttonpress we initiate a clickety sequence; setup clickety with the window and impetus coordinates.. */
int vwm_clickety_pressed(vwm_t *vwm, Window win, XButtonPressedEvent *impetus)
{
	vwm_window_t	*vwin;

	/* verify the window still exists */
	if (!XGetWindowAttributes(VWM_XDISPLAY(vwm), win, &clickety.orig))
		goto _fail;

	if (!(vwin = vwm_win_lookup(vwm, win)))
		goto _fail;

	if (impetus->state & WM_GRAB_MODIFIER) {

		/* always set the input focus to the clicked window, note if we allow this to happen on the root window, it enters sloppy focus mode
		 * until a non-root window is clicked, which is an interesting hybrid but not how I prefer it. */
		if (vwin != vwm->focused_desktop->focused_window && vwin->xwindow->id != VWM_XROOT(vwm)) {
			vwm_win_focus(vwm, vwin);
			vwm_win_mru(vwm, vwin);
		}

		switch (impetus->button) {
			case Button1:
				/* immediately raise the window if we're relocating,
				 * resizes are supported without raising (which also enables NULL resizes to focus without raising) */
				clickety.mode = VWM_ADJUST_MOVE;
				XRaiseWindow(VWM_XDISPLAY(vwm), win);
				break;

			case Button3:
				/* grab the server on resize for the xor rubber-banding's sake */
				XGrabServer(VWM_XDISPLAY(vwm));
				XSync(VWM_XDISPLAY(vwm), False);

				/* FIXME: none of the resize DrawRectangle() calls consider the window border. */
				XDrawRectangle(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), VWM_XGC(vwm), clickety.orig.x, clickety.orig.y, clickety.orig.width, clickety.orig.height);
				clickety.lastrect = clickety.orig;

				clickety.mode = VWM_ADJUST_RESIZE;
				break;

			default:
				goto _fail;
		}
		clickety.vwin = vwin;
		clickety.impetus_x_root = impetus->x_root;
		clickety.impetus_y_root = impetus->y_root;
		clickety.impetus_x = impetus->x;
		clickety.impetus_y = impetus->y;
	}

	return 1;

_fail:
	XUngrabPointer(VWM_XDISPLAY(vwm), CurrentTime);

	return 0;
}
