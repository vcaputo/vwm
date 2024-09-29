/*
 *                                  \/\/\
 *
 *  Copyright (C) 2024 Vito Caputo - <vcaputo@pengaru.com>
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

/* vcr = vwm charts rendering (api)
 *
 * This exists to decouple the rendering needs of charts.c/vmon.c from Xlib for
 * enabling headless use of vmon in embedded/server circumstances.
 *
 * Rather than exploring serverless Xlib implementations which render
 * in-process, embracing something like cairo, or worse - creating a new
 * generic rendering library, a very targeted approach has been taken here
 * catering specifically to the requirements of charts.c.
 *
 * The intention is to enable higher density in-memory representation of the
 * chart layers especially relevant to embedded situations.  The existing Xlib
 * Render based charts usage utilizes several full-color full-size Picture
 * objects for the various layers used to compose the charts.
 *
 * The vcr api should implement layers/planes for a common underlying object as
 * a first-class entity.  When the vcr object is X-backed, those layers may be
 * Pictures just like previously.  But when the vcr object is headless, those
 * layers are free to be represented in whatever packed format makes the most
 * sense, without consideration for real-time rendering efficiency.
 *
 * Imagine for instance if headless vcr allocated a single byte array
 * dimensioned by the chart's dimensions, to represent eight layers in the bit
 * planes of the bytes.  This exploits the fact that chart layers contain
 * monochromatic coverage information for the pixel.  Turning these layers into
 * color renderings could use a simple palette lookup to produce the
 * appropriate blended colors given the combination of bits set in the various
 * layers.
 *
 * So every layer would have a color associated with it, used when compositing
 * the rendered chart from the layers.  But the actual layer maintenance would
 * simply be setting/unsetting the appropriate bit in the affected pixel
 * positions.
 */

#include <assert.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef USE_XLIB
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include "xserver.h"
#endif /* USE_XLIB */

#ifdef USE_PNG
#include <png.h>
#endif /* USE_PNG */

#include "ascii.h"
#include "util.h"
#include "vcr.h"

/* backend is the root vcr object everything else here derives from,
 * for an X backend it encompasses the xserver/display connection.
 */
typedef struct vcr_backend_t {
	vcr_backend_type_t	type;

	union {
#ifdef USE_XLIB
		struct {
			vwm_xserver_t		*xserver;

			unsigned		xserver_created:1;
			Atom			wm_protocols_atom;
			Atom			wm_delete_atom;

			/* X stuff needed for doing all the vwm_charts_t things
			 * (these once resided in vwm_charts_t)
			 */
			XFontStruct		*chart_font;
			GC			text_gc;
			Picture			shadow_fill,
						text_fill,
						bg_fill,
						snowflakes_text_fill,
						grapha_fill,
						graphb_fill,
						finish_fill;
		} xlib;
#endif /* USE_XLIB */
		struct {
			/* TODO */
		} mem;
	};
} vcr_backend_t;


/* vcr is the per-chart object you can present to a dest object,
 * it is tightly bound to backend type and shares the vcr_backend_type_t.
 *
 * where the backend encompasses a bunch of backend-global state applicable to all
 * charts like the GC / "fill" picture sources etc, this object encompasses the chart-specific
 * state like graph layer/shadow/text pictures etc.
 */
typedef struct vcr_t {
	vcr_backend_t	*backend;

	int		width;				/* current width of the chart */
	int		height;				/* current height of the chart */
	int		visible_width;			/* currently visible width of the chart */
	int		visible_height;			/* currently visible height of the chart */
	int		phase;				/* current position within the (horizontally scrolling) graphs */

	/* these pointers point into variables within the chart_t because they're primarily maintained by
	 * the chart renderer, but we need to access them occasionally here.. it's a bit gross.
	 */
	int		*hierarchy_end_ptr;		/* pointer to row where the process hierarchy currently ends */
	int		*snowflakes_cnt_ptr;		/* pointer to count of snowflaked rows (reset to zero to truncate snowflakes display) */

	union {
#ifdef USE_XLIB
		struct {
			Pixmap		text_pixmap;	/* pixmap for charted text (kept around for XDrawText usage) */
			Picture		text_picture;	/* picture representation of text_pixmap */
			Picture		shadow_picture;	/* text shadow layer */
			Picture		grapha_picture;	/* graph A layer */
			Picture		graphb_picture;	/* graph B layer */
			Picture		tmp_a_picture;	/* 1 row worth of temporary graph A space */
			Picture		tmp_b_picture;	/* 1 row worth of temporary graph B space */
			Picture		picture;	/* chart picture derived from the pixmap, for render compositing */
		} xlib;
#endif /* USE_XLIB */
		struct {
			uint8_t	*bits;	/* .pitch * height bytes are used to represent the coverage status of up to 4 layers (was 8 until nibbles happened) */
			uint8_t	*tmp;	/* .pitch * VCR_ROW_HEIGHT bytes for a row's worth of temporary storage */
			int	pitch;	/* "pitch" of mem surface in bytes, which is half the width rounded up to an even number divisible by two. */
		} mem;
	};
} vcr_t;


/* dest represents an output destination for rendering/compositing vcr instances at.
 * for an vmon-on-X scenario, it encompasses the viewable X Window + Picture of of vmon.
 * for an headless vmon-to-PNGs scenario, it encompasses the PNG writer.
 * for an vwm-on-X scenario, it encompasses the Picture associated with a vwm_window_t.
 *
 * it should also be possible to do things like render a vcr instance created from an xlib backend
 * to something like a PNG dest.
 */
typedef enum vcr_dest_type_t {
#ifdef USE_XLIB
	VCR_DEST_TYPE_XWINDOW,
	VCR_DEST_TYPE_XPICTURE,
#endif /* USE_XLIB */
	VCR_DEST_TYPE_PNG
} vcr_dest_type_t;


typedef struct vcr_dest_t {
	vcr_backend_t	*backend;
	vcr_dest_type_t	type;

	union {
#ifdef USE_XLIB
		struct {
			/* vmon use case; xwindow dest maps to vmon's X window, no compositing */
			Window		window;
			Picture		picture;
		} xwindow;

		struct {
			/* vwm use case; xpicture dest maps to composited X root window */
			Picture		picture;
		} xpicture;
#endif /* USE_XLIB */
#ifdef USE_PNG
		struct {
			/* vmon use case; png dest is for persisting snapshots of the vcr state */
			/* this could actually apply to vwm too which would be useful for hot-key based
			 * snapshotting of a focused window's monitoring overlays.
			 */
			png_infop	info_ctx;
			png_structp	png_ctx;
			FILE		*output;
		} png;
#endif /* USE_PNG */
	};
} vcr_dest_t;


#ifdef USE_XLIB
#define CHART_GRAPH_MIN_WIDTH	200					/* always create graphs at least this large */
#define CHART_GRAPH_MIN_HEIGHT	(4 * VCR_ROW_HEIGHT)
#define CHART_MASK_DEPTH	8					/* XXX: 1 would save memory, but Xorg isn't good at it */
#define CHART_FIXED_FONT	"-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso10646-1"

static XRenderColor		chart_visible_color = { 0xffff, 0xffff, 0xffff, 0xffff },
				chart_shadow_color = { 0x0000, 0x0000, 0x0000, 0xC000},
				chart_bg_color = { 0x0, 0x1000, 0x0, 0x9000},
				chart_div_color = { 0x2000, 0x3000, 0x2000, 0x9000},
				chart_snowflakes_visible_color = { 0xd000, 0xd000, 0xd000, 0x8000 },
				chart_trans_color = {0x00, 0x00, 0x00, 0x00},
				chart_grapha_color = { 0xff00, 0x0000, 0x0000, 0x3000 },	/* ~red */
				chart_graphb_color = { 0x0000, 0xffff, 0xffff, 0x3000 };	/* ~cyan */

static XRenderPictureAttributes	pa_repeat = { .repeat = 1 };
static XRenderPictureAttributes	pa_no_repeat = { .repeat = 0 };


/* convenience helper for creating a pixmap */
static Pixmap create_pixmap(vwm_xserver_t *xserver, unsigned width, unsigned height, unsigned depth)
{
	return XCreatePixmap(xserver->display, XSERVER_XROOT(xserver), width, height, depth);
}


/* convenience helper for creating a picture, supply res_pixmap to keep a reference to the pixmap drawable. */
static Picture create_picture(vwm_xserver_t *xserver, unsigned width, unsigned height, unsigned depth, unsigned long attr_mask, XRenderPictureAttributes *attr, Pixmap *res_pixmap)
{
	Pixmap		pixmap;
	Picture		picture;
	int		format;

	/* FIXME this pixmap->picture dance seems silly, investigate further. TODO */
	switch (depth) {
		case 8:
			format = PictStandardA8;
			break;
		case 32:
			format = PictStandardARGB32;
			break;
		default:
			assert(0);
	}

	pixmap = create_pixmap(xserver, width, height, depth);
	picture = XRenderCreatePicture(xserver->display, pixmap, XRenderFindStandardFormat(xserver->display, format), attr_mask, attr);

	if (res_pixmap) {
		*res_pixmap = pixmap;
	} else {
		XFreePixmap(xserver->display, pixmap);
	}

	return picture;
}


/* convenience helper for creating a filled picture, supply res_pixmap to keep a reference to the pixmap drawable. */
static Picture create_picture_fill(vwm_xserver_t *xserver, unsigned width, unsigned height, unsigned depth, unsigned long attrs_mask, XRenderPictureAttributes *attrs, const XRenderColor *color, Pixmap *res_pixmap)
{
	Picture		picture;

	picture = create_picture(xserver, width, height, depth, attrs_mask, attrs, res_pixmap);
	XRenderFillRectangle(xserver->display, PictOpSrc, picture, color, 0, 0, width, height);

	return picture;
}


/* returns NULL on failure, freeing vcr
 * returns vcr on success, fully setup for the given backend.
 */
