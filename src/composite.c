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

/* The compositing code is heavily influenced by Keith Packard's xcompmgr.
 */

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include "overlays.h"
#include "xwindow.h"
#include "vwm.h"

	/* compositing manager stuff */
typedef enum _vwm_compositing_mode_t {
	VWM_COMPOSITING_OFF = 0,	/* non-composited, no redirected windows, most efficient */
	VWM_COMPOSITING_MONITORS = 1	/* composited process monitoring overlays, slower but really useful. */
} vwm_compositing_mode_t;

static vwm_compositing_mode_t	compositing_mode = VWM_COMPOSITING_OFF;		/* current compositing mode */
static XserverRegion		combined_damage = None;
static Picture			root_picture = None, root_buffer = None;        /* compositing gets double buffered */
static XWindowAttributes	root_attrs;
static XRenderPictureAttributes	pa_inferiors = { .subwindow_mode = IncludeInferiors };
static int			repaint_needed;

/* bind the window to a "namewindowpixmap" and create a picture from it (compositing) */
static void bind_namewindow(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	xwin->pixmap = XCompositeNameWindowPixmap(VWM_XDISPLAY(vwm), xwin->id);
	xwin->picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), xwin->pixmap,
					     XRenderFindVisualFormat(VWM_XDISPLAY(vwm), xwin->attrs.visual),
					     CPSubwindowMode, &pa_inferiors);
	XFreePixmap(VWM_XDISPLAY(vwm), xwin->pixmap);
}

/* free the window's picture for accessing its redirected contents (compositing) */
static void unbind_namewindow(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	XRenderFreePicture(VWM_XDISPLAY(vwm), xwin->picture);
}

void vwm_composite_xwin_create(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	if (!compositing_mode)
		return;

	bind_namewindow(vwm, xwin);
	xwin->damage = XDamageCreate(VWM_XDISPLAY(vwm), xwin->id, XDamageReportNonEmpty);
}

void vwm_composite_xwin_destroy(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	if (!compositing_mode)
		return;

	unbind_namewindow(vwm, xwin);
	XDamageDestroy(VWM_XDISPLAY(vwm), xwin->damage);
}

/* add damage to the global combined damage queue where we accumulate damage between calls to paint_all() */
void vwm_composite_damage_add(vwm_t *vwm, XserverRegion damage)
{
	if (combined_damage != None) {
		XFixesUnionRegion(VWM_XDISPLAY(vwm), combined_damage, combined_damage, damage);
		/* TODO FIXME: make destroy optional, change callers to reuse a persistent region where practical */
		XFixesDestroyRegion(VWM_XDISPLAY(vwm), damage);
	} else {
		combined_damage = damage;
	}
}

/* helper to damage an entire window including the border */
void vwm_composite_damage_win(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	XserverRegion	region;
	XRectangle	rect = { xwin->attrs.x,
				 xwin->attrs.y,
				 xwin->attrs.width + xwin->attrs.border_width * 2,
				 xwin->attrs.height + xwin->attrs.border_width * 2 };

	if (!compositing_mode)
		return;

	region = XFixesCreateRegion(VWM_XDISPLAY(vwm), &rect, 1);
	vwm_composite_damage_add(vwm, region);
}


void vwm_composite_handle_configure(vwm_t *vwm, vwm_xwindow_t *xwin, XWindowAttributes *new_attrs)
{
	if (!compositing_mode)
		return;

	/* damage the old and new window areas */
	XserverRegion	region;
	XRectangle	rects[2] = {	{	xwin->attrs.x,
						xwin->attrs.y,
						xwin->attrs.width + xwin->attrs.border_width * 2,
						xwin->attrs.height + xwin->attrs.border_width * 2 },
					{	new_attrs->x,
						new_attrs->y,
						new_attrs->width + new_attrs->border_width * 2,
						new_attrs->height + new_attrs->border_width * 2 } };

	region = XFixesCreateRegion(VWM_XDISPLAY(vwm), rects, 2);
	vwm_composite_damage_add(vwm, region);
	unbind_namewindow(vwm, xwin);
	bind_namewindow(vwm, xwin);
}


