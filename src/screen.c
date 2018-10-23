/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2018  Vito Caputo - <vcaputo@pengaru.com>
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
/* return the appropriate screen, don't use the return value across event loops because randr events reallocate the array. */

#include <X11/Xlib.h>
#include <stdarg.h>
#include <values.h>

#include "list.h"
#include "screen.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

	/* Xinerama/multihead screen functions */

/* return what fraction (0.0-1.0) of the rect x,y,width,height overlaps with scr */
static float vwm_screen_overlaps_rect(vwm_t *vwm, const vwm_screen_t *scr, int x, int y, int width, int height)
{
	float	pct = 0, xover = 0, yover = 0;

	if (scr->x_org + scr->width < x || scr->x_org > x + width ||
	    scr->y_org + scr->height < y || scr->y_org > y + height)
		goto _out;

	/* they overlap, by how much? */
	xover = MIN(scr->x_org + scr->width, x + width) - MAX(scr->x_org, x);
	yover = MIN(scr->y_org + scr->height, y + height) - MAX(scr->y_org, y);

	pct = (xover * yover) / (width * height);
_out:
	VWM_TRACE("xover=%f yover=%f width=%i height=%i pct=%.4f", xover, yover, width, height, pct);
	return pct;
}


/* return what fraction (0.0-1.0) of xwin overlaps with scr */
static float vwm_screen_overlaps_xwin(vwm_t *vwm, const vwm_screen_t *scr, vwm_xwindow_t *xwin)
{
	return vwm_screen_overlaps_rect(vwm, scr, xwin->attrs.x, xwin->attrs.y, xwin->attrs.width, xwin->attrs.height);
}


const vwm_screen_t * vwm_screen_find(vwm_t *vwm, vwm_screen_rel_t rel, ...)
{
	static vwm_screen_t	faux;
	vwm_screen_t		*scr, *best = &faux; /* default to faux as best */
	int			i;

	faux.screen_number = 0;
	faux.x_org = 0;
	faux.y_org = 0;
	faux.width = WidthOfScreen(DefaultScreenOfDisplay(VWM_XDISPLAY(vwm)));
	faux.height = HeightOfScreen(DefaultScreenOfDisplay(VWM_XDISPLAY(vwm)));

	if (!vwm->xinerama_screens)
		goto _out;

#define for_each_screen(_tmp) \
	for (i = 0, _tmp = vwm->xinerama_screens; i < vwm->xinerama_screens_cnt; _tmp = &vwm->xinerama_screens[++i])

	switch (rel) {
		case VWM_SCREEN_REL_RECT: {
			va_list		ap;
			int		x, y, width, height;
			float		best_pct = 0, this_pct;

			va_start(ap, rel);
			x = va_arg(ap, int);
			y = va_arg(ap, int);
			width = va_arg(ap, int);
			height = va_arg(ap, int);
			va_end(ap);

			for_each_screen(scr) {
				this_pct = vwm_screen_overlaps_rect(vwm, scr, x, y, width, height);
				if (this_pct > best_pct) {
					best = scr;
					best_pct = this_pct;
				}
			}
			break;
		}

		case VWM_SCREEN_REL_XWIN: {
			va_list		ap;
			vwm_xwindow_t	*xwin;
			float		best_pct = 0, this_pct;

			va_start(ap, rel);
			xwin = va_arg(ap, vwm_xwindow_t *);
			va_end(ap);

			for_each_screen(scr) {
				this_pct = vwm_screen_overlaps_xwin(vwm, scr, xwin);
				if (this_pct > best_pct) {
					best = scr;
					best_pct = this_pct;
				}
			}
			break;
		}

		case VWM_SCREEN_REL_POINTER: {
			int		root_x, root_y, win_x, win_y;
			unsigned int	mask;
			Window		root, child;

			/* get the pointer coordinates and find which screen it's in */
			XQueryPointer(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), &root, &child, &root_x, &root_y, &win_x, &win_y, &mask);

			for_each_screen(scr) {
				if (root_x >= scr->x_org && root_x < scr->x_org + scr->width &&
				    root_y >= scr->y_org && root_y < scr->y_org + scr->height) {
					best = scr;
					break;
				}
			}
			break;
		}

		case VWM_SCREEN_REL_TOTAL: {
			short	x1 = MAXSHORT, y1 = MAXSHORT, x2 = MINSHORT, y2 = MINSHORT;
			/* find the smallest x_org and y_org, the highest x_org + width and y_org + height, those are the two corners of the total rect */
			for_each_screen(scr) {
				if (scr->x_org < x1)
					x1 = scr->x_org;

				if (scr->y_org < y1)
					y1 = scr->y_org;

				if (scr->x_org + scr->width > x2)
					x2 = scr->x_org + scr->width;

				if (scr->y_org + scr->height > y2)
					y2 = scr->y_org + scr->height;
			}
			faux.x_org = x1;
			faux.y_org = y1;
			faux.width = x2 - x1;
			faux.height = y2 - y1;
			best = &faux;
			break;
		}
	}
_out:
	VWM_TRACE("Found Screen #%i: %hix%hi @ %hi,%hi", best->screen_number, best->width, best->height, best->x_org, best->y_org);

	return best;
}


/* check if a screen contains any windows (assuming the current desktop) */
int vwm_screen_is_empty(vwm_t *vwm, const vwm_screen_t *scr, vwm_xwindow_t *ignore_xwin)
{
	vwm_xwindow_t	*xwin;
	int		is_empty = 1;

	list_for_each_entry(xwin, &vwm->xwindows, xwindows) {
		if (xwin == ignore_xwin || !xwin->client_mapped)
			continue;

		if (!xwin->managed || (xwin->managed->desktop == vwm->focused_desktop && !xwin->managed->shelved)) {
			/* XXX: it may make more sense to see what %age of the screen is overlapped by windows, and consider it empty if < some % */
			/*      This is just seeing if any window is predominantly within the specified screen, the rationale being if you had a focusable
			 *      window on the screen you would have used the keyboard to make windows go there; this function is only used in determining
			 *      wether a new window should go where the pointer is or not. */
			if (vwm_screen_overlaps_xwin(vwm, scr, xwin) >= 0.05) {
				is_empty = 0;
				break;
			}
		}
	}

	return is_empty;
}