static vcr_backend_t * vcr_backend_xlib_setup(vcr_backend_t *vbe, vwm_xserver_t *xserver)
{
	Pixmap	bitmask;

	assert(vbe);

	vbe->type = VCR_BACKEND_TYPE_XLIB;

	if (!xserver) {
		/* we'll connect to the xserver if none is provided */
		xserver = vwm_xserver_open();
		if (!xserver) {
			VWM_ERROR("unable to open xserver");
			goto err_vbe;
		}
		vbe->xlib.xserver_created = 1;
	}
	vbe->xlib.xserver = xserver;

	/* this is really only needed for the xwindow-dest/vmon scenario (where we create the xserver),
	 * but let's just always grab the atoms anyways
	 */
	vbe->xlib.wm_delete_atom = XInternAtom(xserver->display, "WM_DELETE_WINDOW", False);
	vbe->xlib.wm_protocols_atom = XInternAtom(xserver->display, "WM_PROTOCOLS", False);

	/* get all the text and graphics stuff setup for charts,
	 * this all used to be part of vwm_charts_create(), but
	 * moved here as charts.c/vmon.c became X-decoupled and
	 * relied on vcr.c to abstract the X/mem/png specifics
	 * on the road to headless vmon support.
	 */
	vbe->xlib.chart_font = XLoadQueryFont(xserver->display, CHART_FIXED_FONT);
	if (!vbe->xlib.chart_font) {
		VWM_ERROR("unable to load chart font \"%s\"", CHART_FIXED_FONT);
		goto err_vbe;
	}

	/* FIXME: error handling for all this junk */
	/* create a GC for rendering the text using Xlib into the text chart stencils */
	bitmask = create_pixmap(xserver, 1, 1, CHART_MASK_DEPTH);
	vbe->xlib.text_gc = XCreateGC(xserver->display, bitmask, 0, NULL);
	XSetForeground(xserver->display, vbe->xlib.text_gc, WhitePixel(xserver->display, xserver->screen_num));
	XFreePixmap(xserver->display, bitmask);

	/* create some repeating source fill pictures for drawing through the text and graph stencils */
	vbe->xlib.text_fill = create_picture_fill(xserver, 1, 1, 32, CPRepeat, &pa_repeat, &chart_visible_color, NULL);
	vbe->xlib.shadow_fill = create_picture_fill(xserver, 1, 1, 32, CPRepeat, &pa_repeat, &chart_shadow_color, NULL);

	vbe->xlib.bg_fill = create_picture(xserver, 1, VCR_ROW_HEIGHT, 32, CPRepeat, &pa_repeat, NULL);
	XRenderFillRectangle(xserver->display, PictOpSrc, vbe->xlib.bg_fill, &chart_bg_color, 0, 0, 1, VCR_ROW_HEIGHT);
	XRenderFillRectangle(xserver->display, PictOpSrc, vbe->xlib.bg_fill, &chart_div_color, 0, VCR_ROW_HEIGHT - 1, 1, 1);

	vbe->xlib.snowflakes_text_fill = create_picture_fill(xserver, 1, 1, 32, CPRepeat, &pa_repeat, &chart_snowflakes_visible_color, NULL);
	vbe->xlib.grapha_fill = create_picture_fill(xserver, 1, 1, 32, CPRepeat, &pa_repeat, &chart_grapha_color, NULL);
	vbe->xlib.graphb_fill = create_picture_fill(xserver, 1, 1, 32, CPRepeat, &pa_repeat, &chart_graphb_color, NULL);

	vbe->xlib.finish_fill = create_picture(xserver, 1, 2, 32, CPRepeat, &pa_repeat, NULL);
	XRenderFillRectangle(xserver->display, PictOpSrc, vbe->xlib.finish_fill, &chart_visible_color, 0, 0, 1, 1);
	XRenderFillRectangle(xserver->display, PictOpSrc, vbe->xlib.finish_fill, &chart_trans_color, 0, 1, 1, 1);

	return vbe;

err_vbe:
	if (vbe->xlib.xserver_created)
		vwm_xserver_close(vbe->xlib.xserver);

	free(vbe);

	return NULL;
}
#endif /* USE_XLIB */


vcr_backend_t * vcr_backend_new(vcr_backend_type_t backend, ...)
{
	vcr_backend_t	*vbe;
	va_list		ap;

	vbe = calloc(1, sizeof(vcr_backend_t));
	if (!vbe)
		return NULL;

	va_start(ap, backend);
	switch (backend) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB:
		vbe = vcr_backend_xlib_setup(vbe, va_arg(ap, vwm_xserver_t *));
		break;
#endif /* USE_XLIB */
	case VCR_BACKEND_TYPE_MEM:
		vbe->type = VCR_BACKEND_TYPE_MEM;
		break;
	default:
		assert(0);
	}
	va_end(ap);

	return vbe;
}


/* returns the native dimensions of the backend, really only applies to xlib currently for fullscreen dims */
int vcr_backend_get_dimensions(vcr_backend_t *vbe, int *res_width, int *res_height)
{
	assert(vbe);
	assert(res_width);
	assert(res_height);

	switch (vbe->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		XWindowAttributes	wattr;

		if (!XGetWindowAttributes(vbe->xlib.xserver->display, XSERVER_XROOT(vbe->xlib.xserver), &wattr)) {
			return -ENOENT;
		}

		*res_width = wattr.width;
		*res_height = wattr.height;

		return 0;
	}
#endif

	case VCR_BACKEND_TYPE_MEM:
		return -ENOTSUP;

	default:
		assert(0);
	}
}


/* this is basically just needed by the vmon use case */
/* returns 1 if the backend has events to process, 0 on timeout/error. */
int vcr_backend_poll(vcr_backend_t *vbe, int timeout_us)
{
	/* FIXME TODO: should probably switch to ppoll() and keep signals blocked outside of
	 * while blocked in ppoll().  Then ppoll() becomes our signal delivery/handling point...
	 */
	switch (vbe->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		struct pollfd	pfd = {
					.events = POLLIN,
					.fd = ConnectionNumber(vbe->xlib.xserver->display),
				};

		if (XPending(vbe->xlib.xserver->display))
			return 1;

		return poll(&pfd, 1, timeout_us);
	}
#endif
	case VCR_BACKEND_TYPE_MEM:
		return poll(NULL, 0, timeout_us);

	default:
		assert(0);
	}
}


/* this is basically just needed by the vmon use case, called after
 * vcr_backend_poll() returns 1.
 * if VCR_BACKEND_EVENT_RESIZE is returned, res_width and res_height will be updated.
 */
vcr_backend_event_t vcr_backend_next_event(vcr_backend_t *vbe, int *res_width, int *res_height)
{
	assert(vbe);
	assert(res_width);
	assert(res_height);

	switch (vbe->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		XEvent	ev;

		XNextEvent(vbe->xlib.xserver->display, &ev);

		switch (ev.type) {
		case ConfigureNotify:
			*res_width = ev.xconfigure.width;
			*res_height = ev.xconfigure.height;
			return VCR_BACKEND_EVENT_RESIZE;

		case Expose:
			return VCR_BACKEND_EVENT_REDRAW;

		case ClientMessage:
			if (ev.xclient.message_type != vbe->xlib.wm_protocols_atom)
				break;

			if (ev.xclient.data.l[0] != vbe->xlib.wm_delete_atom)
				break;

			return VCR_BACKEND_EVENT_QUIT;
		}
		break;
	}
#endif

	case VCR_BACKEND_TYPE_MEM:
		break;

	default:
		assert(0);
	}

	return VCR_BACKEND_EVENT_NOOP;
}


vcr_backend_t * vcr_backend_free(vcr_backend_t *vbe)
{
	if (vbe) {
		switch (vbe->type) {
#ifdef USE_XLIB
		case VCR_BACKEND_TYPE_XLIB:
			XRenderFreePicture(vbe->xlib.xserver->display, vbe->xlib.shadow_fill);
			XRenderFreePicture(vbe->xlib.xserver->display, vbe->xlib.text_fill);
			XRenderFreePicture(vbe->xlib.xserver->display, vbe->xlib.bg_fill);
			XRenderFreePicture(vbe->xlib.xserver->display, vbe->xlib.snowflakes_text_fill);
			XRenderFreePicture(vbe->xlib.xserver->display, vbe->xlib.grapha_fill);
			XRenderFreePicture(vbe->xlib.xserver->display, vbe->xlib.graphb_fill);
			XRenderFreePicture(vbe->xlib.xserver->display, vbe->xlib.finish_fill);
			XFreeFont(vbe->xlib.xserver->display, vbe->xlib.chart_font);
			XFreeGC(vbe->xlib.xserver->display, vbe->xlib.text_gc);

			if (vbe->xlib.xserver_created)
				vwm_xserver_close(vbe->xlib.xserver);
			break;
#endif /* USE_XLIB */
		case VCR_BACKEND_TYPE_MEM:
			break;

		default:
			assert(0);
		}

		free(vbe);
	}

	return NULL;
}


#ifdef USE_XLIB
/* for the vmon use case, we need a window destination created */
vcr_dest_t * vcr_dest_xwindow_new(vcr_backend_t *vbe, const char *name, unsigned width, unsigned height)
{
	XWindowAttributes		wattr = {};
	XRenderPictureAttributes	pattr = {};
	vwm_xserver_t			*xserver;
	vcr_dest_t			*dest;

	assert(vbe);
	assert(vbe->type == VCR_BACKEND_TYPE_XLIB);
	assert(width > 0);
	assert(height > 0);

	xserver = vbe->xlib.xserver;

	dest = calloc(1, sizeof(vcr_dest_t));
	if (!dest)
		return NULL;

	dest->type = VCR_DEST_TYPE_XWINDOW;
	dest->backend = vbe;

	dest->xwindow.window = XCreateSimpleWindow(xserver->display, XSERVER_XROOT(xserver), 0, 0, width, height, 1, 0, 0);
	if (name)
		XStoreName(xserver->display, dest->xwindow.window, name);
	XGetWindowAttributes(xserver->display, dest->xwindow.window, &wattr);
	dest->xwindow.picture = XRenderCreatePicture(xserver->display, dest->xwindow.window, XRenderFindVisualFormat(xserver->display, wattr.visual), 0, &pattr);
	XMapWindow(xserver->display, dest->xwindow.window);
	XSelectInput(xserver->display, dest->xwindow.window, StructureNotifyMask|ExposureMask);
	XSync(xserver->display, False);

	return dest;
}


/* accessor to get the Window id out of the dest */
unsigned vcr_dest_xwindow_get_id(vcr_dest_t *dest)
{
	assert(dest);
	assert(dest->type == VCR_DEST_TYPE_XWINDOW);

	return dest->xwindow.window;
}


vcr_dest_t * vcr_dest_xpicture_new(vcr_backend_t *vbe, Picture picture)
{
	vcr_dest_t	*dest;

	assert(vbe);
	assert(vbe->type == VCR_BACKEND_TYPE_XLIB);
	assert(picture != None);

	dest = calloc(1, sizeof(vcr_dest_t));
	if (!dest)
		return NULL;

	dest->type = VCR_DEST_TYPE_XPICTURE;
	dest->backend = vbe;
	dest->xpicture.picture = picture;

	return dest;
}
#endif /* USE_XLIB */


#ifdef USE_PNG
vcr_dest_t * vcr_dest_png_new(vcr_backend_t *vbe, FILE *output)
{
	vcr_dest_t	*dest;

	assert(vbe);
	assert(output != NULL);

	/* for png dest we just have to make sure vbe->type is one
	 * we can handle presenting from... the png dest doesn't
	 * actually have to derive any resources from the backend,
	 * unlike the xwindow dest which actually has to create a
	 * window etc.
	 */

	dest = calloc(1, sizeof(vcr_dest_t));
	if (!dest)
		return NULL;

	dest->type = VCR_DEST_TYPE_PNG;
	dest->png.output = output;

	dest->png.png_ctx = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!dest->png.png_ctx) {
		free(dest);
		return NULL;
	}

	dest->png.info_ctx = png_create_info_struct(dest->png.png_ctx);
	if (!dest->png.info_ctx) {
		png_destroy_write_struct(&dest->png.png_ctx, NULL);
		free(dest);
		return NULL;
	}

	png_init_io(dest->png.png_ctx, output);

	return dest;
}
#endif /* USE_PNG */


