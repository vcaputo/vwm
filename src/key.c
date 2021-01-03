/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2018  Vito Caputo - <vcaputo@pengaru.com>
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

#include "charts.h"
#include "composite.h"
#include "desktop.h"
#include "launch.h"
#include "list.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

static int		key_is_grabbed;	/* flag for tracking keyboard grab state */
static vwm_direction_t	direction = VWM_DIRECTION_FORWARD;	/* flag for reversing directional actions */
static int		send_it; /* flag for "sending" a migration operation without following it */

/* Poll the keyboard state to see if _any_ keys are pressed */
static int keys_pressed(vwm_t *vwm)
{
	int	i;
	char	state[32];

	XQueryKeymap(VWM_XDISPLAY(vwm), state);

	for (i = 0; i < sizeof(state); i++) {
		if (state[i])
			return 1;
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
			if (key_is_grabbed > 1) {
				key_is_grabbed--;	/* This is important to prevent treating the final lone Alt release as another cancel/rollback. */
				if (vwm->focused_origin) {
					VWM_TRACE("restoring %p on %p", vwm->focused_origin, vwm->focused_origin->desktop);
					vwm_desktop_focus(vwm, vwm->focused_origin->desktop);
					vwm_win_focus(vwm, vwm->focused_origin);
				}
			}

			/* make the focused window the most recently used */
			if ((vwin = vwm_win_get_focused(vwm)))
				vwm_win_mru(vwm, vwin);

			/* make the focused desktop the most recently used */
			vwm_desktop_mru(vwm, vwm->focused_desktop);

			break;

		case XK_r:
			VWM_TRACE("XK_r released with direction=%i", direction);
			direction = VWM_DIRECTION_FORWARD;
			break;

		case XK_s:
			VWM_TRACE("XK_s released with send_it=%i", send_it);
			send_it = 0;
			break;

		default:
			VWM_TRACE("Unhandled keycode: %x", (unsigned int)sym);
			break;
	}

	if (key_is_grabbed && !keys_pressed(vwm)) {
		XUngrabKeyboard(VWM_XDISPLAY(vwm), CurrentTime);
		XFlush(VWM_XDISPLAY(vwm));
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

	sym = XLookupKeysym(keypress, 0);

	/* detect repeaters, note repeaters do not span interrupted Mod1 sequences! */
	if (key_is_grabbed && sym == last_sym && keypress->state == last_state) {
		repeat_cnt++;
	} else {
		repeat_cnt = 0;
	}

	vwin = vwm_win_get_focused(vwm);

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
			if (key_is_grabbed)
				key_is_grabbed++;

			VWM_TRACE("aborting with origin %p", vwm->focused_origin);
			break;

		case XK_r: /* reverse directional actions */
			VWM_TRACE("XK_r pressed with direction=%i", direction);
			direction = VWM_DIRECTION_REVERSE;
			break;

		case XK_s: /* "send" migrational actions */
			VWM_TRACE("XK_s pressed with send_it=%i", send_it);
			send_it = 1;
			break;

		case XK_grave: { /* cycle focused desktop by context */
			vwm_context_t	*next_context;

			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */
			next_context = vwm_context_next_mru(vwm, vwm->focused_desktop->context, direction);

			if (send_it && (keypress->state & ShiftMask)) { /* "send" the focused window to the MRU context's MRU desktop */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, next_context->focused_desktop));
			} else if (send_it) { /* "send" the focused window to a new desktop created on the MRU context */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_desktop_create(vwm, next_context)));
			} else if (keypress->state & ShiftMask) {
				/* migrate the focused window with the desktop focus to the MRU context's MRU desktop */
				if (vwin)
					vwm_win_migrate(vwm, vwin, next_context->focused_desktop);
			} else {
				vwm_desktop_focus(vwm, next_context->focused_desktop);
			}
			break;
		}

		case XK_Tab: /* cycle focused window */
			do_grab = 1; /* update MRU window on commit (Mod1 release) */

			/* focus the next window, note this doesn't affect MRU yet, that happens on Mod1 release */
			if (vwin) {
				if (keypress->state & ShiftMask) {
					/* TODO: in keeping with the Shift==migrate behavior, perhaps
					 * for Tab it should really do a in-desktop migration of sorts
					 * where the focused window swaps places with the next window?
					 */
					 VWM_TRACE("in-desktop migrate not implemented yet");
				} else {
					vwm_win_focus_next(vwm, vwin, direction, VWM_FENCE_RESPECT);
				}
			}
			break;

		case XK_backslash:
			do_grab = 1;

			if (vwin) {
				if (keypress->state & ShiftMask) {
					/* TODO: migrate window to another screen within this desktop,
					 * like VWM_FENCE_MASKED_VIOLATE would focus the next window on
					 * the next screen, but instead of focusing the next window on
					 * the next display, move the focused one to that next desktop.
					 *
					 * since screens are handled within vwm_win_focus_next() via
					 * the fence abstraction, but fences aren't exposed outside of
					 * their, it's non-trivial to implement here.  I may want to
					 * break that out into a more public interface to make things
					 * more composable at the screen level.
					 */
					 VWM_TRACE("migrate window to screen not implemented yet");
				} else {
					vwm_win_focus_next(vwm, vwin, direction, VWM_FENCE_MASKED_VIOLATE);
				}
			}
			break;

		case XK_space: { /* cycle focused desktop utilizing MRU */
			vwm_desktop_t	*next_desktop;

			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */
			next_desktop = vwm_desktop_next_mru(vwm, vwm->focused_desktop, direction);

			if (send_it && (keypress->state & ShiftMask)) { /* "send" the focused window to the MRU desktop */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, next_desktop));
			} else if (send_it) { /* "send" the focused window to a new desktop in the current context, kind of an alias of send_it+XK_v */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_desktop_create(vwm, vwin->desktop->context)));
			} else if (keypress->state & ShiftMask) { /* migrate the focused window with the desktop focus to the most recently used desktop */
				if (vwin)
					vwm_win_migrate(vwm, vwin, next_desktop);
			} else {
				vwm_desktop_focus(vwm, next_desktop);
			}
			break;
		}

		case XK_d: /* destroy focused */
			if (vwin) {
				if (keypress->state & ShiftMask) {  /* brutally destroy the focused window */
					XKillClient(VWM_XDISPLAY(vwm), vwin->xwindow->id);
				} else { /* kindly destroy the focused window */
					vwm_xwin_message(vwm, vwin->xwindow, vwm->wm_protocols_atom, vwm->wm_delete_atom);
				}
			} else {
				/* destroy the focused desktop when destroy occurs without any windows */
				vwm_desktop_destroy(vwm, vwm->focused_desktop);
			}
			break;

		case XK_Escape: /* leave VWM rudely, after triple press */
			do_grab = 1;

			if (repeat_cnt == 2)
				vwm->done = 1;
			break;

		case XK_v: /* instantiate (and focus) a new (potentially empty, unless migrating) virtual desktop */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (send_it) { /* "send" the focused window to a newly created virtual desktop, */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_desktop_create(vwm, vwin->desktop->context)));
			} else if (keypress->state & ShiftMask) { /* migrate the focused window to a newly created virtual desktop, focusing the new desktop simultaneously */
				if (vwin)
					vwm_win_migrate(vwm, vwin, vwm_desktop_create(vwm, vwin->desktop->context));
			} else {
				vwm_desktop_focus(vwm, vwm_desktop_create(vwm, vwm->focused_desktop->context));
			}
			break;

		case XK_c: /* instantiate (and focus) a new (potentialy empty, unless migrating) virtual desktop in a new context */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (send_it) { /* "send" the focused window to a newly created virtual desktop in a new context */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_desktop_create(vwm, NULL)));
			} else if (keypress->state & ShiftMask) { /* migrate the focused window to a newly created virtual desktop in a new context, focusing the new desktop simultaneously */

				if (vwin)
					vwm_win_migrate(vwm, vwin, vwm_desktop_create(vwm, NULL));
			} else {
				vwm_desktop_focus(vwm, vwm_desktop_create(vwm, NULL));
			}
			break;

		case XK_0:
		case XK_1:
		case XK_2:
		case XK_3:
		case XK_4:
		case XK_5:
		case XK_6:
		case XK_7:
		case XK_8:
		case XK_9:
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (send_it && (keypress->state & ShiftMask)) { /* "send" the focused window to the specified context */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_context_by_color(vwm, sym - XK_0)->focused_desktop));
			} else if (send_it) { /* "send" the focused window to a new desktop created on the specified context */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_desktop_create(vwm, vwm_context_by_color(vwm, sym - XK_0))));
			} else if (keypress->state & ShiftMask) { /* migrate the focused window to the specified context */
				if (vwin)
					vwm_win_migrate(vwm, vwin, vwm_context_by_color(vwm, sym - XK_0)->focused_desktop);
			} else {
				vwm_desktop_focus(vwm, vwm_context_by_color(vwm, sym - XK_0)->focused_desktop);
			}
			break;

		case XK_h: /* previous virtual desktop spatially */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (send_it) { /* "send" the focused window to the previous desktop */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_desktop_next(vwm, vwin->desktop, VWM_DIRECTION_REVERSE)));
			} else if (keypress->state & ShiftMask) { /* migrate the focused window with the desktop focus to the previous desktop */
				if (vwin)
					vwm_win_migrate(vwm, vwin, vwm_desktop_next(vwm, vwin->desktop, VWM_DIRECTION_REVERSE));
			} else { /* focus the previous desktop */
				vwm_desktop_focus(vwm, vwm_desktop_next(vwm, vwm->focused_desktop, VWM_DIRECTION_REVERSE));
			}
			break;

		case XK_l: /* next virtual desktop spatially */
			do_grab = 1; /* update MRU desktop on commit (Mod1 release) */

			if (send_it) { /* "send" the focused window to the next desktop */
				if (vwin)
					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, vwm_desktop_next(vwm, vwin->desktop, VWM_DIRECTION_FORWARD)));
			} else if (keypress->state & ShiftMask) { /* migrate the focused window with the desktop focus to the next desktop */
				if (vwin)
					vwm_win_migrate(vwm, vwin, vwm_desktop_next(vwm, vwin->desktop, VWM_DIRECTION_FORWARD));
			} else { /* focus the next desktop */
				vwm_desktop_focus(vwm, vwm_desktop_next(vwm, vwm->focused_desktop, VWM_DIRECTION_FORWARD));
			}
			break;

		case XK_k: /* raise or context-migrate the focused window up */
			if (vwin) {
				do_grab = 1;

				/* TODO: maybe bare send_it should create a new desktop in the next context,
				 * with Shift+send_it being the migrate-like send */
				if (send_it) { /* "send" the focused window to the next context */
					vwm_context_t	*next_context = vwm_context_next(vwm, vwin->desktop->context, VWM_DIRECTION_FORWARD);

					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, next_context->focused_desktop));
				} else if (keypress->state & ShiftMask) { /* migrate the window and focus the new context */
					vwm_context_t	*next_context = vwm_context_next(vwm, vwin->desktop->context, VWM_DIRECTION_FORWARD);

					vwm_win_migrate(vwm, vwin, next_context->focused_desktop);
				} else {
					XRaiseWindow(VWM_XDISPLAY(vwm), vwin->xwindow->id);

					if (repeat_cnt == 1) { /* double: reraise & fullscreen */
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
					} else if (repeat_cnt == 2) { /* triple: reraise & fullscreen w/borders obscured by screen perimiter */
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_ALL);
					} else if (vwm->xinerama_screens_cnt > 1) {
						if (repeat_cnt == 3) { /* triple: reraise & fullscreen across all screens */
							vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_TOTAL, VWM_WIN_AUTOCONF_FULL);
						} else if (repeat_cnt == 4) { /* quadruple: reraise & fullscreen w/borders obscured by screen perimiter */
							vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_TOTAL, VWM_WIN_AUTOCONF_ALL);
						}
					}
					XFlush(VWM_XDISPLAY(vwm));
				}
			}
			break;

		case XK_j: /* lower or context-migrate the focused window down */
			if (vwin) {
				do_grab = 1;

				/* TODO: maybe bare send_it should create a new desktop in the previous context,
				 * with Shift+send_it being the migrate-like send */
				if (send_it) { /* "send" the focused window to the previous context */
					vwm_context_t	*prev_context = vwm_context_next(vwm, vwin->desktop->context, VWM_DIRECTION_REVERSE);

					vwm_win_send(vwm, vwin, vwm_desktop_mru(vwm, prev_context->focused_desktop));
				} else if (keypress->state & ShiftMask) { /* migrate the window and focus the new context */
					vwm_context_t	*prev_context = vwm_context_next(vwm, vwin->desktop->context, VWM_DIRECTION_REVERSE);

					vwm_win_migrate(vwm, vwin, prev_context->focused_desktop);
				} else {
					if (vwin->autoconfigured == VWM_WIN_AUTOCONF_ALL) {
						vwm_win_autoconf(vwm, vwin, VWM_SCREEN_REL_XWIN, VWM_WIN_AUTOCONF_FULL);
					} else {
						XLowerWindow(VWM_XDISPLAY(vwm), vwin->xwindow->id);
					}
					XFlush(VWM_XDISPLAY(vwm));
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

		case XK_semicolon:	/* toggle composited charts */
			vwm_composite_toggle(vwm);
			break;

		case XK_apostrophe:	/* reset snowflakes of the focused window */
			if (vwin && vwin->xwindow->chart) {
				vwm_chart_reset_snowflakes(vwm->charts, vwin->xwindow->chart);
				vwm_composite_damage_win(vwm, vwin->xwindow);
			}
			break;

		case XK_Right:	/* increase sampling frequency */
			vwm_charts_rate_increase(vwm->charts);
			break;

		case XK_Left:	/* decrease sampling frequency */
			vwm_charts_rate_decrease(vwm->charts);
			break;

		default:
			VWM_TRACE("Unhandled keycode: %x", (unsigned int)sym);
			break;
	}

	/* if what we're doing requests a grab, if not already grabbed, grab keyboard */
	if (!key_is_grabbed && do_grab) {
		VWM_TRACE("saving focused_origin of %p", vwin);
		vwm->focused_origin = vwin; /* for returning to on abort */
		XGrabKeyboard(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), False, GrabModeAsync, GrabModeAsync, CurrentTime);
		key_is_grabbed = 1;
	}

	/* remember the symbol for repeater detection */
	last_sym = sym;
	last_state = keypress->state;
}
