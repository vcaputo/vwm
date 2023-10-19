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
	/* launching of external processes / X clients */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "launch.h"
#include "vwm.h"
#include "window.h"
#include "xwindow.h"

#define LAUNCHED_RELATIVE_PRIORITY	10	/* the wm priority plus this is used as the priority of launched processes */


/* return an interpolated copy of arg */
static char * arg_interpolate(const vwm_t *vwm, const char *arg)
{
	FILE	*memfp;
	char	*xarg = NULL;
	size_t	xarglen;
	int	fmt = 0;

	assert(vwm);
	assert(arg);

	/* this came from vmon.c, it'd be nice if they could share */

	memfp = open_memstream(&xarg, &xarglen);
	if (!memfp) {
		VWM_PERROR("unable to create memstream");
		return NULL;
	}

	for (size_t i = 0; arg[i]; i++) {
		char	c = arg[i];

		if (!fmt) {
			if (c == '%')
				fmt = 1;
			else
				fputc(c, memfp);

			continue;
		}

		switch (c) {
		case 'W': {	/* focused X window id in hex, root window if nothing focused */
			vwm_window_t	*vwin;
			Window		winid;

			vwin = vwm_win_get_focused(vwm);
			if (vwin)
				winid = vwin->xwindow->id;
			else
				winid = VWM_XROOT(vwm);

			fprintf(memfp, "%#x", (unsigned)winid);
			break;
		}

		case '%':	/* literal % */
			fputc(c, memfp);
			break;

		default:
			VWM_ERROR("Unrecognized specifier \'%c\'", c);
			goto _err;
		}

		fmt = 0;
	}

	fclose(memfp);

	return xarg;

_err:
	fclose(memfp);
	free(xarg);

	return NULL;
}


static char ** args_interpolate(vwm_t *vwm, char **argv)
{
	char	**args;
	int	n_args;

	assert(vwm);
	assert(argv);

	for (n_args = 0; argv[n_args]; n_args++);

	args = calloc(n_args + 1, sizeof(*args));
	if (!args)
		return NULL;

	for (int i = 0; i < n_args; i++) {
		args[i] = arg_interpolate(vwm, argv[i]);
		if (!args[i]) {
			for (int j = 0; j < n_args; j++)
				free(args[j]);
			free(args);

			return NULL;
		}
	}

	return args;
}


static void args_free(char **args)
{
	assert(args);

	for (int i = 0; args[i]; i++)
		free(args[i]);
	free(args);
}


/* launch a child command specified in argv, mode decides if we wait for the child to exit before returning. */
void vwm_launch(vwm_t *vwm, char **argv, vwm_launch_mode_t mode)
{
	char	**args;

	args = args_interpolate(vwm, argv);
	if (!args)
		return;

	/* XXX: in BG mode I double fork and let init inherit the orphan so I don't have to collect the return status */
	if (mode == VWM_LAUNCH_MODE_FG || !fork()) {
		if (!fork()) {
			/* child */
			setpriority(PRIO_PROCESS, getpid(), vwm->priority + LAUNCHED_RELATIVE_PRIORITY);
			execvp(args[0], args);
		}
		if (mode == VWM_LAUNCH_MODE_BG)
			exit(0);
	}
	wait(NULL); /* TODO: could wait for the specific pid, particularly in FG mode ... */

	args_free(args);
}