vcr_dest_t * vcr_dest_free(vcr_dest_t *dest)
{
	if (dest) {
		switch (dest->type) {
#ifdef USE_XLIB
		case VCR_DEST_TYPE_XWINDOW:
			XDestroyWindow(dest->backend->xlib.xserver->display, dest->xwindow.window);
			XRenderFreePicture(dest->backend->xlib.xserver->display, dest->xwindow.picture);
			break;
		case VCR_DEST_TYPE_XPICTURE:
			XRenderFreePicture(dest->backend->xlib.xserver->display, dest->xpicture.picture);
			break;
#endif /* USE_XLIB */
#ifdef USE_PNG
		case VCR_DEST_TYPE_PNG:
			/* XXX: we don't take ownership of the FILE* @ dest->png.output, but
			 * that could change.  The thinking being the caller provided the
			 * FILE* pre-opened, and it could very well be something like stdout,
			 * which we might not want to close immediately after writing the png
			 * to it.  But maybe it'd be better to just take ownership of it.
			 */
			png_destroy_write_struct(&dest->png.png_ctx, &dest->png.info_ctx);
			break;
#endif /* USE_PNG */
		default:
			assert(0);
		}

		free(dest);
	}

	return NULL;
}


/* vcr is the workhorse of doing the actual chart compositing/rendering using a given backend.
 *
 * The vcr object encapsulates a chart instance and the state of all its layers, in whatever form
 * is appropriate for the backend it's derived from.
 *
 * In the xlib backend case, that closely resembles what the OG charts.c X-coupled implementation
 * did, just shoved behind the vcr api.  This results in efficient vcr_present() to X dests for
 * real-time usage.
 *
 * In the mem backend case, X types are not used at all, and an ad-hoc packed byte array is used to
 * represent the various chart layers as bit planes in the interests of saving memory.  This makes for
 * slower manipulation and compositing, and a slower present, but is intended for more embedded headless
 * uses where the priority is more lower frequency (1HZ) and more history (larger dimensions) with periodic
 * PNG presents on the order of minutes/hours for cloud uploading to facilitate investigations.
 */
vcr_t * vcr_new(vcr_backend_t *vbe, int *hierarchy_end_ptr, int *snowflakes_cnt_ptr)
{
	vcr_t	*vcr;

	assert(vbe);
	assert(hierarchy_end_ptr);
	assert(snowflakes_cnt_ptr);

	vcr = calloc(1, sizeof(vcr_t));
	if (!vcr)
		return NULL;

	vcr->backend = vbe;
	vcr->hierarchy_end_ptr = hierarchy_end_ptr;
	vcr->snowflakes_cnt_ptr = snowflakes_cnt_ptr;

	return vcr;
}


#ifdef USE_XLIB
/* helper for _only_ freeing the xlib internal stuff embedded within the vcr,
 * split out because resizes need to throw this stuff away after copying to the
 * newly allocated instances.
 */
static void vcr_free_xlib_internal(vcr_t *vcr)
{
	vwm_xserver_t	*xserver;

	assert(vcr);
	assert(vcr->backend);
	assert(vcr->backend->type == VCR_BACKEND_TYPE_XLIB);

	xserver = vcr->backend->xlib.xserver;

	assert(xserver);

	XRenderFreePicture(xserver->display, vcr->xlib.grapha_picture);
	XRenderFreePicture(xserver->display, vcr->xlib.graphb_picture);
	XRenderFreePicture(xserver->display, vcr->xlib.tmp_a_picture);
	XRenderFreePicture(xserver->display, vcr->xlib.tmp_b_picture);
	XRenderFreePicture(xserver->display, vcr->xlib.text_picture);
	XFreePixmap(xserver->display, vcr->xlib.text_pixmap);
	XRenderFreePicture(xserver->display, vcr->xlib.shadow_picture);
	XRenderFreePicture(xserver->display, vcr->xlib.picture);
}


/* helper for _only_ copying the xlib internal stuff embedded in the vcr,
 * for resizing purposes.
 */
static void vcr_copy_xlib_internal(vcr_t *src, vcr_t *dest)
{
	vwm_xserver_t	*xserver;

	assert(src);
	assert(src->backend);
	assert(src->backend->type == VCR_BACKEND_TYPE_XLIB);
	assert(dest);
	assert(dest->backend);
	assert(dest->backend->type == VCR_BACKEND_TYPE_XLIB);
	assert(src->backend->xlib.xserver == dest->backend->xlib.xserver);

	xserver = src->backend->xlib.xserver;

	/* XXX: note the graph pictures are copied from their current phase in the x dimension */
	XRenderComposite(xserver->display, PictOpSrc, src->xlib.grapha_picture, None, dest->xlib.grapha_picture,
		src->phase, 0,		/* src x, y */
		0, 0,			/* mask x, y */
		dest->phase, 0,		/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->xlib.graphb_picture, None, dest->xlib.graphb_picture,
		src->phase, 0,		/* src x, y */
		0, 0,			/* mask x, y */
		dest->phase, 0,		/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->xlib.text_picture, None, dest->xlib.text_picture,
		0, 0,			/* src x, y */
		0, 0,			/* mask x, y */
		0, 0,			/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->xlib.shadow_picture, None, dest->xlib.shadow_picture,
		0, 0,			/* src x, y */
		0, 0,			/* mask x, y */
		0, 0,			/* dest x, y */
		src->width, src->height);
	XRenderComposite(xserver->display, PictOpSrc, src->xlib.picture, None, dest->xlib.picture,
		0, 0,			/* src x, y */
		0, 0,			/* mask x, y */
		0, 0,			/* dest x, y */
		src->width, src->height);
}
#endif /* USE_XLIB */


vcr_t * vcr_free(vcr_t *vcr)
{
	if (vcr) {
		assert(vcr->backend);

		switch (vcr->backend->type) {
#ifdef USE_XLIB
		case VCR_BACKEND_TYPE_XLIB:
			vcr_free_xlib_internal(vcr);
			break;
#endif /* USE_XLIB */
		case VCR_BACKEND_TYPE_MEM:
			free(vcr->mem.bits);
			free(vcr->mem.tmp);
			break;

		default:
			assert(0);
		}

		free(vcr);
	}

	return NULL;
}


/* resize the specified vcr's visible dimensions, which may or may not require actual
 * resizing of the underlying backend resources.
 *
 * -errno is returned on failure (will generally be -ENOMEM), 0 returned on success with no redraw needed,
 * 1 returned on success with redraw needed.
 */
int vcr_resize_visible(vcr_t *vcr, int width, int height)
{
	assert(vcr);
	assert(width > 0);
	assert(height > 0);

	/* nothing to do */
	if (width == vcr->visible_width && height == vcr->visible_height)
		return 0; /* no redraw needed */

	if (width <= vcr->width && height <= vcr->height) {
		/* we've stayed within the current allocation, no need to involve the backend */
		vcr->visible_width = width;
		vcr->visible_height = height;
		/* you may be wondering how this can happen - when windows get resized smaller, we don't
		 * shrink the resources backing the vcr... they only grow.  The shrinking just affects
		 * the visible_{height,width} dimensions, it doesn't resize the backend smaller.  So
		 * when/if they grow again in visibility, it's just a matter of adjusting the visible
		 * dimensions, unless they exceed the maximum dimensions... which requires allocation.
		 */

		return 1; /* redraw needed */
	}

	/* we're going outside the current allocation dimensions, so we need to involve the backend
	 * in _really_ resizing.
	 */
	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		vcr_t		existing = *vcr;	/* stow the current vcr's contents for copy+free */
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		vcr->width = MAX(vcr->width, MAX(width, CHART_GRAPH_MIN_WIDTH));
		vcr->height = MAX(vcr->height, MAX(height, CHART_GRAPH_MIN_HEIGHT));

		/* XXX: note this is actually _the_ place these things get allocated */
		vcr->xlib.grapha_picture = create_picture_fill(xserver, vcr->width, vcr->height, CHART_MASK_DEPTH, CPRepeat, &pa_repeat, &chart_trans_color, NULL);
		vcr->xlib.graphb_picture = create_picture_fill(xserver, vcr->width, vcr->height, CHART_MASK_DEPTH, CPRepeat, &pa_repeat, &chart_trans_color, NULL);
		vcr->xlib.tmp_a_picture = create_picture(xserver, vcr->width, VCR_ROW_HEIGHT, CHART_MASK_DEPTH, 0, NULL, NULL);
		vcr->xlib.tmp_b_picture = create_picture(xserver, vcr->width, VCR_ROW_HEIGHT, CHART_MASK_DEPTH, 0, NULL, NULL);

		/* keep the text_pixmap reference around for XDrawText usage */
		vcr->xlib.text_picture = create_picture_fill(xserver, vcr->width, vcr->height, CHART_MASK_DEPTH, 0, NULL, &chart_trans_color, &vcr->xlib.text_pixmap);

		vcr->xlib.shadow_picture = create_picture_fill(xserver, vcr->width, vcr->height, CHART_MASK_DEPTH, 0, NULL, &chart_trans_color, NULL);
		vcr->xlib.picture = create_picture(xserver, vcr->width, vcr->height, 32, 0, NULL, NULL);

		if (existing.width) {
			vcr_copy_xlib_internal(&existing, vcr);
			vcr_free_xlib_internal(&existing);
		}
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		int	pitch = (width + 1) >> 1;

		/* no attempt to preserve the existing contents is done for the mem backend,
		 * as it's intended for a non-interactive headless use case - there is no
		 * resizing @ runtime.  We get entered once to create the initial dimensions,
		 * then never recurs.
		 */
		assert(!vcr->mem.bits); /* since we're assuming this doesn't recur, assert it */
		vcr->mem.bits = calloc(pitch * height, sizeof(uint8_t));
		if (!vcr->mem.bits)
			return -ENOMEM;

		assert(!vcr->mem.tmp); /* since we're assuming this doesn't recur, assert it */
		vcr->mem.tmp = calloc(pitch * VCR_ROW_HEIGHT, sizeof(uint8_t));
		if (!vcr->mem.tmp) {
			free(vcr->mem.bits);
			return -ENOMEM;
		}

		vcr->mem.pitch = pitch;
		vcr->width = width;
		vcr->height = height;

		break;
	}

	default:
		assert(0);
	}

	vcr->visible_width = width;
	vcr->visible_height = height;

	assert(vcr->width >= vcr->visible_width);
	assert(vcr->height >= vcr->visible_height);

	return 0;
}


