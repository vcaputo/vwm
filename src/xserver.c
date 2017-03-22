/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2017  Vito Caputo - <vcaputo@gnugeneration.com>
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
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"
#include "xserver.h"

/* xserver context shared by vwm and vmon.
 * This is the X part monitor.c interfaces with, so monitor.c can avoid depending
 * on vwm_t which won't exist in the vmon tool.
 */

static int errhandler(Display *display, XErrorEvent *err)
{
	/* TODO */
	return 1;
}

/* open the X display server, initializing *xserver on success. */
vwm_xserver_t * vwm_xserver_open(void)
{
	vwm_xserver_t	*xserver;

	if (!(xserver = calloc(sizeof(vwm_xserver_t), 1))) {
		VWM_ERROR("Cannot allocate vwm_xserver_t");
		goto _err;
	}

	if (!(xserver->display = XOpenDisplay(NULL))) {
		VWM_ERROR("Cannot open display");
		goto _err_free;
	}

	/* prevent children from inheriting the X connection */
	if (fcntl(ConnectionNumber(xserver->display), F_SETFD, FD_CLOEXEC) < 0) {
		VWM_ERROR("Cannot set FD_CLOEXEC on X connection");
		goto _err_xclose;
	}

	XSetErrorHandler(errhandler); /* TODO, this may not belong here. */

	xserver->screen_num = DefaultScreen(xserver->display);
	xserver->gc = XCreateGC(xserver->display, XSERVER_XROOT(xserver), 0, NULL);
	xserver->cmap = DefaultColormap(xserver->display, xserver->screen_num);

	return xserver;


_err_xclose:
	XCloseDisplay(xserver->display);

_err_free:
	free(xserver);

_err:
	return NULL;
}


/* close on opened xserver */
void vwm_xserver_close(vwm_xserver_t *xserver)
{
	XFlush(xserver->display);
	XCloseDisplay(xserver->display);
	free(xserver);
}