void vwm_composite_handle_map(vwm_t *vwm, vwm_xwindow_t *xwin)
{
	if (!compositing_mode)
		return;

	vwm_composite_damage_win(vwm, xwin);
	unbind_namewindow(vwm, xwin);
	bind_namewindow(vwm, xwin);
}

/* take what regions of the damaged window have been damaged, subtract them from the per-window damage object, and add them to the combined damage */
void vwm_composite_damage_event(vwm_t *vwm, XDamageNotifyEvent *ev)
{
	XserverRegion	region;
	vwm_xwindow_t	*xwin;

	xwin = vwm_xwin_lookup(vwm, ev->drawable);
	if (!xwin) {
		VWM_ERROR("damaged unknown drawable %x", (unsigned int)ev->drawable);
		return;
	}

	region = XFixesCreateRegion(VWM_XDISPLAY(vwm), NULL, 0);
	XDamageSubtract(VWM_XDISPLAY(vwm), xwin->damage, None, region);
	XFixesTranslateRegion(VWM_XDISPLAY(vwm), region, xwin->attrs.x + xwin->attrs.border_width, xwin->attrs.y + xwin->attrs.border_width);
	vwm_composite_damage_add(vwm, region);
}


/* throw away our double buffered root pictures so they get recreated on the next paint_all() */
/* used in response to screen configuration changes... */
void vwm_composite_invalidate_root(vwm_t *vwm)
{
	if (!compositing_mode)
		return;

	if (root_picture) {
		XRenderFreePicture(VWM_XDISPLAY(vwm), root_picture);
		root_picture = None;
	}

	if (root_buffer) {
		XRenderFreePicture(VWM_XDISPLAY(vwm), root_buffer);
		root_buffer = None;
	}
}

void vwm_composite_repaint_needed(vwm_t *vwm)
{
	if (!compositing_mode)
		return;

	repaint_needed = 1;
}

