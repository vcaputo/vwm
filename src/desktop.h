#ifndef _DESKTOP_H
#define _DESKTOP_H

#include "direction.h"
#include "list.h"
#include "window.h"

typedef struct _vwm_t vwm_t;
typedef struct _vwm_window_t vwm_window_t;

typedef struct _vwm_desktop_t {
	list_head_t	desktops;		/* global list of (virtual) desktops */
	list_head_t	desktops_mru;		/* global list of (virtual) desktops in MRU order */
	vwm_window_t	*focused_window;	/* the focused window on this virtual desktop */
} vwm_desktop_t;

void vwm_desktop_mru(vwm_t *vwm, vwm_desktop_t *desktop);
int vwm_desktop_focus(vwm_t *vwm, vwm_desktop_t *desktop);
vwm_desktop_t * vwm_desktop_create(vwm_t *vwm);
void vwm_desktop_destroy(vwm_t *vwm, vwm_desktop_t *desktop);
vwm_desktop_t * vwm_desktop_next_mru(vwm_t *vwm, vwm_desktop_t *desktop, vwm_direction_t direction);
vwm_desktop_t * vwm_desktop_next(vwm_t *vwm, vwm_desktop_t *desktop, vwm_direction_t direction);

#endif