/* this is inspired by XDrawText and its XTextItem *items + nr_items API,
 * primarily so it's easy to map the incoming call to an XDrawText call...
 * but for non-xlib backends, an XDrawText equivalent will be needed.
 *
 * note the formatting and font-switching aspects of XTextItem have not
 * been exposed here...  this just takes an array of char* strings and
 * turns it into an XTextItem array etc.
 *
 * this draws to the specified vcr layer.
 *
 * supply a res_width to get the rendered text width in pixels
 *
 * supply a negative row to suppress the actual drawing, but still get the would-be res_width
 *
 * x may be negative or extend outside vcr bounds, clipping will be performed as needed.
 */
 /* XXX: maybe these strs should also include lengths instead of being null-terminated */
void vcr_draw_text(vcr_t *vcr, vcr_layer_t layer, int x, int row, const vcr_str_t *strs, int n_strs, int *res_width)
{
	assert(vcr);
	assert(vcr->backend);
	assert(layer >= 0 && layer < VCR_LAYER_CNT);
	assert(row >= 0 || res_width);
	assert(strs);
	assert(n_strs > 0);
	/* FIXME: this should really be able to draw text into any valid layer,
	 * it's just the pictures/pixmaps in vcr_t aren't currently organized as an
	 * array easily indexed by the layer enum. TODO
	 */
	assert(layer == VCR_LAYER_TEXT);

	if (n_strs > VCR_DRAW_TEXT_N_STRS_MAX)
		n_strs = VCR_DRAW_TEXT_N_STRS_MAX;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		XTextItem	items[VCR_DRAW_TEXT_N_STRS_MAX];

		for (int i = 0; i < n_strs; i++) {
			items[i].nchars = strs[i].len;
			items[i].chars = (char *)strs[i].str;
			items[i].delta = 4;
			items[i].font = None;
		}

		if (row >= 0) {
			XDrawText(vcr->backend->xlib.xserver->display, vcr->xlib.text_pixmap, vcr->backend->xlib.text_gc,
				x, (row + 1) * VCR_ROW_HEIGHT - 3,	/* dst x, y */
				items, n_strs);
		}

		/* if the caller wants to know the width, compute it, it's dumb that XDrawText doesn't
		 * return the dimensions of what was drawn, fucking xlib.
		 */
		if (res_width) {
			int	width = 0;

			for (int i = 0; i < n_strs; i++)
				width += XTextWidth(vcr->backend->xlib.chart_font, items[i].chars, items[i].nchars) + items[i].delta;

			*res_width = width;
		}
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		if (row >= 0 && row * VCR_ROW_HEIGHT < vcr->height) {
			int	y = row * VCR_ROW_HEIGHT + 3;
			uint8_t	mask = (0x1 << layer);

			for (int i = 0; i < n_strs && x < vcr->width; i++) {
				unsigned char	c;

				x += 4; /* match the delta used w/XDrawText */

				for (int j = 0, n = 0; j < strs[i].len; j++) {
					c = strs[i].str[j];

					/* skip weird/non-printable chars */
					if (c < ' ' || c > '~')
						continue;

					if (n > 0)
						x += 1;

					if (x + ASCII_WIDTH >= vcr->width) {
						x = vcr->width;
						break;
					}

					for (int k = 0; k < ASCII_HEIGHT; k++) {
						for (int l = 0; l < ASCII_WIDTH; l++) {
							int	x_l = x + l;
							uint8_t	*p = &vcr->mem.bits[(y + k) * vcr->mem.pitch + (x_l >> 1)];

							/* FIXME this can all be done more efficiently */
							if (x_l < 0)
								continue;

							*p = (*p & ~(mask << ((x_l & 0x1) << 2))) | ((mask * ascii_chars[c][k * ASCII_WIDTH + l]) << ((x_l & 0x1) << 2));
						}
					}

					x += ASCII_WIDTH;
					n++;
				}
			}
		}

		if (res_width) {
			int	w = 0;
			/* assume fixed 5x11 ascii glyphs */
			for (int i = 0; i < n_strs; i++) {
				w += 4; /* match the delta used w/XDrawText */

				w += strs[i].len * (ASCII_WIDTH + 1);
			}
			*res_width = w;
		}
		break;
	}

	default:
		assert(0);
	}
}


/* draw an arbitrary orthonormal line into the given layer, on the xlib backend this only works
 * on the VCR_LAYER_TEXT layer, so it's just asserted to only go there for now.. which
 * is fine for the existing callers.
 */
/* TODO: this could have a horiz/vert flag then an offset and length, but since
 * the original code was using XDrawLine directly the call sites already had
 * x1,y1,x2,y2 paramaters onhand.. but we really only draw orthonormal lines,
 * which enforcing simplifies ad-hoc rendering for TYPE_MEM.
 */
void vcr_draw_ortho_line(vcr_t *vcr, vcr_layer_t layer, int x1, int y1, int x2, int y2)
{
	assert(vcr);
	assert(vcr->backend);
	assert(layer == VCR_LAYER_TEXT); /* this is just because only the text layer has the pixmap still */

	assert(x1 >= 0 && y1 >= 0 && x2 >= 0 && y2 >= 0);
	assert(x1 == x2 || y1 == y2); /* expected always orthonormal */

	if (x1 >= vcr->width)
		x1 = vcr->width - 1;
	if (x2 >= vcr->width)
		x2 = vcr->width - 1;
	if (y1 >= vcr->height)
		y1 = vcr->height - 1;
	if (y2 >= vcr->height)
		y2 = vcr->height - 1;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		XDrawLine(vcr->backend->xlib.xserver->display, vcr->xlib.text_pixmap, vcr->backend->xlib.text_gc,
			x1, y1, x2, y2);
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		if (x1 == x2) {
			unsigned	which = (x1 & 0x1) << 2;

			if (y1 > y2) {
				int	t = y1;

				y1 = y2;
				y2 = t;
			}

			/* vertical */
			for (uint8_t *p = &vcr->mem.bits[y1 * vcr->mem.pitch + (x1 >> 1)]; y1 <= y2; p += vcr->mem.pitch, y1++)
				*p |= (0x1 << layer) << which;

		} else {
			/* horizontal */

			if (x1 > x2) {
				int	t = x1;

				x1 = x2;
				x2 = t;
			}

			for (; x1 <= x2; x1++) {
				uint8_t		*p = &vcr->mem.bits[y1 * vcr->mem.pitch + (x1 >> 1)];
				unsigned	which = (x1 & 0x1) << 2;

				*p |= (0x1 << layer) << which;
			}
		}

		break;
	}

	default:
		assert(0);
	}


}


/* marks a "finish line" in layer for row @ current phase */
void vcr_mark_finish_line(vcr_t *vcr, vcr_layer_t layer, int row)
{
	assert(vcr);
	assert(row >= 0);
	/* FIXME: the layers in backend/vcr etc should be in a layer-indexable array */
	assert(layer == VCR_LAYER_GRAPHA || layer == VCR_LAYER_GRAPHB);

	if ((row + 1) * VCR_ROW_HEIGHT >= vcr->height)
		return;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;
		Picture		dest;

		switch (layer) {
		case VCR_LAYER_GRAPHA:
			dest = vcr->xlib.grapha_picture;
			break;
		case VCR_LAYER_GRAPHB:
			dest = vcr->xlib.graphb_picture;
			break;
		default:
			assert(0);
		}

		assert(xserver);

		XRenderComposite(xserver->display, PictOpSrc, vcr->backend->xlib.finish_fill, None, dest,
				 0, 0,					/* src x, y */
				 0, 0,					/* mask x, y */
				 vcr->phase, row * VCR_ROW_HEIGHT,	/* dst x, y */
				 1, VCR_ROW_HEIGHT - 1);

		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	mask = (0x1 << layer) << ((vcr->phase & 0x1) << 2);
		uint8_t	*p;

		p = &vcr->mem.bits[row * VCR_ROW_HEIGHT * vcr->mem.pitch + (vcr->phase >> 1)];
		for (int i = 0; i < VCR_ROW_HEIGHT; i++, p += vcr->mem.pitch)
			*p = ((*p & ~mask) | (mask * (i & 0x1)));

		break;
	}

	default:
		assert(0);
	}
}


/* draw a bar at the current phase into the specified layer of t % with a minimum of min_height pixels.
 *
 * the only layers supported right now are grapha/graphb
 */
void vcr_draw_bar(vcr_t *vcr, vcr_layer_t layer, int row, double t, int min_height)
{
	int	height, y = row * VCR_ROW_HEIGHT;

	assert(vcr);
	assert(row >= 0);
	assert(layer == VCR_LAYER_GRAPHA || layer == VCR_LAYER_GRAPHB);
	assert(min_height >= 0 && min_height < (VCR_ROW_HEIGHT - 1));

	if ((row + 1) * VCR_ROW_HEIGHT >= vcr->height)
		return;

	height = fabs(t) * (double)(VCR_ROW_HEIGHT - 1);

	if (height < min_height)
		height = min_height;

	/* clamp the height to not potentially overflow */
	if (height > (VCR_ROW_HEIGHT - 1))
		height = (VCR_ROW_HEIGHT - 1);

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;
		Picture		*dest;

		switch (layer) {
		case VCR_LAYER_GRAPHA:
			dest = &vcr->xlib.grapha_picture;
			break;
		case VCR_LAYER_GRAPHB:
			dest = &vcr->xlib.graphb_picture;
			y += VCR_ROW_HEIGHT - height - 1;
			break;
		default:
			assert(0);
		}

		assert(xserver);

		XRenderFillRectangle(xserver->display, PictOpSrc, *dest, &chart_visible_color,
			vcr->phase, y,	/* dst x, y */
			1, height);	/* dst w, h */

		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	mask = (0x1 << layer) << ((vcr->phase & 0x1) << 2);
		uint8_t	*p;

		if (layer == VCR_LAYER_GRAPHB)
			y += VCR_ROW_HEIGHT - height - 1;

		p = &vcr->mem.bits[y * vcr->mem.pitch + (vcr->phase >> 1)];
		for (int i = 0; i < height; i++, p += vcr->mem.pitch)
			*p |= mask;

		break;
	}

	default:
		assert(0);
	}
}