/* consume combined_damage by iterating the xwindows list from the top down, drawing visible windows as encountered, subtracting their area from combined_damage */
/* when compositing is active this is the sole function responsible for making things show up on the screen */
void vwm_composite_paint_all(vwm_t *vwm)
{
	vwm_xwindow_t		*xwin;
	XRenderColor		bgcolor = {0x0000, 0x00, 0x00, 0xffff};
	Region			occluded;
	static XserverRegion	undamage_region = None;

	/* if there's no damage to repaint, short-circuit, this happens when compositing for overlays is disabled. */
	if (!compositing_mode || (combined_damage == None && !repaint_needed))
		return;

	repaint_needed = 0;

	if (!undamage_region)
		undamage_region = XFixesCreateRegion(VWM_XDISPLAY(vwm), NULL, 0);

	/* (re)create the root picture from the root window and allocate a double buffer for the off-screen composition of the damaged contents */
	if (root_picture == None) {
		Pixmap	root_pixmap;

		XGetWindowAttributes(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), &root_attrs);
		root_picture = XRenderCreatePicture(VWM_XDISPLAY(vwm), VWM_XROOT(vwm),
						    XRenderFindVisualFormat(VWM_XDISPLAY(vwm), VWM_XVISUAL(vwm)),
						    CPSubwindowMode, &pa_inferiors);
		root_pixmap = XCreatePixmap(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), root_attrs.width, root_attrs.height, VWM_XDEPTH(vwm));
		root_buffer = XRenderCreatePicture(VWM_XDISPLAY(vwm), root_pixmap, XRenderFindVisualFormat(VWM_XDISPLAY(vwm), VWM_XVISUAL(vwm)), 0, 0);
		XFreePixmap(VWM_XDISPLAY(vwm), root_pixmap);
	}

	occluded = XCreateRegion();
	/* compose overlays for all visible windows up front in a separate pass (kind of lame, but it's simpler since compose_overlay() adds to combined_damage) */
	list_for_each_entry_prev(xwin, &vwm->xwindows, xwindows) {
		XRectangle	r;

		if (!vwm_xwin_is_mapped(vwm, xwin))
			continue;	/* if !mapped skip */

		/* Everything mapped next goes through an occlusion check.
		 * Since the composite extension stops delivery of VisibilityNotify events for redirected windows,
		 * (it assumes redirected windows should be treated as part of a potentially transparent composite, and provides no api to alter this assumption)
		 * we can't simply select the VisibilityNotify events on all windows and cache their visibility state in vwm_xwindow_t then skip
		 * xwin->state==VisibilityFullyObscured windows here to avoid the cost of pointlessly composing overlays and rendering fully obscured windows :(.
		 *
		 * Instead we accumulate an occluded region (starting empty) of painted windows from the top-down on every paint_all().
		 * Before we compose_overlay() a window, we check if the window's rectangle fits entirely within the occluded region.
		 * If it does, no compose_overlay() is performed.
		 * If it doesn't, compose_overlay() is called, and the window's rect is added to the occluded region.
		 * The occluded knowledge is also cached for the XRenderComposite() loop immediately following, where we skip the rendering of
		 * occluded windows as well.
		 * This does technically break SHAPE windows (xeyes, xmms), but only when monitoring is enabled which covers them with rectangular overlays anyways.
		 */
		r.x = xwin->attrs.x;
		r.y = xwin->attrs.y;
		r.width = xwin->attrs.width + xwin->attrs.border_width * 2;
		r.height = xwin->attrs.height + xwin->attrs.border_width * 2;
		if (XRectInRegion(occluded, r.x, r.y, r.width, r.height) != RectangleIn) {
			/* the window isn't fully occluded, compose it and add it to occluded */
			if (xwin->overlay) {
				XserverRegion	overlay_damage = None;

				vwm_overlay_compose(vwm->overlays, xwin->overlay, &overlay_damage);
				if (overlay_damage != None) {
					/* the damage region is in overlay coordinate space, translation necessary. */
					XFixesTranslateRegion(VWM_XDISPLAY(vwm), overlay_damage,
							xwin->attrs.x + xwin->attrs.border_width,
							xwin->attrs.y + xwin->attrs.border_width);
					vwm_composite_damage_add(vwm, overlay_damage);
				}
			}
			XUnionRectWithRegion(&r, occluded, occluded);
			xwin->occluded = 0;
		} else {
			xwin->occluded = 1;
			VWM_TRACE("window %#x occluded, skipping compose_overlay()", (int)xwin->id);
		}
	}
	XDestroyRegion(occluded);

	/* start with the clip regions set to the damaged area accumulated since the previous paint_all() */
	XFixesSetPictureClipRegion(VWM_XDISPLAY(vwm), root_buffer, 0, 0, combined_damage);	/* this is the double buffer where the in-flight screen contents are staged */
	XFixesSetPictureClipRegion(VWM_XDISPLAY(vwm), root_picture, 0, 0, combined_damage);	/* this is the visible root window */

	/* since translucent windows aren't supported in vwm, I can do this more efficiently */
	list_for_each_entry_prev(xwin, &vwm->xwindows, xwindows) {
		XRectangle	r;

		if (!vwm_xwin_is_mapped(vwm, xwin) || xwin->occluded)
			continue;	/* if !mapped or occluded skip */

		/* these coordinates + dimensions incorporate the border (since XCompositeNameWindowPixmap is being used) */
		r.x = xwin->attrs.x;
		r.y = xwin->attrs.y;
		r.width = xwin->attrs.width + xwin->attrs.border_width * 2;
		r.height = xwin->attrs.height + xwin->attrs.border_width * 2;

		/* render the redirected window contents into root_buffer, note root_buffer has the remaining combined_damage set as the clip region */
		XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, xwin->picture, None, root_buffer,
				 0, 0, 0, 0, /* src x, y, mask x, y */
				 r.x, r.y, /* dst x, y */
				 r.width, r.height);

		if (xwin->overlay) {
			/* draw the monitoring overlay atop the window, note we stay within the window borders here. */
			vwm_overlay_render(vwm->overlays, xwin->overlay, PictOpOver, root_buffer,
					xwin->attrs.x + xwin->attrs.border_width,
					xwin->attrs.y + xwin->attrs.border_width,
					xwin->attrs.width,
					xwin->attrs.height);
		}

		/* subtract the region of the window from the combined damage and update the root_buffer clip region to reflect the remaining damage */
		XFixesSetRegion(VWM_XDISPLAY(vwm), undamage_region, &r, 1);
		XFixesSubtractRegion(VWM_XDISPLAY(vwm), combined_damage, combined_damage, undamage_region);
		XFixesSetPictureClipRegion(VWM_XDISPLAY(vwm), root_buffer, 0, 0, combined_damage);
	}

	/* at this point all of the visible windows have been subtracted from the clip region, so paint any root window damage (draw background) */
	XRenderFillRectangle(VWM_XDISPLAY(vwm), PictOpSrc, root_buffer, &bgcolor, 0, 0, root_attrs.width, root_attrs.height);

	/* discard the root_buffer clip region and copy root_buffer to root_picture, root_picture still has the combined damage as its clip region */
	XFixesSetPictureClipRegion(VWM_XDISPLAY(vwm), root_buffer, 0, 0, None);
	XRenderComposite(VWM_XDISPLAY(vwm), PictOpSrc, root_buffer, None, root_picture, 0, 0, 0, 0, 0, 0, root_attrs.width, root_attrs.height);

	/* fin */
	XFixesDestroyRegion(VWM_XDISPLAY(vwm), combined_damage);
	combined_damage = None;
	XSync(VWM_XDISPLAY(vwm), False);
}


