#ifndef _VWM_H
#define _VWM_H

#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>

#include "context.h"
#include "list.h"
#include "charts.h"
#include "util.h"
#include "xserver.h"

#define WINDOW_BORDER_WIDTH	1
#define WM_GRAB_MODIFIER	Mod1Mask	/* the modifier for invoking vwm's controls */
						/* Mod4Mask would be the windows key instead of Alt, but there's an assumption
						 * in the code that grabs are being activated by Alt which complicates changing it,
						 * search for XGetModifierMapping to see where, feel free to fix it.  Or you can
						 * just hack the code to expect the appropriate key instead of Alt, I didn't see the
						 * value of making it modifier mapping aware if it's always Alt for me. */

#define CONSOLE_WM_CLASS	"VWMConsoleXTerm"		/* the class we specify to the "console" xterm */
#define CONSOLE_SESSION_STRING	"_vwm_console.$DISPLAY"		/* the unique console screen session identifier */

#define VWM_XCMAP(_vwm)		(_vwm)->xserver->cmap
#define VWM_XDISPLAY(_vwm)	(_vwm)->xserver->display
#define VWM_XGC(_vwm)		(_vwm)->xserver->gc
#define VWM_XSCREENNUM(_vwm)	(_vwm)->xserver->screen_num
#define VWM_XROOT(_vwm)		XSERVER_XROOT((_vwm)->xserver)
#define VWM_XVISUAL(_vwm)	XSERVER_XVISUAL((_vwm)->xserver)
#define VWM_XDEPTH(_vwm)	XSERVER_XDEPTH((_vwm)->xserver)

typedef struct _vwm_window_t vwm_window_t;
typedef struct _vwm_desktop_t vwm_desktop_t;

typedef struct _vwm_t {
	vwm_xserver_t		*xserver;		/* global xserver instance */
	vwm_charts_t		*charts;		/* golbal charts instance */

	/* extra X stuff needed by vwm */
	Atom			wm_delete_atom;
	Atom			wm_protocols_atom;
	Atom			wm_pid_atom;
	int			damage_event, damage_error;
	XineramaScreenInfo	*xinerama_screens;
	int			xinerama_screens_cnt;

	int			done;			/* global flag to cause vwm to quit */
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
} vwm_t;

#endif