/* clear a row in the specified layer */
/* specify negative x and width to clear the entire row, otherwise constraints the clear to x..x+width */
/* TODO FIXME an API that allowed providing a batch of layers would work _very_ well with TYPE_MEM. */
void vcr_clear_row(vcr_t *vcr, vcr_layer_t layer, int row, int x, int width)
{
	assert(vcr);
	assert(layer < VCR_LAYER_CNT);
	assert(row >= 0);

	if (x < 0)
		x = 0;

	if (x > vcr->width)
		x = vcr->width;

	if (width < 0)
		width = vcr->width;

	if (x + width > vcr->width)
		width = vcr->width - x;

	assert(x + width <= vcr->width);

	if ((row + 1) * VCR_ROW_HEIGHT >= vcr->height)
		return;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		Picture		*layers[] = {	/* vcr->xlib should just have these in an array */
					&vcr->xlib.text_picture,
					&vcr->xlib.shadow_picture,
					&vcr->xlib.grapha_picture,
					&vcr->xlib.graphb_picture,
				};
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		XRenderFillRectangle(xserver->display, PictOpSrc, *layers[layer], &chart_trans_color,
			x, row * VCR_ROW_HEIGHT,	/* dst x, y */
			width, VCR_ROW_HEIGHT);		/* dst w, h */
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	mask = ((uint8_t)(0x1 << layer));

		/* naive but correct for now - TODO: optimize */
		for (int i = 0; i < VCR_ROW_HEIGHT; i++) {
			uint8_t *p = &vcr->mem.bits[(row * VCR_ROW_HEIGHT + i) * vcr->mem.pitch + (x >> 1)];

			if (width >= 2) {
				int	W = ((width >> 1) << 1);

				for (int j = 0; j < W; j++, p++) {
					unsigned	which = ((x + j) & 0x1) << 2;

					*p &= ~(mask << which);

					j++;

					which = ((x + j) & 0x1) << 2;
					*p &= ~(mask << which);
				}
			}

			if (width & 0x1) {
				unsigned	which = ((x + 1) & 0x1) << 2;

				*p &= ~(mask << which);
			}
		}
		break;
	}

	default:
		assert(0);
	}
}


/* copy what's below a given row up the specified amount across all the layers */
/* XXX: note for now we expect (and assert) rows == 1, to simplify TYPE_MEM,
 * this is acceptable for all existing call sites
 */
void vcr_shift_below_row_up_one(vcr_t *vcr, int row)
{
	assert(vcr);
	assert(vcr->backend);
	assert(row > 0); /* TODO? assert row doesn't overflow? clamp to hierarchy_end? */

	if ((row + 1) * VCR_ROW_HEIGHT >= vcr->height)
		return;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		Picture		*layers[] = {	/* vcr->xlib should just have these in an array */
					&vcr->xlib.text_picture,
					&vcr->xlib.shadow_picture,
					&vcr->xlib.grapha_picture,
					&vcr->xlib.graphb_picture,
				};
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		assert(xserver);

		for (int layer = 0; layer < NELEMS(layers); layer++) {
			XRenderChangePicture(xserver->display, *layers[layer], CPRepeat, &pa_no_repeat);
			XRenderComposite(xserver->display, PictOpSrc, *layers[layer], None, *layers[layer],
				0, (1 + row) * VCR_ROW_HEIGHT,						/* src */
				0, 0,									/* mask */
				0, row * VCR_ROW_HEIGHT,						/* dest */
				vcr->width, (1 + *(vcr->hierarchy_end_ptr)) * VCR_ROW_HEIGHT - (1 + row) * VCR_ROW_HEIGHT);	/* dimensions */
			XRenderChangePicture(xserver->display, *layers[layer], CPRepeat, &pa_repeat);
		}
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	*dest = &vcr->mem.bits[row * VCR_ROW_HEIGHT * vcr->mem.pitch];
		uint8_t	*src = &vcr->mem.bits[(1 + row) * VCR_ROW_HEIGHT * vcr->mem.pitch];
		size_t	len = ((1 + *(vcr->hierarchy_end_ptr)) - (1 + row)) * VCR_ROW_HEIGHT * vcr->mem.pitch;

		memmove(dest, src, len);
		break;
	}

	default:
		assert(0);
	}
}


/* copy what's below a given row down the specified amount across all layers */
void vcr_shift_below_row_down_one(vcr_t *vcr, int row)
{
	int	dest_y = (row + 1) * VCR_ROW_HEIGHT;

	assert(vcr);
	assert(vcr->backend);
	assert(row >= 0);

	if (dest_y >= vcr->height)
		return;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		Picture		*layers[] = {	/* vcr->xlib should just have these in an array */
					&vcr->xlib.text_picture,
					&vcr->xlib.shadow_picture,
					&vcr->xlib.grapha_picture,
					&vcr->xlib.graphb_picture,
				};
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		assert(xserver);

		for (int layer = 0; layer < NELEMS(layers); layer++) {
			XRenderComposite(xserver->display, PictOpSrc, *layers[layer], None, *layers[layer],
				0, row * VCR_ROW_HEIGHT,				/* src */
				0, 0,							/* mask */
				0, dest_y,						/* dest */
				vcr->width, vcr->height - dest_y);	/* dimensions */
		}
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	*dest = &vcr->mem.bits[dest_y * vcr->mem.pitch];
		uint8_t	*src = &vcr->mem.bits[row * VCR_ROW_HEIGHT * vcr->mem.pitch];
		size_t	len = (vcr->height - dest_y) * vcr->mem.pitch;

		memmove(dest, src, len);
		break;
	}

	default:
		assert(0);
	}
}


/* This shadows the provided layer into the shadow layer for the given row.
 * Currently only layer == VCR_LAYER_TEXT is supported.
 */
void vcr_shadow_row(vcr_t *vcr, vcr_layer_t layer, int row)
{
	assert(vcr);
	assert(layer == VCR_LAYER_TEXT);
	assert(row >= 0);

	if ((row + 1) * VCR_ROW_HEIGHT >= vcr->height)
		return;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		assert(xserver);

		/* the current technique for creating the shadow is to simply render the text at +1/-1 pixel offsets on both axis in translucent black */
		XRenderComposite(xserver->display, PictOpSrc,
			vcr->backend->xlib.shadow_fill, vcr->xlib.text_picture, vcr->xlib.shadow_picture,
			0, 0,
			-1, row * VCR_ROW_HEIGHT,
			0, row * VCR_ROW_HEIGHT,
			vcr->visible_width, VCR_ROW_HEIGHT);

		XRenderComposite(xserver->display, PictOpOver,
			vcr->backend->xlib.shadow_fill, vcr->xlib.text_picture, vcr->xlib.shadow_picture,
			0, 0,
			0, -1 + row * VCR_ROW_HEIGHT,
			0, row * VCR_ROW_HEIGHT,
			vcr->visible_width, VCR_ROW_HEIGHT);

		XRenderComposite(xserver->display, PictOpOver,
			vcr->backend->xlib.shadow_fill, vcr->xlib.text_picture, vcr->xlib.shadow_picture,
			0, 0,
			1, row * VCR_ROW_HEIGHT,
			0, row * VCR_ROW_HEIGHT,
			vcr->visible_width, VCR_ROW_HEIGHT);

		XRenderComposite(xserver->display, PictOpOver,
			vcr->backend->xlib.shadow_fill, vcr->xlib.text_picture, vcr->xlib.shadow_picture,
			0, 0,
			0, 1 + row * VCR_ROW_HEIGHT,
			0, row * VCR_ROW_HEIGHT,
			vcr->visible_width, VCR_ROW_HEIGHT);

		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	text_mask = (0x1 << VCR_LAYER_TEXT);
		uint8_t	shadow_mask = (0x1 << VCR_LAYER_SHADOW);
		int	vcr_width = vcr->width;

		/* TODO: optimize this abomination, maybe switch to shadowing the text @ serialization to png time for the mem->png headless scenario? */

		/* first pass has to clean up the shadow plane while doing one offset of shadow bits */
		for (int i = 0; i < VCR_ROW_HEIGHT; i++) {
			uint8_t	*s = &vcr->mem.bits[(row * VCR_ROW_HEIGHT + i) * vcr->mem.pitch];
			uint8_t	*d = &vcr->mem.bits[(row * VCR_ROW_HEIGHT + i) * vcr->mem.pitch + 1];

			for (int j = 0; j < vcr_width - 2; j++, d++) {
				int	s_shift = (((j + 1) & 0x1) << 2);
				int	d_shift = ((j & 0x1) << 2);
				uint8_t	t = ((*s & (0xf << s_shift) & (text_mask << s_shift)) << 1) >> s_shift; /* turn text bit into shadow bit by shifting over one */

				*d = (*d & ~(shadow_mask << d_shift)) | (t << d_shift);

				j++;
				s++;

				s_shift = (((j + 1) & 0x1) << 2);
				d_shift = ((j & 0x1) << 2);
				t = ((*s & (0xf << s_shift) & (text_mask << s_shift)) << 1) >> s_shift; /* turn text bit into shadow bit by shifting over one */
				*d = (*d & ~(shadow_mask << d_shift)) | (t << d_shift);
			}
		}

		/* second pass ORs the rest of the shadow bits into the now fully initialized shadow plane
		 * at the remaining surrounding offsets.  These can all happen at once now that we can
		 * OR things additively.
		 */
		for (int i = 1; i < VCR_ROW_HEIGHT - 1; i++) {
			uint8_t	*s = &vcr->mem.bits[(row * VCR_ROW_HEIGHT + i) * vcr->mem.pitch];
			uint8_t	*d = &vcr->mem.bits[(row * VCR_ROW_HEIGHT + i) * vcr->mem.pitch];

			for (int j = 0; j < vcr_width - 2; j++, d++) {
				int	s_shift = (((j + 1) & 0x1) << 2);
				int	d_shift = ((j & 0x1) << 2);
				uint8_t	t = ((*s & (0xf << s_shift) & (text_mask << s_shift)) << 1) >> s_shift; /* turn text bit into shadow bit by shifting over one */

				*d |= t << d_shift;

				/* for above and below use *s as dest */
				*(s - vcr->mem.pitch) |= t << s_shift;
				*(s + vcr->mem.pitch) |= t << s_shift;

				j++;
				s++;

				s_shift = (((j + 1) & 0x1) << 2);
				d_shift = ((j & 0x1) << 2);
				t = ((*s & (0xf << s_shift) & (text_mask << s_shift)) << 1) >> s_shift; /* turn text bit into shadow bit by shifting over one */
				*d |= t << d_shift;

				/* for above and below use *s as dest */
				*(s - vcr->mem.pitch) |= t << s_shift;
				*(s + vcr->mem.pitch) |= t << s_shift;
			}
		}
		break;
	}

	default:
		assert(0);
	}
}