/* toggle compositing/monitoring overlays on/off */
void vwm_composite_toggle(vwm_t *vwm)
{
	vwm_xwindow_t	*xwin;

	XGrabServer(VWM_XDISPLAY(vwm));
	XSync(VWM_XDISPLAY(vwm), False);

	switch (compositing_mode) {
		case VWM_COMPOSITING_OFF:
			VWM_TRACE("enabling compositing");
			compositing_mode = VWM_COMPOSITING_MONITORS;
			XCompositeRedirectSubwindows(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), CompositeRedirectManual);
			list_for_each_entry_prev(xwin, &vwm->xwindows, xwindows) {
				bind_namewindow(vwm, xwin);
				xwin->damage = XDamageCreate(VWM_XDISPLAY(vwm), xwin->id, XDamageReportNonEmpty);
			}
			/* damage everything */
			/* TODO: keep a list of rects reflecting all the current screens and create a region from that... */
			vwm_composite_damage_add(vwm, XFixesCreateRegionFromWindow(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), WindowRegionBounding));
			break;

		case VWM_COMPOSITING_MONITORS: {
			XEvent	ev;

			VWM_TRACE("disabling compositing");
			compositing_mode = VWM_COMPOSITING_OFF;
			list_for_each_entry_prev(xwin, &vwm->xwindows, xwindows) {
				unbind_namewindow(vwm, xwin);
				XDamageDestroy(VWM_XDISPLAY(vwm), xwin->damage);
			}
			XCompositeUnredirectSubwindows(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), CompositeRedirectManual);
			vwm_composite_invalidate_root(vwm);

			/* if there's any damage queued up discard it so we don't enter paint_all() until compositing is reenabled again. */
			if (combined_damage) {
				XFixesDestroyRegion(VWM_XDISPLAY(vwm), combined_damage);
				combined_damage = None;
			}
			while (XCheckTypedEvent(VWM_XDISPLAY(vwm), vwm->damage_event + XDamageNotify, &ev) != False);
			break;
		}
	}

	XUngrabServer(VWM_XDISPLAY(vwm));
}
