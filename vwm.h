#ifndef _VWM_H
#define _VWM_H

#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>

#include "list.h"

#define VWM_ERROR(fmt, args...)   fprintf(stderr, "%s:%i\t%s() "fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##args)
#define VWM_PERROR(fmt, args...)   fprintf(stderr, "%s:%i\t%s() "fmt"; %s\n", __FILE__, __LINE__, __FUNCTION__, ##args, strerror(errno))
#define VWM_BUG(fmt, args...)   fprintf(stderr, "BUG %s:%i\t%s() "fmt"; %s\n", __FILE__, __LINE__, __FUNCTION__, ##args, strerror(errno))

#ifdef TRACE
#define VWM_TRACE(fmt, args...)   fprintf(stderr, "%s:%i\t%s() "fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##args)
#else
#define VWM_TRACE(fmt, args...)
#endif

typedef struct _vwm_box_t {
	int		x, y;
	unsigned int	width, height;
} vwm_box_t;

typedef struct _vwm_desktop_t {
	list_head_t		desktops;			/* global list of (virtual) desktops */
	list_head_t		desktops_mru;			/* global list of (virtual) desktops in MRU order */
	char			*name;				/* name of the desktop (TODO) */
	struct _vwm_window_t	*focused_window;		/* the focused window on this virtual desktop */
} vwm_desktop_t;

typedef struct _vwm_window_t {
	list_head_t		windows;		/* global list of managed client windows */
	struct _vwm_desktop_t	*desktop;		/* desktop this window belongs to currently */
	char			*name;			/* client name */

	vwm_box_t		client;			/* box of the configured client window relative to the root */

	Window			window;			/* the X window being managed by vwm */

	XSizeHints		*hints;			/* hints the client supplied */
	long			hints_supplied;		/* bitfield reflecting the hints the client supplied */

	unsigned int		fullscreened:2;		/* is the window fullscreened (1)?  is the window "allscreened" with the border obscured (2)? */
	unsigned int		mapped:1;		/* has mapping been requested on the window? */
	unsigned int		shelved:1;		/* is the window shelved? */
} vwm_window_t;

#endif