/* stash the row from the specified layer in temp storage which the next unstash will copy from */
void vcr_stash_row(vcr_t *vcr, vcr_layer_t layer, int row)
{
	assert(vcr);
	assert(vcr->backend);
	assert(layer == VCR_LAYER_GRAPHA || layer == VCR_LAYER_GRAPHB);
	/* for now we only support stashing graphs */

	if ((row + 1) * VCR_ROW_HEIGHT >= vcr->height)
		return;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		Picture		*layers[] = {
					[VCR_LAYER_GRAPHA] = &vcr->xlib.grapha_picture,
					[VCR_LAYER_GRAPHB] = &vcr->xlib.graphb_picture,
				};
		Picture		*tmps[] = {
					[VCR_LAYER_GRAPHA] = &vcr->xlib.tmp_a_picture,
					[VCR_LAYER_GRAPHB] = &vcr->xlib.tmp_b_picture,
				};
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		assert(xserver);

		XRenderComposite(xserver->display, PictOpSrc, *layers[layer], None, *tmps[layer],
			0, row * VCR_ROW_HEIGHT,	/* src */
			0, 0,				/* mask */
			0, 0,				/* dest */
			vcr->width, VCR_ROW_HEIGHT);	/* dimensions */
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	*src = &vcr->mem.bits[row * VCR_ROW_HEIGHT * vcr->mem.pitch];
		uint8_t *dest = &vcr->mem.tmp[0];
		uint8_t	mask = 0x1 << layer;

		/* we'll do both nibbles at once since this is simply a masked, full-pitch copy of a row,
		 * which means we need to prep the mask for doing both nibbles concurrently.
		 */
		mask |= mask << 4;

		for (int i = 0; i < VCR_ROW_HEIGHT; i++) {
			for (int j = 0; j < vcr->mem.pitch; j++, dest++, src++) {
				*dest = (*dest & ~mask) | (*src & mask);
			}
		}
		break;
	}

	default:
		assert(0);
	}
}


/* unstash the temp stored row to the destination row in the specified layer */
void vcr_unstash_row(vcr_t *vcr, vcr_layer_t layer, int row)
{
	assert(vcr);
	assert(vcr->backend);
	assert(layer == VCR_LAYER_GRAPHA || layer == VCR_LAYER_GRAPHB);

	if ((row + 1) * VCR_ROW_HEIGHT >= vcr->height)
		return;

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		Picture		*layers[] = {
					[VCR_LAYER_GRAPHA] = &vcr->xlib.grapha_picture,
					[VCR_LAYER_GRAPHB] = &vcr->xlib.graphb_picture,
				};
		Picture		*tmps[] = {
					[VCR_LAYER_GRAPHA] = &vcr->xlib.tmp_a_picture,
					[VCR_LAYER_GRAPHB] = &vcr->xlib.tmp_b_picture,
				};
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		assert(xserver);

		XRenderComposite(xserver->display, PictOpSrc, *tmps[layer], None, *layers[layer],
			0, 0,				/* src */
			0, 0,				/* mask */
			0, row * VCR_ROW_HEIGHT,	/* dest */
			vcr->width, VCR_ROW_HEIGHT);	/* dimensions */
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	*dest = &vcr->mem.bits[row * VCR_ROW_HEIGHT * vcr->mem.pitch];
		uint8_t *src = &vcr->mem.tmp[0];
		uint8_t	mask = (0x1 << layer);

		/* see comment above for stash_row */
		mask |= mask << 4;

		for (int i = 0; i < VCR_ROW_HEIGHT; i++) {
			for (int j = 0; j < vcr->mem.pitch; j++, dest++, src++) {
				*dest = (*dest & ~mask) | (*src & mask);
			}
		}
		break;
	}

	default:
		assert(0);
	}
}


/* for now delta is assumed to be either +1/-1, but it'd be interesting to capture delayed samples/updates
 * by having a abs(delta)>1 and show them as gaps in the graph or something like that. TODO
 */
void vcr_advance_phase(vcr_t *vcr, int delta)
{
	assert(vcr);
	assert(vcr->backend);
	assert(delta == -1 || delta == 1);

	vcr->phase += (vcr->width + delta);
	vcr->phase %= vcr->width;

	/* We clear the graphs at the new phase in preparation for the new sample being drawn, across *all*
	 * rows, the entire vertical slice of pixels at this phase is cleared.  Just for the graph layers.
	 */
	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

		assert(xserver);

		XRenderFillRectangle(xserver->display, PictOpSrc, vcr->xlib.grapha_picture, &chart_trans_color, vcr->phase, 0, 1, vcr->height);
		XRenderFillRectangle(xserver->display, PictOpSrc, vcr->xlib.graphb_picture, &chart_trans_color, vcr->phase, 0, 1, vcr->height);

		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM: {
		uint8_t	mask = ~(((uint8_t)((0x1 << VCR_LAYER_GRAPHA) | (0x1 << VCR_LAYER_GRAPHB))) << ((vcr->phase & 0x1) << 2));
		uint8_t	*p = &vcr->mem.bits[vcr->phase >> 1];

		for (int i = 0; i < vcr->height; i++, p += vcr->mem.pitch)
			*p &= mask;

		break;
	}

	default:
		assert(0);
	}
}


/* return the # of combined hierarchy+snowflakes rows in chart */
static inline int vcr_composed_rows(vcr_t *vcr)
{
	int	snowflakes = *(vcr->snowflakes_cnt_ptr) ? 1 + *(vcr->snowflakes_cnt_ptr) : 0; /* don't include the separator row if there are no snowflakes */

	return *(vcr->hierarchy_end_ptr) + snowflakes;
}


/* return the composed height of the chart */
static inline int vcr_composed_height(vcr_t *vcr)
{
	int	rows_height = vcr_composed_rows(vcr) * VCR_ROW_HEIGHT;

	return MIN(rows_height, vcr->visible_height);
}


/* Compose performs whatever work remains up to but not including the present-to-dest step, if there
 * is any such intermediate work necessary to go from the incremental vcr state to a presentable
 * form.  For some backends this may do nothing at all, for others it may do a bunch of compositing
 * of separate layers into a single cached layer which would then be used as the source for a
 * present.
 *
 * This is decoupled from the present() because in cases like real-time visualization, the present may
 * occur at 60FPS re-using the result of a single compose(), then compose() would occur on some lower
 * frequency related to the chart update/sampling frequency, whenever the sampling was performed /and/
 * produced changes.
 *
 * So the compose is likely to be performed as part of the chart update, not part of the present.  But
 * depending on how present is implemented for a given vcr+dest, the present might implicitly perform
 * a compose as part of the serialization of vcr->dest.  (think a mem-vcr serializing to a png-dest,
 * which might be doing some very tedious and slow row-of-pixels at a time compose while it writes the
 * png file, with the mem vcr representation not having a composited result cached at all, instead always
 * keeping the layers in a packed bit-planes in array of bytes form, with the vcr_compose doing effectively
 * nothing for lack of memory to cache the composed results in.
 *
 * This can require allocations so it may fail, hence the non-voide return.
 */
int vcr_compose(vcr_t *vcr)
{
	assert(vcr);
	assert(vcr->backend);

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		/* XXX: this came from charts.c::vwm_chart_compose() */
		vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;
		int		height = vcr_composed_height(vcr);

		assert(xserver);

		/* fill the chart picture with the background */
		XRenderComposite(xserver->display, PictOpSrc, vcr->backend->xlib.bg_fill, None, vcr->xlib.picture,
			0, 0,
			0, 0,
			0, 0,
			vcr->visible_width, height);

		/* draw the graphs into the chart through the stencils being maintained by the sample callbacks */
		XRenderComposite(xserver->display, PictOpOver, vcr->backend->xlib.grapha_fill, vcr->xlib.grapha_picture, vcr->xlib.picture,
			0, 0,
			vcr->phase, 0,
			0, 0,
			vcr->visible_width, height);
		XRenderComposite(xserver->display, PictOpOver, vcr->backend->xlib.graphb_fill, vcr->xlib.graphb_picture, vcr->xlib.picture,
			0, 0,
			vcr->phase, 0,
			0, 0,
			vcr->visible_width, height);

		/* draw the shadow into the chart picture using a translucent black source drawn through the shadow mask */
		XRenderComposite(xserver->display, PictOpOver, vcr->backend->xlib.shadow_fill, vcr->xlib.shadow_picture, vcr->xlib.picture,
			0, 0,
			0, 0,
			0, 0,
			vcr->visible_width, height);

		/* render chart text into the chart picture using a white source drawn through the chart text as a mask, on top of everything */
		XRenderComposite(xserver->display, PictOpOver, vcr->backend->xlib.text_fill, vcr->xlib.text_picture, vcr->xlib.picture,
			0, 0,
			0, 0,
			0, 0,
			vcr->visible_width, (*(vcr->hierarchy_end_ptr) * VCR_ROW_HEIGHT));

		XRenderComposite(xserver->display, PictOpOver, vcr->backend->xlib.snowflakes_text_fill, vcr->xlib.text_picture, vcr->xlib.picture,
			0, 0,
			0, *(vcr->hierarchy_end_ptr) * VCR_ROW_HEIGHT,
			0, *(vcr->hierarchy_end_ptr) * VCR_ROW_HEIGHT,
			vcr->visible_width, height - (*(vcr->hierarchy_end_ptr) * VCR_ROW_HEIGHT));
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM:
		/* this probably doesn't do anything since mem is embedded-targed, but variants could be
		 * introduced in the future .
		 */
		break;

	default:
		assert(0);
	}

	return 0;
}


#ifdef USE_XLIB
/* this is an xlib-backend specific helper for turning the composed area into an xdamage region,
 * which is primarily needed by the vwm use case.
 */
int vcr_get_composed_xdamage(vcr_t *vcr, XserverRegion *res_damaged_region)
{
	XRectangle	damage = {};

	assert(vcr);
	assert(vcr->backend);
	assert(vcr->backend->type == VCR_BACKEND_TYPE_XLIB);
	assert(res_damaged_region);

	/* TODO: ideally this would actually be a more granular damage region constructed
	 * piecemeal during the compose process since the last present of the vcr to an xlib dest.
	 * but for now it just produces a damage region of the visible area for the chart.
	 */
	damage.width = vcr->visible_width;
	damage.height = vcr->visible_height;

	*res_damaged_region = XFixesCreateRegion(vcr->backend->xlib.xserver->display, &damage, 1);

	return 0;
}


#ifdef USE_PNG
/* present the chart into a newly allocated pixmap, intended for snapshotting purposes */
static void vcr_present_xlib_to_pixmap(vcr_t *vcr, const XRenderColor *bg_color, Pixmap *res_pixmap)
{
	static const XRenderColor	blackness = { 0x0000, 0x0000, 0x0000, 0xFFFF};
	vcr_dest_t			*vcr_dest;
	vwm_xserver_t			*xserver;
	Picture			 	dest;

	assert(vcr);
	assert(vcr->backend);
	assert(vcr->backend->type == VCR_BACKEND_TYPE_XLIB);
	assert(res_pixmap);

	xserver = vcr->backend->xlib.xserver;

	if (!bg_color)
		bg_color = &blackness;

	dest = create_picture_fill(xserver, vcr->visible_width, vcr_composed_height(vcr), 32, 0, NULL, bg_color, res_pixmap);
	vcr_dest = vcr_dest_xpicture_new(vcr->backend, dest);
	vcr_present(vcr, VCR_PRESENT_OP_OVER, vcr_dest, -1, -1, -1, -1);
	vcr_dest = vcr_dest_free(vcr_dest);
}


