/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2016  Vito Caputo - <vcaputo@gnugeneration.com>
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 3 as published
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

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdlib.h>

#include "composite.h"
#include "desktop.h"
#include "launch.h"
#include "list.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

static int	key_is_grabbed;	/* flag for tracking keyboard grab state */

/* Poll the keyboard state to see if _any_ keys are pressed */
static int keys_pressed(vwm_t *vwm) {
	int	i;
	char	state[32];

	XQueryKeymap(vwm->display, state);

	for (i = 0; i < sizeof(state); i++) {
		if (state[i]) return 1;
	}

	return 0;
}

/* Called in response to KeyRelease events, for now only interesting for detecting when Mod1 is released termintaing
 * window cycling for application of MRU on the focused window */
void vwm_key_released(vwm_t *vwm, Window win, XKeyReleasedEvent *keyrelease)
{
	vwm_window_t	*vwin;
	KeySym		sym;

	switch ((sym = XLookupKeysym(keyrelease, 0))) {
		case XK_Alt_R:
		case XK_Alt_L:	/* TODO: actually use the modifier mapping, for me XK_Alt_[LR] is Mod1.  XGetModifierMapping()... */
			VWM_TRACE("XK_Alt_[LR] released");

			/* aborted? try restore focused_origin */
			if (key_is_grabbed > 1 && vwm->focused_origin) {
				VWM_TRACE("restoring %p on %p", vwm->focused_origin, vwm->focused_origin->desktop);
				vwm_desktop_focus(vwm, vwm->focused_origin->desktop);
				vwm_win_focus(vwm, vwm->focused_origin);
			}

			/* make the focused window the most recently used */
			if ((vwin = vwm_win_focused(vwm))) vwm_win_mru(vwm, vwin);

			/* make the focused desktop the most recently used */
			if (vwm->focused_context == VWM_CONTEXT_DESKTOP && vwm->focused_desktop) vwm_desktop_mru(vwm, vwm->focused_desktop);

			break;

		default:
			VWM_TRACE("Unhandled keycode: %x", (unsigned int)sym);
			break;
	}

	if (key_is_grabbed && !keys_pressed(vwm)) {
		XUngrabKeyboard(vwm->display, CurrentTime);
		XFlush(vwm->display);
		key_is_grabbed = 0;
		vwm->fence_mask = 0;	/* reset the fence mask on release for VWM_FENCE_MASKED_VIOLATE */
	}
}


