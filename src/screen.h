#ifndef _SCREEN_H
#define _SCREEN_H

#include <X11/extensions/Xinerama.h>	/* XINERAMA extension, facilitates easy multihead awareness */

typedef struct _vwm_t vwm_t;
typedef struct _vwm_xwindow_t vwm_xwindow_t;

typedef XineramaScreenInfo vwm_screen_t;					/* conveniently reuse the xinerama type for describing screens */

typedef enum _vwm_screen_rel_t {
	VWM_SCREEN_REL_RECT,		/* return the screen the supplied rectangle x,y,w,h most resides in */
	VWM_SCREEN_REL_XWIN,		/* return the screen the supplied window most resides in */
	VWM_SCREEN_REL_XWIN_NEXT,	/* return the next screen from the one the supplied window most resides in */
	VWM_SCREEN_REL_XWIN_PREV,	/* return the prev screen from the one the supplied window most resides in */
	VWM_SCREEN_REL_POINTER,		/* return the screen the pointer resides in */
	VWM_SCREEN_REL_TOTAL,		/* return the bounding rectangle of all screens as one */
} vwm_screen_rel_t;

const vwm_screen_t * vwm_screen_find(vwm_t *vwm, vwm_screen_rel_t rel, ...);
int vwm_screen_is_empty(vwm_t *vwm, const vwm_screen_t *scr, vwm_xwindow_t *ignore_xwin);

#endif