/* present the chart the chart into an ximage we can access locally for saving as a png */
static void vcr_present_xlib_to_ximage(vcr_t *vcr, const XRenderColor *bg_color, XImage **res_ximage)
{
	Pixmap		dest_pixmap;
	vwm_xserver_t	*xserver;

	assert(vcr);
	assert(vcr->backend);
	assert(vcr->backend->type == VCR_BACKEND_TYPE_XLIB);
	assert(res_ximage);

	xserver = vcr->backend->xlib.xserver;

	assert(xserver);

	vcr_present_xlib_to_pixmap(vcr, bg_color, &dest_pixmap);
	*res_ximage = XGetImage(xserver->display,
				dest_pixmap,
				0,
				0,
				vcr->visible_width,
				vcr_composed_height(vcr),
				AllPlanes,
				ZPixmap);
	XFreePixmap(xserver->display, dest_pixmap);
}


/* Implements present of an xlib-backed vcr to a png dest.
 * This is basically the OG pre-vcr snapshot_as_png code from vmon.c,
 * meaning it makes no effort to be super conservative in terms of memory
 * consumption etc.
 */
static int vcr_present_xlib_to_png(vcr_t *vcr, vcr_dest_t *dest)
{
	XImage		*chart_as_ximage;
	png_bytepp	row_pointers;

	assert(vcr);
	assert(vcr->backend);
	assert(vcr->backend->type == VCR_BACKEND_TYPE_XLIB);
	assert(dest);
	assert(dest->type == VCR_DEST_TYPE_PNG);
	assert(dest->png.output);

	vcr_present_xlib_to_ximage(vcr, NULL, &chart_as_ximage);

	row_pointers = malloc(sizeof(void *) * chart_as_ximage->height);
	if (!row_pointers) {
		XDestroyImage(chart_as_ximage);

		return -ENOMEM;
	}

	for (unsigned i = 0; i < chart_as_ximage->height; i++)
		row_pointers[i] = &((png_byte *)chart_as_ximage->data)[i * chart_as_ximage->bytes_per_line];

	if (setjmp(png_jmpbuf(dest->png.png_ctx)) != 0) {
		XDestroyImage(chart_as_ximage);
		free(row_pointers);

		return -ENOMEM;
	}

	/* XXX: I'm sure this is making flawed assumptions about the color format
	 * and type etc, but this makes it work for me and that's Good Enough for now.
	 * One can easily turn runtime mapping of X color formats, endianness, and packing
	 * details to whatever a file format like PNG can express into a tar-filled rabbithole
	 * of fruitless wankery.
	 */
	png_set_bgr(dest->png.png_ctx);
	png_set_IHDR(dest->png.png_ctx, dest->png.info_ctx,
		chart_as_ximage->width,
		chart_as_ximage->height,
		8,
		PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_BASE,
		PNG_FILTER_TYPE_BASE);

	png_write_info(dest->png.png_ctx, dest->png.info_ctx);
	png_write_image(dest->png.png_ctx, row_pointers);
	png_write_end(dest->png.png_ctx, NULL);

	XDestroyImage(chart_as_ximage);
	free(row_pointers);

	return 0;
}
#endif /* USE_PNG */

#endif /* USE_XLIB */


#ifdef USE_PNG

/* We basically just statically define the blended outputs of all the layer combinations,
 * so first we or together all the relevant palette indices for those combinations, and
 * give them symbolic names to later use when populating hte palette with actual rgb values.
 */
#define VCR_TEXT			(0x1 << VCR_LAYER_TEXT)
#define VCR_SHADOW			(0x1 << VCR_LAYER_SHADOW)
#define VCR_GRAPHA			(0x1 << VCR_LAYER_GRAPHA)
#define VCR_GRAPHB			(0x1 << VCR_LAYER_GRAPHB)
#define VCR_GRAPHAB			((0x1 << VCR_LAYER_GRAPHA) | (0x1 << VCR_LAYER_GRAPHB))
/* These bits aren't stored in the vcr->mem.bits[] nibbles, but do get used as palette indices.
 * Their value is generated during the mem_to_png() process, derived from Y position within a row,
 * and row position within a snapshot.
 */
#define VCR_SEP				(0x1 << VCR_LAYER_CNT)
#define VCR_ODD				(0x1 << (VCR_LAYER_CNT + 1))

#define VCR_SEP_ODD			(VCR_SEP | VCR_ODD)
#define VCR_GRAPHA_ODD			(VCR_GRAPHA | VCR_ODD)
#define VCR_GRAPHB_ODD			(VCR_GRAPHB | VCR_ODD)
#define VCR_GRAPHAB_ODD			(VCR_GRAPHAB | VCR_ODD)
#define VCR_SHADOW_ODD			(VCR_SHADOW | VCR_ODD)

/* text over anything is going to just be white */
#define VCR_TEXT_SEP			(VCR_TEXT | VCR_SEP)
#define VCR_TEXT_ODD			(VCR_TEXT | VCR_ODD)
#define VCR_TEXT_SEP_ODD		(VCR_TEXT | VCR_SEP | VCR_ODD)

#define VCR_TEXT_GRAPHA			(VCR_TEXT | VCR_GRAPHA)
#define VCR_TEXT_GRAPHB			(VCR_TEXT | VCR_GRAPHB)
#define VCR_TEXT_GRAPHAB		(VCR_TEXT | VCR_GRAPHAB)
#define VCR_TEXT_SHADOW			(VCR_TEXT | VCR_SHADOW)
#define VCR_TEXT_GRAPHA_SHADOW		(VCR_TEXT | VCR_GRAPHA | VCR_SHADOW)
#define VCR_TEXT_GRAPHB_SHADOW		(VCR_TEXT | VCR_GRAPHB | VCR_SHADOW)
#define VCR_TEXT_GRAPHAB_SHADOW		(VCR_TEXT | VCR_GRAPHAB | VCR_SHADOW)

#define VCR_TEXT_SEP_SHADOW		(VCR_TEXT | VCR_SEP | VCR_SHADOW)

#define VCR_TEXT_ODD_SEP		(VCR_TEXT | VCR_SEP | VCR_ODD)
#define VCR_TEXT_ODD_GRAPHA		(VCR_TEXT | VCR_ODD | VCR_GRAPHA)
#define VCR_TEXT_ODD_GRAPHB		(VCR_TEXT | VCR_ODD | VCR_GRAPHB)
#define VCR_TEXT_ODD_GRAPHAB		(VCR_TEXT | VCR_ODD | VCR_GRAPHAB)
#define VCR_TEXT_ODD_SHADOW		(VCR_TEXT | VCR_ODD | VCR_SHADOW)
#define VCR_TEXT_ODD_SEP_SHADOW		(VCR_TEXT | VCR_ODD | VCR_SEP | VCR_SHADOW)
#define VCR_TEXT_ODD_GRAPHA_SHADOW	(VCR_TEXT | VCR_ODD | VCR_GRAPHA | VCR_SHADOW)
#define VCR_TEXT_ODD_GRAPHB_SHADOW	(VCR_TEXT | VCR_ODD | VCR_GRAPHB | VCR_SHADOW)
#define VCR_TEXT_ODD_GRAPHAB_SHADOW	(VCR_TEXT | VCR_ODD | VCR_GRAPHAB | VCR_SHADOW)

/* shadows over graph colors get blended, otherwise they're left black */
#define VCR_SHADOW_GRAPHA		(VCR_SHADOW | VCR_GRAPHA)
#define VCR_SHADOW_GRAPHB		(VCR_SHADOW | VCR_GRAPHB)
#define VCR_SHADOW_GRAPHAB		(VCR_SHADOW | VCR_GRAPHAB)
#define VCR_SHADOW_ODD_GRAPHA		(VCR_SHADOW | VCR_ODD | VCR_GRAPHA)
#define VCR_SHADOW_ODD_GRAPHB		(VCR_SHADOW | VCR_ODD | VCR_GRAPHB)
#define VCR_SHADOW_ODD_GRAPHAB		(VCR_SHADOW | VCR_ODD | VCR_GRAPHAB)

/* when without shadow */
#define VCR_PNG_WHITE			{0xff, 0xff, 0xff}
#define VCR_PNG_RED			{0xff, 0x00, 0x00}
#define VCR_PNG_CYAN			{0x00, 0xff, 0xff}
#define VCR_PNG_DARK_GRAY		{0x30, 0x30, 0x30}	/* used for separator */
#define VCR_PNG_DARKER_GRAY		{0x10, 0x10, 0x10}	/* used for odd rows background */

/* when in shadow */
#define VCR_PNG_DARK_WHITE		{0x4a, 0x4a, 0x4a}
#define VCR_PNG_DARK_RED		{0x80, 0x00, 0x00}
#define VCR_PNG_DARK_CYAN		{0x00, 0x5b, 0x5b}

enum {
	VCR_LUT_BLACK = 0,
	VCR_LUT_WHITE,
	VCR_LUT_RED,
	VCR_LUT_CYAN,
	VCR_LUT_DARK_GRAY,
	VCR_LUT_DARKER_GRAY,
	VCR_LUT_DARK_WHITE,
	VCR_LUT_DARK_RED,
	VCR_LUT_DARK_CYAN,
};