/* Called in response to KeyPress events, I currenly only grab Mod1 keypress events */
void vwm_key_pressed(vwm_t *vwm, Window win, XKeyPressedEvent *keypress)
{
	vwm_window_t				*vwin;
	KeySym					sym;
	static KeySym				last_sym;
	static typeof(keypress->state)		last_state;
	static int				repeat_cnt = 0;
	int					do_grab = 0;
	char					*quit_console_args[] = {"/bin/sh", "-c", "screen -dr " CONSOLE_SESSION_STRING " -X quit", NULL};

	sym = XLookupKeysym(keypress, 0);

	/* detect repeaters, note repeaters do not span interrupted Mod1 sequences! */
	if (key_is_grabbed && sym == last_sym && keypress->state == last_state) {
		repeat_cnt++;
	} else {
		repeat_cnt = 0;
	}

	vwin = vwm_win_focused(vwm);

	switch (sym) {

#define launcher(_sym, _label, _argv)\
		case _sym:	\
			{	\
			char	*args[] = {"/bin/sh", "-c", "screen -dr " CONSOLE_SESSION_STRING " -X screen /bin/sh -i -x -c \"" _argv " || sleep 86400\"", NULL};\
			vwm_launch(vwm, args, VWM_LAUNCH_MODE_BG);\
			break;	\
		}
#include "launchers.def"
#undef launcher
		case XK_Alt_L: /* transaction abort */
		case XK_Alt_R:
			if (key_is_grabbed) key_is_grabbed++;
			VWM_TRACE("aborting with origin %p", vwm->focused_origin);
			break;

		case XK_grave: /* toggle shelf visibility */
			vwm_context_focus(vwm, VWM_CONTEXT_OTHER);
			break;

		case XK_Tab: /* cycle focused window */
			do_grab = 1; /* update MRU window on commit (Mod1 release) */

			/* focus the next window, note this doesn't affect MRU yet, that happens on Mod1 release */
			if (vwin) {
				if (keypress->state & ShiftMask) {
					vwm_win_focus_next(vwm, vwin, VWM_FENCE_MASKED_VIOLATE);
				} else {
					vwm_win_focus_next(vwm, vwin, VWM_FENCE_RESPECT);
				}
			}
			break;

		case XK_space: { /* cycle focused desktop utilizing MRU */
			vwm_desktop_t	*next_desktop = vwm_desktop_next_mru(vwm, vwm->focused_desktop);

			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (keypress->state & ShiftMask) {
				/* migrate the focused window with the desktop focus to the most recently used desktop */
				if (vwin) vwm_win_migrate(vwm, vwin, next_desktop);
			} else {
				vwm_desktop_focus(vwm, next_desktop);
			}
			break;
		}

		case XK_d: /* destroy focused */
			if (vwin) {
				if (keypress->state & ShiftMask) {  /* brutally destroy the focused window */
					XKillClient(vwm->display, vwin->xwindow->id);
				} else { /* kindly destroy the focused window */
					vwm_xwin_message(vwm, vwin->xwindow, vwm->wm_protocols_atom, vwm->wm_delete_atom);
				}
			} else if (vwm->focused_context == VWM_CONTEXT_DESKTOP) {
				/* destroy the focused desktop when destroy occurs without any windows */
				vwm_desktop_destroy(vwm, vwm->focused_desktop);
			}
			break;

		case XK_Escape: /* leave VWM rudely, after triple press */
			do_grab = 1;

			if (repeat_cnt == 2) {
				vwm_launch(vwm, quit_console_args, VWM_LAUNCH_MODE_FG);
				exit(42);
			}
			break;

		case XK_v: /* instantiate (and focus) a new (potentially empty, unless migrating) virtual desktop */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (keypress->state & ShiftMask) {
				if (vwin) {
					/* migrate the focused window to a newly created virtual desktop, focusing the new desktop simultaneously */
					vwm_win_migrate(vwm, vwin, vwm_desktop_create(vwm, NULL));
				}
			} else {
				vwm_desktop_focus(vwm, vwm_desktop_create(vwm, NULL));
				vwm_desktop_mru(vwm, vwm->focused_desktop);
			}
			break;

		case XK_h: /* previous virtual desktop, if we're in the shelf context this will simply switch to desktop context */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (keypress->state & ShiftMask) {
				if (vwin) {
					/* migrate the focused window with the desktop focus to the previous desktop */
					vwm_win_migrate(vwm, vwin, vwm_desktop_prev(vwm, vwin->desktop));
				}
			} else {
				if (vwm->focused_context == VWM_CONTEXT_SHELF) {
					/* focus the focused desktop instead of the shelf */
					vwm_context_focus(vwm, VWM_CONTEXT_DESKTOP);
				} else {
					/* focus the previous desktop */
					vwm_desktop_focus(vwm, vwm_desktop_prev(vwm, vwm->focused_desktop));
				}
			}
			break;

		case XK_l: /* next virtual desktop, if we're in the shelf context this will simply switch to desktop context */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (keypress->state & ShiftMask) {
				if (vwin) {
					/* migrate the focused window with the desktop focus to the next desktop */
					vwm_win_migrate(vwm, vwin, vwm_desktop_next(vwm, vwin->desktop));
				}
			} else {
				if (vwm->focused_context == VWM_CONTEXT_SHELF) {
					/* focus the focused desktop instead of the shelf */
					vwm_context_focus(vwm, VWM_CONTEXT_DESKTOP);
				} else {
					/* focus the next desktop */
					vwm_desktop_focus(vwm, vwm_desktop_next(vwm, vwm->focused_desktop));
				}
			}
			break;

		case XK_k: /* raise or shelve the focused window */
			if (vwin) {
				if (keypress->state & ShiftMask) { /* shelf the window and focus the shelf */
					if (vwm->focused_context != VWM_CONTEXT_SHELF) {
						/* shelve the focused window while focusing the shelf */
						vwm_win_shelve(vwm, vwin);
						vwm_context_focus(vwm, VWM_CONTEXT_SHELF);
					}
				} else {
					do_grab = 1;

					XRaiseWindow(vwm->display, vwin->xwindow->id);

					if (repeat_cnt == 1) {
						/* double: reraise & fullscreen */
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
					} else if (repeat_cnt == 2) {
						 /* triple: reraise & fullscreen w/borders obscured by screen perimiter */
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_ALL);
					} else if (vwm->xinerama_screens_cnt > 1) {
						if (repeat_cnt == 3) {
							 /* triple: reraise & fullscreen across all screens */
							vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_TOTAL, VWM_WIN_AUTOCONF_FULL);
						} else if (repeat_cnt == 4) {
							 /* quadruple: reraise & fullscreen w/borders obscured by screen perimiter */
							vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_TOTAL, VWM_WIN_AUTOCONF_ALL);
						}
					}
					XFlush(vwm->display);
				}
			}
			break;

		case XK_j: /* lower or unshelve the focused window */
			if (vwin) {
				if (keypress->state & ShiftMask) { /* unshelf the window to the focused desktop, and focus the desktop */
					if (vwm->focused_context == VWM_CONTEXT_SHELF) {
						/* unshelve the focused window, focus the desktop it went to */
						vwm_win_migrate(vwm, vwin, vwm->focused_desktop);
					}
				} else {
					if (vwin->autoconfigured == VWM_WIN_AUTOCONF_ALL) {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
					} else {
						XLowerWindow(vwm->display, vwin->xwindow->id);
					}
					XFlush(vwm->display);
				}
			}
			break;

		case XK_Return: /* (full-screen / restore) focused window */
			if (vwin) {
				if (vwin->autoconfigured) {
					vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_NONE);
				} else {
					vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
				}
			}
			break;

		case XK_s: /* shelve focused window */
			if (vwin && !vwin->shelved) vwm_win_shelve(vwm, vwin);
			break;

		case XK_bracketleft:	/* reconfigure the focused window to occupy the left or top half of the screen or left quarters on repeat */
			if (vwin) {
				do_grab = 1;

				if (keypress->state & ShiftMask) {
					if (!repeat_cnt) {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_TOP);
					} else {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_TOP_LEFT);
					}
				} else {
					if (!repeat_cnt) {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_LEFT);
					} else {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_BOTTOM_LEFT);
					}
				}
			}
			break;

		case XK_bracketright:	/* reconfigure the focused window to occupy the right or bottom half of the screen or right quarters on repeat */
			if (vwin) {
				do_grab = 1;

				if (keypress->state & ShiftMask) {
					if (!repeat_cnt) {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_BOTTOM);
					} else {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_BOTTOM_RIGHT);
					}
				} else {
					if (!repeat_cnt) {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_HALF, VWM_SIDE_RIGHT);
					} else {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_QUARTER, VWM_CORNER_TOP_RIGHT);
					}
				}
			}
			break;

		case XK_semicolon:	/* toggle composited overlays */
			vwm_composite_toggle(vwm);
			break;

		case XK_apostrophe:	/* reset snowflakes of the focused window, suppressed when not compositing */
			if (vwin) {
				vwm_overlay_xwin_reset_snowflakes(vwm, vwin->xwindow);
			}
			break;

		case XK_Right:	/* increase sampling frequency */
			vwm_overlay_rate_increase(vwm);
			break;

		case XK_Left:	/* decrease sampling frequency */
			vwm_overlay_rate_decrease(vwm);
			break;

		default:
			VWM_TRACE("Unhandled keycode: %x", (unsigned int)sym);
			break;
	}

	/* if what we're doing requests a grab, if not already grabbed, grab keyboard */
	if (!key_is_grabbed && do_grab) {
		VWM_TRACE("saving focused_origin of %p", vwin);
		vwm->focused_origin = vwin; /* for returning to on abort */
		XGrabKeyboard(vwm->display, VWM_XROOT(vwm), False, GrabModeAsync, GrabModeAsync, CurrentTime);
		key_is_grabbed = 1;
	}

	/* remember the symbol for repeater detection */
	last_sym = sym;
	last_state = keypress->state;
}
