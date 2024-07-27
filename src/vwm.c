/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2018  Vito Caputo - <vcaputo@pengaru.com>
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
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
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/extensions/sync.h>	/* SYNC extension, enables us to give vwm the highest X client priority, helps keep vwm responsive at all times */
#include <X11/extensions/Xinerama.h>	/* XINERAMA extension, facilitates easy multihead awareness */
#include <X11/extensions/Xrandr.h>	/* RANDR extension, facilitates display configuration change awareness */
#include <X11/extensions/Xdamage.h>	/* Damage extension, enables receipt of damage events, reports visible regions needing updating (compositing) */
#include <X11/extensions/Xrender.h>     /* Render extension, enables use of alpha channels and accelerated rendering of surfaces having alpha (compositing) */
#include <X11/extensions/Xcomposite.h>	/* Composite extension, enables off-screen redirection of window rendering (compositing) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <values.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <poll.h>

#include "charts.h"
#include "composite.h"
#include "desktop.h"
#include "launch.h"
#include "logo.h"
#include "vwm.h"
#include "xevent.h"
#include "xwindow.h"

	/* Sync */
static int	sync_event, sync_error;

	/* Xinerama */
static int	xinerama_event, xinerama_error;
static int	randr_event, randr_error;

	/* Compositing */
static int	composite_event, composite_error, composite_opcode;


static vwm_t * vwm_startup(void)
{
	Cursor	pointer;
	char	*console_args[] = {"xterm", "-class", CONSOLE_WM_CLASS, "-e", "/bin/sh", "-c", "screen -D -RR " CONSOLE_SESSION_STRING, NULL};
	vwm_t	*vwm;

	if (!(vwm = calloc(1, sizeof(vwm_t)))) {
		VWM_ERROR("Unable to allocate vwm_t");
		goto _err;
	}

	INIT_LIST_HEAD(&vwm->contexts);
	INIT_LIST_HEAD(&vwm->contexts_mru);
	INIT_LIST_HEAD(&vwm->desktops);
	INIT_LIST_HEAD(&vwm->desktops_mru);
	INIT_LIST_HEAD(&vwm->windows_mru);
	INIT_LIST_HEAD(&vwm->xwindows);

	if (!(vwm->xserver = vwm_xserver_open())) {
		VWM_ERROR("Failed to open xserver");
		goto _err_free;
	}

	if (!(vwm->vcr_backend = vcr_backend_new(VCR_BACKEND_TYPE_XLIB, vwm->xserver))) {
		VWM_ERROR("Failed to create vcr backend");
		goto _err_xclose;
	}

	if (!(vwm->charts = vwm_charts_create(vwm->vcr_backend, 0))) {
		VWM_ERROR("Failed to create charts");
		goto _err_vbe;
	}

	/* query the needed X extensions */
	if (!XQueryExtension(VWM_XDISPLAY(vwm), COMPOSITE_NAME, &composite_opcode, &composite_event, &composite_error)) {
		VWM_ERROR("No composite extension available");
		goto _err_charts;
	}

	if (!XDamageQueryExtension(VWM_XDISPLAY(vwm), &vwm->damage_event, &vwm->damage_error)) {
		VWM_ERROR("No damage extension available");
		goto _err_charts;
	}

	if (XSyncQueryExtension(VWM_XDISPLAY(vwm), &sync_event, &sync_error)) {
		/* set the window manager to the maximum X client priority */
		XSyncSetPriority(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), 0x7fffffff);
	}

	if (XineramaQueryExtension(VWM_XDISPLAY(vwm), &xinerama_event, &xinerama_error)) {
		vwm->xinerama_screens = XineramaQueryScreens(VWM_XDISPLAY(vwm), &vwm->xinerama_screens_cnt);
	}

	if (XRRQueryExtension(VWM_XDISPLAY(vwm), &randr_event, &randr_error)) {
		XRRSelectInput(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), RRScreenChangeNotifyMask);
	}

	/* get our scheduling priority, clients are launched with a priority LAUNCHED_RELATIVE_PRIORITY nicer than this */
	if ((vwm->priority = getpriority(PRIO_PROCESS, getpid())) == -1) {
		VWM_ERROR("Cannot get scheduling priority");
		goto _err_charts;
	}

	vwm->wm_delete_atom = XInternAtom(VWM_XDISPLAY(vwm), "WM_DELETE_WINDOW", False);
	vwm->wm_protocols_atom = XInternAtom(VWM_XDISPLAY(vwm), "WM_PROTOCOLS", False);
	vwm->wm_pid_atom = XInternAtom(VWM_XDISPLAY(vwm), "_NET_WM_PID", False);

	/* allocate colors, I make assumptions about the X server's color capabilities since I'll only use this on modern-ish computers... */
