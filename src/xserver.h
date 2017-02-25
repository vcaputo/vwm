#ifndef _XSERVER_H
#define _XSERVER_H

#include <X11/Xlib.h>

#define XSERVER_XROOT(_xserver)		RootWindow((_xserver)->display, (_xserver)->screen_num)
#define XSERVER_XVISUAL(_xserver)	DefaultVisual((_xserver)->display, (_xserver)->screen_num)
#define XSERVER_XDEPTH(_xserver)	DefaultDepth((_xserver)->display, (_xserver)->screen_num)

/* bare xserver context, split out for vmon's shared monitoring overlay use sake */
typedef struct vwm_xserver_t {
	Display		*display;
	Colormap	cmap;
	int		screen_num;
	GC		gc;
} vwm_xserver_t;

vwm_xserver_t * vwm_xserver_open(void);
void vwm_xserver_close(vwm_xserver_t *);

#endif