static int vcr_present_mem_to_png(vcr_t *vcr, vcr_dest_t *dest)
{
	static png_color	pal[] = { /* programming gfx like it's 1990 can be such a joy */
					[VCR_LUT_BLACK] = {},
					[VCR_LUT_WHITE] = VCR_PNG_WHITE,
					[VCR_LUT_RED] = VCR_PNG_RED,
					[VCR_LUT_CYAN] = VCR_PNG_CYAN,
					[VCR_LUT_DARK_GRAY] = VCR_PNG_DARK_GRAY,
					[VCR_LUT_DARKER_GRAY] = VCR_PNG_DARKER_GRAY,
					[VCR_LUT_DARK_WHITE] = VCR_PNG_DARK_WHITE,
					[VCR_LUT_DARK_RED] = VCR_PNG_DARK_RED,
					[VCR_LUT_DARK_CYAN] = VCR_PNG_DARK_CYAN,
				};

	/* lut is an indirection table for mapping layer bit combinations to the above deduplicated denser color palette */
	static uint8_t		lut[256] = {
					/* text solid white above all layers */
					[VCR_TEXT] = VCR_LUT_WHITE,
					[VCR_TEXT_SEP] = VCR_LUT_WHITE,
					[VCR_TEXT_SEP_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_GRAPHA] = VCR_LUT_WHITE,
					[VCR_TEXT_GRAPHB] = VCR_LUT_WHITE,
					[VCR_TEXT_GRAPHAB] = VCR_LUT_WHITE,
					[VCR_TEXT_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_GRAPHA_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_GRAPHB_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_GRAPHAB_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_SEP] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_SEP_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_GRAPHA] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_GRAPHB] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_GRAPHAB] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_GRAPHA_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_GRAPHB_SHADOW] = VCR_LUT_WHITE,
					[VCR_TEXT_ODD_GRAPHAB_SHADOW] = VCR_LUT_WHITE,

					/* no shadow or text, plain graph colors */
					[VCR_GRAPHA] = VCR_LUT_RED,
					[VCR_GRAPHB] = VCR_LUT_CYAN,
					[VCR_GRAPHAB] = VCR_LUT_WHITE,
					[VCR_GRAPHA_ODD] = VCR_LUT_RED,
					[VCR_GRAPHB_ODD] = VCR_LUT_CYAN,
					[VCR_GRAPHAB_ODD] = VCR_LUT_WHITE,

					/* shadowed same but dark */
					[VCR_SHADOW_GRAPHA] = VCR_LUT_DARK_RED,
					[VCR_SHADOW_GRAPHB] = VCR_LUT_DARK_CYAN,
					[VCR_SHADOW_GRAPHAB] = VCR_LUT_DARK_WHITE,
					[VCR_SHADOW_ODD_GRAPHA] = VCR_LUT_DARK_RED,
					[VCR_SHADOW_ODD_GRAPHB] = VCR_LUT_DARK_CYAN,
					[VCR_SHADOW_ODD_GRAPHAB] = VCR_LUT_DARK_WHITE,

					/* the rest get defaulted to black, which is great. */
					[VCR_SEP] = VCR_LUT_DARK_GRAY,
					[VCR_ODD] = VCR_LUT_DARKER_GRAY,
					[VCR_SEP_ODD] = VCR_LUT_DARK_GRAY,
				};

	png_bytepp		row_pointers;
	uint8_t			*row_pixels;
	size_t			row_stride = vcr->width >> 1;

	row_pixels = malloc(VCR_ROW_HEIGHT * row_stride);
	if (!row_pixels)
		return -ENOMEM;

	row_pointers = malloc(sizeof(void *) * VCR_ROW_HEIGHT);
	if (!row_pointers) {
		free(row_pixels);
		return -ENOMEM;
	}

	for (int i = 0; i < VCR_ROW_HEIGHT; i++)
		row_pointers[i] = &((png_byte *)row_pixels)[i * row_stride];

	if (setjmp(png_jmpbuf(dest->png.png_ctx)) != 0) {
		free(row_pixels);
		free(row_pointers);
		return -ENOMEM;
	}

	png_set_IHDR(dest->png.png_ctx, dest->png.info_ctx,
		vcr->width,		/* always use the full width/height for the file dimensions, it's annoying when comparing images to have a variety of dimensions. */
		vcr->height,
		4,			/* 4-bit color index (16 color palette) for smaller file sizes */
		PNG_COLOR_TYPE_PALETTE,	/* we use a palette for mem->png for less ram and filesize */
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_BASE,
		PNG_FILTER_TYPE_BASE);

	png_set_PLTE(dest->png.png_ctx, dest->png.info_ctx, pal, NELEMS(pal));

	/* This differs from xlib_to_png in that it presents row-at-a-time from
	 * the packed form @ vcr->mem.bits to dest->png_ctx.  Note "row" in this
	 * context is a row of chart data, not a single row of pixels.
	 *
	 * This approach saves memory by not needing another WxH full copy of
	 * the rendered form of vcr->mem.bits for png_write_image() to access.
	 * But it does make the implementation a bit more tedious, and probably
	 * a little slower.
	 */
	png_write_info(dest->png.png_ctx, dest->png.info_ctx);
	{
		int	n_rows = MIN(vcr_composed_rows(vcr), vcr->height / VCR_ROW_HEIGHT); /* prevent n_rows from overflowing the height */

		for (int i = 0; i < n_rows; i++) {
			uint8_t	*d = row_pixels;
			uint8_t	mask = (0x1 << VCR_LAYER_GRAPHA) | (0x1 << VCR_LAYER_GRAPHB);
			uint8_t odd = ((VCR_ODD << 4 | VCR_ODD) * (i & 0x1));

			/* The graph layers need to be moved to vcr->phase, since the per-sample updates just draw
			 * individual graph bars without bothering to move the whole graph layer every sample.
			 * It makes the present more complicated / less efficient, but generally sampling is done
			 * more frequently.
			 */
			for (int j = 0; j < VCR_ROW_HEIGHT; j++) {
				uint8_t	*s = &vcr->mem.bits[(i * VCR_ROW_HEIGHT + j) * vcr->mem.pitch];
				uint8_t	border = j == (VCR_ROW_HEIGHT - 1) ? VCR_SEP : 0x0;

				for (int k = 0; k < vcr->width; k++, s++, d++) {
					unsigned phase_k_mod_width = ((vcr->phase + k) % vcr->width);
					unsigned sg_shift = (phase_k_mod_width & 0x1) << 2;
					uint8_t	*sg = &vcr->mem.bits[(i * VCR_ROW_HEIGHT + j) * vcr->mem.pitch + (phase_k_mod_width >> 1)];
					uint8_t	pp;

					/* pp will hold the png-appropriate indexed-color 4bpp packed pixel */
					pp = lut[(*s & (~mask & 0xf)) | ((*sg & (mask << sg_shift)) >> sg_shift) | border | odd] << 4;

					/* this copy pasta unrolls the loop to unpack two pixels from the nibbles at a time */
					k++;
					/* note there's no need to advance s twice since we get two pixels out of it per byte, and sg
					 * is simply recomputed entirely again because of the phase wrapping that must be dealt with,
					 * this can all be optimized later if we care.
					 */

					phase_k_mod_width = ((vcr->phase + k) % vcr->width);
					sg_shift = (phase_k_mod_width & 0x1) << 2;
					sg = &vcr->mem.bits[(i * VCR_ROW_HEIGHT + j) * vcr->mem.pitch + (phase_k_mod_width >> 1)];
					pp |= lut[((*s & ~(mask << 4)) >> 4) | ((*sg & (mask << sg_shift)) >> sg_shift) | border | odd];

					*d = pp;
				}
			}

			png_write_rows(dest->png.png_ctx, row_pointers, VCR_ROW_HEIGHT);
		}

		/* just black out whatever remains */
		memset(row_pixels, 0x00, row_stride);
		for (int i = n_rows * VCR_ROW_HEIGHT; i < vcr->height; i++)
			png_write_row(dest->png.png_ctx, row_pointers[0]);
	}
	png_write_end(dest->png.png_ctx, dest->png.info_ctx);

	free(row_pixels);
	free(row_pointers);

	return 0;
}
#endif /* USE_PNG */


/* This serializes vcr's state to dest, which may be a fast and snappy operation
 * like when the dest is an xwindow or xpicture from an xlib backend with a vcr
 * from that same xlib backend.  Or it might be a rather slow and tedious but
 * memory-frugal operation like when dest is a png and vcr a mem backend, which
 * targets more embedded-style memory-constrained headless use cases.
 *
 * Note there are coordinates/dimensions provided which most of the time will just
 * match the destination, but especially in the vwm composited WM use case this
 * won't be true because the dest is a picture representing the X root window.
 * There we must translate the presented output within the root window wherever the
 * related X window is being composited, and clip it to within the window's borders.
 *
 * Otherwise, we generally just spit out the entirety of the vcr's charts.
 *
 * providing x/y/width/height all as -1 is treated as a special case of present the whole vcr to the dest
 * @ 0,0 - it's a convenience for use caases that just want to spit the full chart out to a dest containing
 * nothing but the cart, so callers don't have to access+supply such things.
 */
int vcr_present(vcr_t *vcr, vcr_present_op_t op, vcr_dest_t *dest, int x, int y, int width, int height)
{
	assert(vcr);
	assert(vcr->backend);
	assert(dest);

	if (x == -1 && y == -1 && width == -1 && height == -1) {
		x = y = 0;
		width = vcr->visible_width;
		height = vcr_composed_height(vcr);
	}

	switch (vcr->backend->type) {
#ifdef USE_XLIB
	case VCR_BACKEND_TYPE_XLIB: {
		int	xop;

		switch (op) {
		case VCR_PRESENT_OP_SRC:
			xop = PictOpSrc;
			break;
		case VCR_PRESENT_OP_OVER:
			xop = PictOpOver;
			break;
		default:
			assert(0);
		}
		switch (dest->type) {
		case VCR_DEST_TYPE_XWINDOW: {
			vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;

			/* present xlib->xwindow */
			/* vmon use case */

			XRenderComposite(xserver->display, xop, vcr->xlib.picture, None, dest->xwindow.picture,
					 0, 0, 0, 0,							/* src x,y, maxk x, y */
					 x,								/* dst x */
					 y,								/* dst y */
					 width, MIN(vcr_composed_height(vcr), height) /* FIXME */);	/* w, h */
			break;
		}

		case VCR_DEST_TYPE_XPICTURE: {
			vwm_xserver_t	*xserver = vcr->backend->xlib.xserver;
			/* present xlib->xpicture */
			/* vwm use case */
			XRenderComposite(xserver->display, xop, vcr->xlib.picture, None, dest->xpicture.picture,
					 0, 0, 0, 0,							/* src x,y, maxk x, y */
					 x,								/* dst x */
					 y,								/* dst y */
					 width, MIN(vcr_composed_height(vcr), height) /* FIXME */);	/* w, h */
			break;
		}

#ifdef USE_PNG
		case VCR_DEST_TYPE_PNG:
			/* present xlib->png */
			/* this would enable snapshotting to png in vwm which isn't currently supported,
			 * but is also necessary to support the vmon PNG snapshotting from X use case,
			 * which is already supported in the pre-vcr era.
			 */
			return vcr_present_xlib_to_png(vcr, dest);
#endif /* USE_PNG */

		default:
			assert(0);
		}
		break;
	}
#endif /* USE_XLIB */

	case VCR_BACKEND_TYPE_MEM:
		switch (dest->type) {
#ifdef USE_XLIB
		case VCR_DEST_TYPE_XWINDOW:
			/* present mem->xwindow */
		case VCR_DEST_TYPE_XPICTURE:
			/* present mem->xpicture */
			VWM_ERROR("vcr_present(vcr=mem dest=x{window,picture}) unsupported");
			/* XXX: these aren't currently supported, but may be interesting to experiment
			 * with in the future as a low-memory real-time visualization mode, if it could
			 * be made efficient enough.
			 */
			assert(0);
#endif /* USE_XLIB */

#ifdef USE_PNG
		case VCR_DEST_TYPE_PNG:
			/* present mem->png */
			/* this is the headless vmon -> periodic png snapshots mode,
			 * which is the whole impetus for adding the vcr abstraction.
			 */
			return vcr_present_mem_to_png(vcr, dest);
#endif
		default:
			assert(0);
		}
		break;

	default:
		assert(0);
	}

	return 0;
}