#define color(_sym, _str) \
	XAllocNamedColor(VWM_XDISPLAY(vwm), VWM_XCMAP(vwm), _str, &vwm->colors._sym ## _color, &vwm->colors._sym ## _color);
#include "colors.def"
#undef color

#define color(_num, _str) \
	XAllocNamedColor(VWM_XDISPLAY(vwm), VWM_XCMAP(vwm), _str, &vwm->context_colors[_num], &vwm->context_colors[_num]);
#include "context_colors.def"
#undef color

	XSelectInput(VWM_XDISPLAY(vwm), VWM_XROOT(vwm),
		     FocusChangeMask | PropertyChangeMask | SubstructureNotifyMask | SubstructureRedirectMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
	XGrabKey(VWM_XDISPLAY(vwm), AnyKey, WM_GRAB_MODIFIER, VWM_XROOT(vwm), False, GrabModeAsync, GrabModeAsync);

	XFlush(VWM_XDISPLAY(vwm));

	XSetInputFocus(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), RevertToPointerRoot, CurrentTime);

	/* create initial contexts and desktop */
	vwm_desktop_create(vwm, NULL); /* shelf */
	vwm_desktop_focus(vwm, vwm_desktop_create(vwm, NULL)); /* general */
	vwm_desktop_mru(vwm, vwm->focused_desktop);

	/* manage all preexisting windows */
	vwm_xwin_create_existing(vwm);

	/* setup GC for logo drawing and window rubber-banding */
	XSetSubwindowMode(VWM_XDISPLAY(vwm), VWM_XGC(vwm), IncludeInferiors);
	XSetFunction(VWM_XDISPLAY(vwm), VWM_XGC(vwm), GXxor);

	/* launch the console here so it's likely ready by the time the logo animation finishes (there's no need to synchronize with it currently) */
	vwm_launch(vwm, console_args, VWM_LAUNCH_MODE_BG);

	/* first the logo color is the foreground */
	XSetForeground(VWM_XDISPLAY(vwm), VWM_XGC(vwm), vwm->colors.logo_color.pixel);
	vwm_draw_logo(vwm);

	/* change to the rubber-banding foreground color */
	XSetForeground(VWM_XDISPLAY(vwm), VWM_XGC(vwm), vwm->colors.rubberband_color.pixel);

	XClearWindow(VWM_XDISPLAY(vwm), VWM_XROOT(vwm));

	/* set the pointer */
	pointer = XCreateFontCursor(VWM_XDISPLAY(vwm), XC_X_cursor);
	XDefineCursor(VWM_XDISPLAY(vwm), VWM_XROOT(vwm), pointer);

	return vwm;

_err_charts:
	vwm_charts_destroy(vwm->charts);

_err_vbe:
	vcr_backend_free(vwm->vcr_backend);

_err_xclose:
	vwm_xserver_close(vwm->xserver);

_err_free:
	free(vwm);

_err:
	return NULL;
}


void vwm_shutdown(vwm_t *vwm)
{
	char	*quit_console_args[] = {"/bin/sh", "-c", "screen -dr " CONSOLE_SESSION_STRING " -X quit", NULL};

	vwm_launch(vwm, quit_console_args, VWM_LAUNCH_MODE_FG);
	vwm_charts_destroy(vwm->charts);
	vwm_xserver_close(vwm->xserver);
	/* TODO there's more shit to cleanup here, but we're exiting anyways. */
	free(vwm);
}


