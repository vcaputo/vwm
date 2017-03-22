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
	/* desktop/shelf context handling */

#include <X11/Xlib.h>

#include "context.h"
#include "desktop.h"
#include "vwm.h"
#include "xwindow.h"
#include "window.h"

/* switch to the desired context if it isn't already the focused one, inform caller if anything happened */
int vwm_context_focus(vwm_t *vwm, vwm_context_t desired_context)
{
	vwm_context_t	entry_context = vwm->focused_context;

	switch (vwm->focused_context) {
		vwm_xwindow_t	*xwin;
		vwm_window_t	*vwin;

		case VWM_CONTEXT_SHELF:
			if (desired_context == VWM_CONTEXT_SHELF)
				break;

			/* desired == DESKTOP && focused == SHELF */

			VWM_TRACE("unmapping shelf window \"%s\"", vwm->focused_shelf->xwindow->name);
			vwm_win_unmap(vwm, vwm->focused_shelf);
			XFlush(VWM_XDISPLAY(vwm)); /* for a more responsive feel */

			/* map the focused desktop, from the top of the stack down */
			list_for_each_entry_prev(xwin, &vwm->xwindows, xwindows) {
				if (!(vwin = xwin->managed))
					continue;

				if (vwin->desktop == vwm->focused_desktop && !vwin->shelved) {
					VWM_TRACE("Mapping desktop window \"%s\"", xwin->name);
					vwm_win_map(vwm, vwin);
				}
			}

			if (vwm->focused_desktop->focused_window) {
				VWM_TRACE("Focusing \"%s\"", vwm->focused_desktop->focused_window->xwindow->name);
				XSetInputFocus(VWM_XDISPLAY(vwm), vwm->focused_desktop->focused_window->xwindow->id, RevertToPointerRoot, CurrentTime);
			}

			vwm->focused_context = VWM_CONTEXT_DESKTOP;
			break;

		case VWM_CONTEXT_DESKTOP:
			/* unmap everything, map the shelf */
			if (desired_context == VWM_CONTEXT_DESKTOP)
				break;

			/* desired == SHELF && focused == DESKTOP */

			/* there should be a focused shelf if the shelf contains any windows, we NOOP the switch if the shelf is empty. */
			if (vwm->focused_shelf) {
				/* unmap everything on the current desktop */
				list_for_each_entry(xwin, &vwm->xwindows, xwindows) {
					if (!(vwin = xwin->managed))
						continue;

					if (vwin->desktop == vwm->focused_desktop) {
						VWM_TRACE("Unmapping desktop window \"%s\"", xwin->name);
						vwm_win_unmap(vwm, vwin);
					}
				}

				XFlush(VWM_XDISPLAY(vwm)); /* for a more responsive feel */

				VWM_TRACE("Mapping shelf window \"%s\"", vwm->focused_shelf->xwindow->name);
				vwm_win_map(vwm, vwm->focused_shelf);
				vwm_win_focus(vwm, vwm->focused_shelf);

				vwm->focused_context = VWM_CONTEXT_SHELF;
			}
			break;

		default:
			VWM_BUG("unexpected focused context %x", vwm->focused_context);
			break;
	}

	/* return if the context has been changed, the caller may need to branch differently if nothing happened */
	return (vwm->focused_context != entry_context);
}
