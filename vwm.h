#ifndef _VWM_H
#define _VWM_H

#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>

#include "list.h"

#define VWM_ERROR(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##_args)
#define VWM_PERROR(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"; %s\n", __FILE__, __LINE__, __FUNCTION__, ##_args, strerror(errno))
#define VWM_BUG(_fmt, _args...)		fprintf(stderr, "BUG %s:%i\t%s() "_fmt"; %s\n", __FILE__, __LINE__, __FUNCTION__, ##_args, strerror(errno))

#ifdef TRACE
#define VWM_TRACE(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##_args)
#else
#define VWM_TRACE(_fmt, _args...)	do { } while(0)
#endif

typedef struct _vwm_desktop_t {
	list_head_t		desktops;		/* global list of (virtual) desktops */
	list_head_t		desktops_mru;		/* global list of (virtual) desktops in MRU order */
	char			*name;			/* name of the desktop (TODO) */
	struct _vwm_window_t	*focused_window;	/* the focused window on this virtual desktop */
} vwm_desktop_t;

/* everything needed by the per-window overlay's context */
typedef struct _vwm_overlay_t {
	Pixmap				text_pixmap;		/* pixmap for overlayed text (kept around for XDrawText usage) */
	Picture				text_picture;		/* picture representation of text_pixmap */
	Picture				shadow_picture;		/* text shadow layer */
	Picture				grapha_picture;		/* graph A layer */
	Picture				graphb_picture;		/* graph B layer */
	Picture				tmp_picture;		/* 1 row worth of temporary picture space */
	Picture				picture;		/* overlay picture derived from the pixmap, for render compositing */
	int				width;			/* current width of the overlay */
	int				height;			/* current height of the overlay */
	int				phase;			/* current position within the (horizontally scrolling) graphs */
	int				heirarchy_end;		/* row where the process heirarchy currently ends */
	int				snowflakes_cnt;		/* count of snowflaked rows (reset to zero to truncate snowflakes display) */
	int				gen_last_composed;	/* the last composed vmon generation */
} vwm_overlay_t;

/* every window gets this, even non-managed ones.  For compositing vwm must track everything visible, even popup menus. */
typedef struct _vwm_xwindow_t {
	list_head_t		xwindows;		/* global list of all windows kept in X stacking order */

	Window			id;			/* X Window backing this instance */
	XWindowAttributes	attrs;			/* X window's current attributes, kept up-to-date in handling of ConfigureNotify events */
	Damage			damage;			/* X damage object associated with the window (for compositing) */
	Picture			picture;		/* X picture object representing the window (for compositing) */
	Pixmap			pixmap;			/* X pixmap object representing the window (for compositing) */

	vmon_proc_t		*monitor;		/* vmon process monitor handle, may be NULL if for example the X client doesn't supply a PID */
	vwm_overlay_t		overlay;		/* monitoring overlay state */

	char			*name;			/* client name */
	unsigned int		mapped:1;		/* is the window currently mapped (by client) */
	unsigned int		occluded:1;		/* is the window occluded entirely by another window? (used and valid only during paint_all()) */
							/* if only Xorg could send VisibilityNotify events when requested for redirected windows :( */
	struct _vwm_window_t	*managed;		/* is the window "managed"? NULL or this points to the managed context of the window */
} vwm_xwindow_t;

/* the managed window we create for every mapped window we actually manage */
typedef struct _vwm_window_t {
	list_head_t		windows_mru;		/* global list of managed windows kept in MRU order */

	vwm_xwindow_t		*xwindow;		/* window being managed */
	vwm_desktop_t		*desktop;		/* desktop this window belongs to currently */

	XWindowAttributes	client;			/* attrs of the client-configured window */

	XSizeHints		*hints;			/* hints the client supplied */
	long			hints_supplied;		/* bitfield reflecting the hints the client supplied */

	unsigned int		autoconfigured:3;	/* autoconfigured window states (none/quarter/half/full/all) */
	unsigned int		mapping:1;		/* is the window being mapped? (by vwm) */
	unsigned int		unmapping:1;		/* is the window being unmapped? (by vwm) */
	unsigned int		configuring:1;		/* is the window being configured/placed? (by vwm) */
	unsigned int		shelved:1;		/* is the window shelved? */
} vwm_window_t;

#endif