void vwm_process_event(vwm_t *vwm)
{
	XEvent	event;

	XNextEvent(VWM_XDISPLAY(vwm), &event);
	switch (event.type) {
		case KeyPress:
			VWM_TRACE_WIN(event.xkey.window, "keypress");
			vwm_xevent_handle_key_press(vwm, &event.xkey);
			break;

		case KeyRelease:
			VWM_TRACE_WIN(event.xkey.window, "keyrelease");
			vwm_xevent_handle_key_release(vwm, &event.xkey);
			break;

		case ButtonPress:
			VWM_TRACE_WIN(event.xbutton.window, "buttonpresss");
			vwm_xevent_handle_button_press(vwm, &event.xbutton);
			break;

		case MotionNotify:
			//VWM_TRACE_WIN(event.xmotion.window, "motionnotify");
			vwm_xevent_handle_motion_notify(vwm, &event.xmotion);
			break;

		case ButtonRelease:
			VWM_TRACE_WIN(event.xbutton.window, "buttonrelease");
			vwm_xevent_handle_button_release(vwm, &event.xbutton);
			break;

		case CreateNotify:
			VWM_TRACE_WIN(event.xcreatewindow.window, "createnotify");
			vwm_xevent_handle_create_notify(vwm, &event.xcreatewindow);
			break;

		case DestroyNotify:
			VWM_TRACE_WIN(event.xdestroywindow.window, "destroynotify");
			vwm_xevent_handle_destroy_notify(vwm, &event.xdestroywindow);
			break;

		case ConfigureRequest:
			VWM_TRACE_WIN(event.xconfigurerequest.window, "configurerequest");
			vwm_xevent_handle_configure_request(vwm, &event.xconfigurerequest);
			break;

		case ConfigureNotify:
			VWM_TRACE_WIN(event.xconfigure.window, "configurenotify");
			vwm_xevent_handle_configure_notify(vwm, &event.xconfigure);
			break;

		case UnmapNotify:
			VWM_TRACE_WIN(event.xunmap.window, "unmapnotify");
			vwm_xevent_handle_unmap_notify(vwm, &event.xunmap);
			break;

		case MapNotify:
			VWM_TRACE_WIN(event.xmap.window, "mapnotify");
			vwm_xevent_handle_map_notify(vwm, &event.xmap);
			break;

		case MapRequest:
			VWM_TRACE_WIN(event.xmaprequest.window, "maprequest");
			vwm_xevent_handle_map_request(vwm, &event.xmaprequest);
			break;

		case PropertyNotify:
			VWM_TRACE_WIN(event.xproperty.window, "property notify");
			vwm_xevent_handle_property_notify(vwm, &event.xproperty);
			break;

		case FocusIn:
			VWM_TRACE_WIN(event.xfocus.window, "focusin");
			vwm_xevent_handle_focusin(vwm, &event.xfocus);
			break;

		case FocusOut:
			VWM_TRACE_WIN(event.xfocus.window, "focusout");
			break;

		case MappingNotify:
			VWM_TRACE_WIN(event.xmapping.window, "mapping notify");
			vwm_xevent_handle_mapping_notify(vwm, &event.xmapping);
			break;

		case Expose:
			VWM_TRACE_WIN(event.xexpose.window, "expose");
			break;

		case GravityNotify:
			VWM_TRACE_WIN(event.xgravity.window, "gravitynotify");
			break;

		case ReparentNotify:
			VWM_TRACE_WIN(event.xreparent.window, "reparentnotify");
			break;

		default:
			if (event.type == randr_event + RRScreenChangeNotify) {
				if (vwm->xinerama_screens)
					XFree(vwm->xinerama_screens);
				vwm->xinerama_screens = XineramaQueryScreens(VWM_XDISPLAY(vwm), &vwm->xinerama_screens_cnt);

				vwm_composite_invalidate_root(vwm);
			} else if (event.type == vwm->damage_event + XDamageNotify) {
				//VWM_TRACE("damagenotify");
				vwm_composite_damage_event(vwm, (XDamageNotifyEvent *)&event);
			} else {
				VWM_ERROR("Unhandled X op %i", event.type);
			}
			break;
	}
}


int main(int argc, char *argv[])
{
	vwm_t		*vwm;
	struct pollfd	pfd;

	if (!(vwm = vwm_startup())) {
		VWM_ERROR("Unable to startup vwm");
		goto _err;
	}

	pfd.events = POLLIN;
	pfd.revents = 0;
	pfd.fd = ConnectionNumber(VWM_XDISPLAY(vwm));

	while (!vwm->done) {
		do {
			int	delay;

			if (vwm_charts_update(vwm->charts, &delay))
				vwm_composite_repaint_needed(vwm);

			if (!XPending(VWM_XDISPLAY(vwm))) {
				if (poll(&pfd, 1, delay) == 0)
					break;
			}

			vwm_process_event(vwm);
		} while (QLength(VWM_XDISPLAY(vwm)));

		vwm_composite_paint_all(vwm);
	}

	vwm_shutdown(vwm);

	return EXIT_SUCCESS;

_err:
	return EXIT_FAILURE;
}
