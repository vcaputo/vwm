#ifndef _UTIL_H
#define _UTIL_H

#include <stdio.h>
// #include <X11/Xlib.h>
// #include <X11/Xutil.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>

#include "context.h"
#include "list.h"

#define WINDOW_BORDER_WIDTH	1
#define WM_GRAB_MODIFIER	Mod1Mask	/* the modifier for invoking vwm's controls */
						/* Mod4Mask would be the windows key instead of Alt, but there's an assumption
						 * in the code that grabs are being activated by Alt which complicates changing it,
						 * search for XGetModifierMapping to see where, feel free to fix it.  Or you can
						 * just hack the code to expect the appropriate key instead of Alt, I didn't see the
						 * value of making it modifier mapping aware if it's always Alt for me. */

#define CONSOLE_WM_CLASS	"VWMConsoleXTerm"		/* the class we specify to the "console" xterm */
#define CONSOLE_SESSION_STRING	"_vwm_console.$DISPLAY"		/* the unique console screen session identifier */


#define VWM_ERROR(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##_args)
#define VWM_PERROR(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"; %s\n", __FILE__, __LINE__, __FUNCTION__, ##_args, strerror(errno))
#define VWM_BUG(_fmt, _args...)		fprintf(stderr, "BUG %s:%i\t%s() "_fmt"; %s\n", __FILE__, __LINE__, __FUNCTION__, ##_args, strerror(errno))

#ifdef TRACE
#define VWM_TRACE(_fmt, _args...)	fprintf(stderr, "%s:%i\t%s() "_fmt"\n", __FILE__, __LINE__, __FUNCTION__, ##_args)
#else
#define VWM_TRACE(_fmt, _args...)	do { } while(0)
#endif

#define VWM_XROOT(_vwm)			RootWindow((_vwm)->display, (_vwm)->screen_num)
#define VWM_XVISUAL(_vwm)		DefaultVisual((_vwm)->display, (_vwm)->screen_num)
#define VWM_XDEPTH(_vwm)		DefaultDepth((_vwm)->display, (_vwm)->screen_num)

#define MIN(_a, _b)			((_a) < (_b) ? (_a) : (_b))
#define MAX(_a, _b)			((_a) > (_b) ? (_a) : (_b))

typedef struct _vwm_window_t vwm_window_t;
typedef struct _vwm_desktop_t vwm_desktop_t;

typedef struct _vwm_t {
	Display			*display;
	Colormap		cmap;
	int			screen_num;
	GC			gc;
	Atom			wm_delete_atom;
	Atom			wm_protocols_atom;
	Atom			wm_pid_atom;
	int			damage_event, damage_error;

	list_head_t		desktops;		/* global list of all (virtual) desktops in spatial created-in order */
	list_head_t		desktops_mru;		/* global list of all (virtual) desktops in MRU order */
	list_head_t		windows_mru;		/* global list of all managed windows kept in MRU order */
	list_head_t		xwindows;		/* global list of all xwindows kept in the X server stacking order */
	vwm_window_t		*console;		/* the console window */
	vwm_window_t		*focused_origin;	/* the originating window in a grabbed operation/transaction */
	vwm_desktop_t		*focused_desktop;	/* currently focused (virtual) desktop */
	vwm_window_t		*focused_shelf;		/* currently focused shelved window */
	vwm_context_t		focused_context;	/* currently focused context */
	int			priority;		/* scheduling priority of the vwm process, launcher nices relative to this */
	unsigned long		fence_mask;		/* global mask state for vwm_win_focus_next(... VWM_FENCE_MASKED_VIOLATE),
						 * if you use vwm on enough screens to overflow this, pics or it didn't happen. */
	struct colors {
#define color(_sym, _str) \
		XColor			_sym ## _color;
		#include "colors.def"
#undef color
	} colors;

	XineramaScreenInfo	*xinerama_screens;
	int			xinerama_screens_cnt;
} vwm_t;

#endif
