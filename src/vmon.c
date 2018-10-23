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
#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "charts.h"
#include "util.h"
#include "xserver.h"

/* vmon exposes the monitoring charts to the shell in an strace-like cli */

typedef struct vmon_t {
	vwm_xserver_t	*xserver;
	vwm_charts_t	*charts;
	vwm_chart_t	*chart;
	Window		window;
	int		width, height;
	Picture		picture;
	int		pid;
	int		done;
	int		linger;
} vmon_t;


/* these are fairly arbitrarily chosen to be sane */
#define WIDTH_DEFAULT	800
#define HEIGHT_DEFAULT	600
#define WIDTH_MIN	200
#define HEIGHT_MIN	28

static volatile int got_sigchld;

/* return if arg == flag or altflag if provided */
static int is_flag(const char *arg, const char *flag, const char *altflag)
{
	assert(arg);
	assert(flag);

	if (!strcmp(arg, flag) || (altflag && !strcmp(arg, altflag)))
		return 1;

	return 0;
}


/* parse integer out of opt, stores parsed opt in *res on success */
static int parse_flag_int(char * const *flag, char * const *end, char * const *opt, int min, int max, int *res)
{
	long int	num;
	char		*endp;

	assert(flag);
	assert(end);
	assert(opt);
	assert(res);

	if (flag == end || **opt == '\0') {
		VWM_ERROR("flag \"%s\" expects an integer argument", *flag);
		return 0;
	}

	errno = 0;
	num = strtol(*opt, &endp, 10);
	if (errno) {
		VWM_PERROR("flag \"%s\" integer argument parse error, got \"%s\"", *flag, *opt);
		return 0;
	}

	if (*endp != '\0') {
		VWM_ERROR("flag \"%s\" integer argument invalid at \"%s\"", *flag, endp);
		return 0;
	}

	if (num < min) {
		VWM_ERROR("flag \"%s\" integer argument must be >= %i, got \"%s\"", *flag, min, *opt);
		return 0;
	}

	if (num > max) {
		VWM_ERROR("flag \"%s\" integer argument must be <= %i, got \"%s\"", *flag, max, *opt);
		return 0;
	}

	*res = num;

	return 1;
}


/* set vmon->{width,height} to fullscreen dimensions */
static int set_fullscreen(vmon_t *vmon)
{
	XWindowAttributes	wattr;

	assert(vmon);
	assert(vmon->xserver);

	if (!XGetWindowAttributes(vmon->xserver->display, XSERVER_XROOT(vmon->xserver), &wattr)) {
		VWM_ERROR("unable to get root window attributes");
		return 0;
	}

	vmon->width = wattr.width;
	vmon->height = wattr.height;

	return 1;
}


/* print commandline help */
static void print_help(void)
{
	puts(
		"\n"
		"-----------------------------------------------------------------------------\n"
		" Flag              Description\n"
		"-----------------------------------------------------------------------------\n"
		" --                Sentinel, subsequent arguments form command to execute\n"
		" -f  --fullscreen  Fullscreen window\n"
		" -h  --help        Show this help\n"
		" -l  --linger      Don't exit when top-level process exits\n"
		" -p  --pid         PID of the top-level process to monitor (1 if unspecified)\n"
		" -x  --width       Window width\n"
		" -y  --height      Window height\n"
		" -z  --hertz       Sample rate in hertz\n"
		"-----------------------------------------------------------------------------"
	);
}


/* print copyright */
static void print_copyright(void)
{
	puts(
		"\n"
		"Copyright (C) 2012-2018 Vito Caputo <vcaputo@pengaru.com>\n"
		"\n"
		"This program comes with ABSOLUTELY NO WARRANTY.  This is free software, and\n"
		"you are welcome to redistribute it under certain conditions.  For details\n"
		"please see the LICENSE file included with this program."
	);
}


/* collect status of child */
static void handle_sigchld(int signum)
{
	got_sigchld = 1;
}


