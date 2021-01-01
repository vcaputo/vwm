#ifndef _WINDOW_H
#define _WINDOW_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "direction.h"
#include "list.h"
#include "screen.h"

typedef struct _vwm_t vwm_t;
typedef struct _vwm_xwindow_t vwm_xwindow_t;
typedef struct _vwm_desktop_t vwm_desktop_t;

typedef enum _vwm_win_autoconf_t {
	VWM_WIN_AUTOCONF_NONE,		/* un-autoconfigured window (used to restore the configuration) */
	VWM_WIN_AUTOCONF_QUARTER,	/* quarter-screened */
	VWM_WIN_AUTOCONF_HALF,		/* half-screened */
	VWM_WIN_AUTOCONF_FULL,		/* full-screened */
	VWM_WIN_AUTOCONF_ALL		/* all-screened (borderless) */
} vwm_win_autoconf_t;

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
	unsigned int		shelved:1;		/* is the window shelved? */
} vwm_window_t;


void vwm_win_unmap(vwm_t *vwm, vwm_window_t *vwin);
void vwm_win_map(vwm_t *vwm, vwm_window_t *vwin);
vwm_window_t * vwm_win_mru(vwm_t *vwm, vwm_window_t *vwin);
vwm_window_t * vwm_win_lookup(vwm_t *vwm, Window win);
vwm_window_t * vwm_win_get_focused(vwm_t *vwm);
void vwm_win_set_focused(vwm_t *vwm, vwm_window_t *vwin);

typedef enum _vwm_side_t {
	VWM_SIDE_TOP,
	VWM_SIDE_BOTTOM,
	VWM_SIDE_LEFT,
	VWM_SIDE_RIGHT
} vwm_side_t;

typedef enum _vwm_corner_t {
	VWM_CORNER_TOP_LEFT,
	VWM_CORNER_TOP_RIGHT,
	VWM_CORNER_BOTTOM_RIGHT,
	VWM_CORNER_BOTTOM_LEFT
} vwm_corner_t;

void vwm_win_autoconf(vwm_t *vwm, vwm_window_t *vwin, vwm_screen_rel_t rel, vwm_win_autoconf_t conf, ...);
void vwm_win_autoconf_magic(vwm_t *vwm, vwm_window_t *vwin, const vwm_screen_t *scr, int x, int y, int width, int height);
void vwm_win_focus(vwm_t *vwm, vwm_window_t *vwin);

typedef enum _vwm_fence_t {
	VWM_FENCE_IGNORE = 0,		/* behave as if screen boundaries don't exist (like the pre-Xinerama code) */
	VWM_FENCE_RESPECT,		/* confine operation to within the screen */
	VWM_FENCE_TRY_RESPECT,		/* confine operation to within the screen, unless no options exist. */
	VWM_FENCE_VIOLATE,		/* leave the screen for any other */
	VWM_FENCE_MASKED_VIOLATE	/* leave the screen for any other not masked */
} vwm_fence_t;

vwm_window_t * vwm_win_focus_next(vwm_t *vwm, vwm_window_t *vwin, vwm_direction_t direction, vwm_fence_t fence);
void vwm_win_shelve(vwm_t *vwm, vwm_window_t *vwin);
void vwm_win_unfocus(vwm_t *vwm, vwm_window_t *vwin);
vwm_xwindow_t * vwm_win_unmanage(vwm_t *vwm, vwm_window_t *vwin);
vwm_window_t * vwm_win_manage_xwin(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_win_migrate(vwm_t *vwm, vwm_window_t *vwin, vwm_desktop_t *desktop);


#endif
