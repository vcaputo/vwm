#ifndef _XWIN_H
#define _XWIN_H

#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xlib.h>

#include "list.h"
#include "charts.h"

typedef struct _vwm_t vwm_t;
typedef struct _vwm_window_t vwm_window_t;

/* every window gets this, even non-managed ones.  For compositing vwm must track everything visible, even popup menus. */
typedef struct _vwm_xwindow_t {
	list_head_t		xwindows;		/* global list of all windows kept in X stacking order */

	Window			id;			/* X Window backing this instance */
	XWindowAttributes	attrs;			/* X window's current attributes, kept up-to-date in handling of ConfigureNotify events */
	Damage			damage;			/* X damage object associated with the window (for compositing) */
	Picture			picture;		/* X picture object representing the window (for compositing) */
	Pixmap			pixmap;			/* X pixmap object representing the window (for compositing) */

	vwm_chart_t		*chart;			/* monitoring chart state */

	char			*name;			/* client name */
	unsigned int		client_mapped:1;	/* is the window currently mapped (by client) */
	unsigned int		occluded:1;		/* is the window occluded entirely by another window? (used and valid only during paint_all()) */
							/* if only Xorg could send VisibilityNotify events when requested for redirected windows :( */
	vwm_window_t		*managed;		/* is the window "managed"? NULL or this points to the managed context of the window */
} vwm_xwindow_t;

/* creates and potentially manages a new window (called in response to CreateNotify events, and during startup for all existing windows) */
/* if the window is already mapped and not an override_redirect window, it becomes managed here. */
typedef enum _vwm_grab_mode_t {
	VWM_NOT_GRABBED = 0,
	VWM_GRABBED
} vwm_grab_mode_t;

void vwm_xwin_message(vwm_t *vwm, vwm_xwindow_t *xwin, Atom type, long foo);
vwm_xwindow_t * vwm_xwin_lookup(vwm_t *vwm, Window win);
int vwm_xwin_is_mapped(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_xwin_monitor(vwm_t *vwm, vwm_xwindow_t *xwin);
vwm_xwindow_t * vwm_xwin_create(vwm_t *vwm, Window win, vwm_grab_mode_t grabbed);
void vwm_xwin_destroy(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_xwin_restack(vwm_t *vwm, vwm_xwindow_t *xwin, Window new_above);
void vwm_xwin_setup_chart(vwm_t *vwm, vwm_xwindow_t *xwin);
int vwm_xwin_create_existing(vwm_t *vwm);


#endif