/* parse and apply argv, implementing an strace-like cli, mutates vmon. */
static int vmon_handle_argv(vmon_t *vmon, int argc, char * const argv[])
{
	char	*const*end = &argv[argc - 1], *const*last = argv;

	assert(vmon);
	assert(!vmon->pid);
	assert(argc > 0);
	assert(argv);

	for (argv++; argv <= end; argv++) {
		if (is_flag(*argv, "-p", "--pid")) {
			if (vmon->pid) {
				VWM_ERROR("--pid may only be specified once currently (TODO)");
				return 0;
			}

			if (!parse_flag_int(argv, end, argv + 1, 0, INT_MAX, &vmon->pid))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-x", "--width")) {
			if (!parse_flag_int(argv, end, argv + 1, WIDTH_MIN, INT_MAX, &vmon->width))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-y", "--height")) {
			if (!parse_flag_int(argv, end, argv + 1, HEIGHT_MIN, INT_MAX, &vmon->height))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-f", "--fullscreen")) {
			if (!set_fullscreen(vmon)) {
				VWM_ERROR("unable to set fullscreen dimensions");
				return 0;
			}

			last = argv;
		} else if (is_flag(*argv, "-l", "--linger")) {
			vmon->linger = 1;
			last = argv;
		} else if (is_flag(*argv, "--", NULL)) {
			/* stop looking for more flags, anything remaining will be used as a command. */
			last = argv;
			break;
		} else if (is_flag(*argv, "-z", "--hertz")) {
			int	hertz;

			if (!parse_flag_int(argv, end, argv + 1, 1, 1000, &hertz))
				return 0;

			vwm_charts_rate_set(vmon->charts, hertz);

			last = ++argv;
		} else if (is_flag(*argv, "-h", "--help")) {
			print_help();
			exit(EXIT_SUCCESS);
		} else {
			/* stop looking for more flags on first unrecognized thing, assume we're in command territory now */
			break;
		}
	}

	/* if more argv remains, treat as a command to run */
	if (last != end) {
		pid_t	pid;

		if (vmon->pid) {
			VWM_ERROR("combining --pid with a command to run is not yet supported (TODO)\n");
			return 0;
		}

		last++;

		if (signal(SIGCHLD, handle_sigchld) == SIG_ERR) {
			VWM_PERROR("unable to set SIGCHLD handler");
			return 0;
		}

		pid = fork();
		if (pid == -1) {
			VWM_PERROR("unable to fork");
			return 0;
		}

		if (pid == 0) {
			if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
				VWM_PERROR("unable to prctl(PR_SET_PDEATHSIG, SIGKILL)");
				exit(EXIT_FAILURE);
			}

			/* TODO: would be better to synchronize with the monitoring loop starting before the execvp() occurs,
			 * very early program start can be an interesting thing to observe. */
			if (execvp(*last, last) == -1) {
				VWM_PERROR("unable to exec \"%s\"", *last);
				exit(EXIT_FAILURE);
			}
		}

		vmon->pid = pid;
	}

	/* default to PID 1 when no PID or command was supplied */
	if (!vmon->pid)
		vmon->pid = 1;

	return 1;
}


/* parse argv, connect to X, create window, attach libvmon to monitored process */
static vmon_t * vmon_startup(int argc, char * const argv[])
{
	vmon_t				*vmon;
	XRenderPictureAttributes	pattr = {};
	XWindowAttributes		wattr;

	assert(argv);

	print_copyright();

	vmon = calloc(1, sizeof(vmon_t));
	if (!vmon) {
		VWM_PERROR("unable to allocate vmon_t");
		goto _err;
	}

	vmon->width = WIDTH_DEFAULT;
	vmon->height = HEIGHT_DEFAULT;

	vmon->xserver = vwm_xserver_open();
	if (!vmon->xserver) {
		VWM_ERROR("unable to open xserver");
		goto _err_free;
	}

	vmon->charts = vwm_charts_create(vmon->xserver);
	if (!vmon->charts) {
		VWM_ERROR("unable to create charts instance");
		goto _err_xserver;
	}

	if (!vmon_handle_argv(vmon, argc, argv)) {
		VWM_ERROR("unable to handle arguments");
		goto _err_charts;
	}

	vmon->chart = vwm_chart_create(vmon->charts, vmon->pid, vmon->width, vmon->height);
	if (!vmon->chart) {
		VWM_ERROR("unable to create chart");
		goto _err_charts;
	}

	vmon->window = XCreateSimpleWindow(vmon->xserver->display, XSERVER_XROOT(vmon->xserver), 0, 0, vmon->width, vmon->height, 1, 0, 0);
	XGetWindowAttributes(vmon->xserver->display, vmon->window, &wattr);
	vmon->picture = XRenderCreatePicture(vmon->xserver->display, vmon->window, XRenderFindVisualFormat(vmon->xserver->display, wattr.visual), 0, &pattr);

	XMapWindow(vmon->xserver->display, vmon->window);

	XSelectInput(vmon->xserver->display, vmon->window, StructureNotifyMask|ExposureMask);

	return vmon;

_err_charts:
	vwm_charts_destroy(vmon->charts);
_err_xserver:
	vwm_xserver_close(vmon->xserver);
_err_free:
	free(vmon);
_err:
	return NULL;
}


/* tear everything down */
static void vmon_shutdown(vmon_t *vmon)
{
	assert(vmon);
	assert(vmon->xserver);

	XDestroyWindow(vmon->xserver->display, vmon->window);
	vwm_chart_destroy(vmon->charts, vmon->chart);
	vwm_charts_destroy(vmon->charts);
	vwm_xserver_close(vmon->xserver);
	free(vmon);
}


/* resize the chart (in response to configure events) */
static void vmon_resize(vmon_t *vmon, int width, int height)
{
	assert(vmon);

	vmon->width = width;
	vmon->height = height;
	vwm_chart_set_visible_size(vmon->charts, vmon->chart, width, height);
}


/* handle the next X event, may block */
static void vmon_process_event(vmon_t *vmon)
{
	XEvent	ev;

	assert(vmon);

	XNextEvent(vmon->xserver->display, &ev);

	switch (ev.type) {
		case ConfigureNotify:
			vmon_resize(vmon, ev.xconfigure.width, ev.xconfigure.height);
			vwm_chart_compose(vmon->charts, vmon->chart, NULL);
			vwm_chart_render(vmon->charts, vmon->chart, PictOpSrc, vmon->picture, 0, 0, vmon->width, vmon->height);
			break;
		case Expose:
			vwm_chart_render(vmon->charts, vmon->chart, PictOpSrc, vmon->picture, 0, 0, vmon->width, vmon->height);
			break;
		default:
			VWM_TRACE("unhandled event: %x\n", ev.type);
	}
}


int main(int argc, char * const argv[])
{
	vmon_t		*vmon;
	struct pollfd	pfd = { .events = POLLIN };
	int		ret = EXIT_SUCCESS;

	vmon = vmon_startup(argc, argv);
	if (!vmon) {
		VWM_ERROR("error starting vmon");
		return EXIT_FAILURE;
	}

	pfd.fd = ConnectionNumber(vmon->xserver->display);

	while (!vmon->done) {
		int	delay;

		if (vwm_charts_update(vmon->charts, &delay)) {
			vwm_chart_compose(vmon->charts, vmon->chart, NULL);
			vwm_chart_render(vmon->charts, vmon->chart, PictOpSrc, vmon->picture, 0, 0, vmon->width, vmon->height);
		}

		if (XPending(vmon->xserver->display) || poll(&pfd, 1, delay) > 0)
			vmon_process_event(vmon);

		if (got_sigchld) {
			int	status;

			got_sigchld = 0;
			wait(&status);

			if (WIFEXITED(status)) {
				ret = WEXITSTATUS(status);

				if (!vmon->linger)
					vmon->done = 1;
			}
		}
	}

	vmon_shutdown(vmon);

	return ret;
}
