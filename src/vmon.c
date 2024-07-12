/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2024  Vito Caputo - <vcaputo@pengaru.com>
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
#include <png.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
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
	Atom		wm_protocols_atom;
	Atom		wm_delete_atom;
	Window		window;
	int		width, height;
	Picture		picture;
	int		pid;
	int		done;
	int		linger;
	time_t		start_time;
	int		snapshots_interval;
	int		snapshot;
	char		*output_dir;
	char		*name;
	unsigned	n_snapshots;
	const char	* const *execv;
	unsigned	n_execv;
} vmon_t;


/* these are fairly arbitrarily chosen to be sane */
#define WIDTH_DEFAULT	800
#define HEIGHT_DEFAULT	600
#define WIDTH_MIN	200
#define HEIGHT_MIN	28

static volatile int got_sigchld, got_sigusr1, got_sigint, got_sigquit;

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
static int parse_flag_int(const char * const *flag, const char * const *end, const char * const *opt, int min, int max, int *res)
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


/* parse string out of opt, stores parsed opt in newly allocated *res on success */
static int parse_flag_str(const char * const *flag, const char * const *end, const char * const *opt, int min_len, char **res)
{
	char	*tmp;

	assert(flag);
	assert(end);
	assert(opt);
	assert(res);

	if (flag == end || (!min_len || **opt == '\0')) {
		VWM_ERROR("flag \"%s\" expects an argument", *flag);
		return 0;
	}

	if (strlen(*opt) < min_len) {
		VWM_ERROR("flag \"%s\" argument must be longer than %i, got \"%s\"", *flag, min_len, *opt);
		return 0;
	}

	tmp = strdup(*opt);
	if (!tmp) {
		VWM_ERROR("unable to duplicate argument \"%s\"", *opt);
		return 0;
	}

	free(*res);
	*res = tmp;

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


static void print_help(void)
{
	puts(
		"\n"
		"-------------------------------------------------------------------------------\n"
		" Flag              Description\n"
		"-------------------------------------------------------------------------------\n"
		" --                Sentinel, subsequent arguments form command to execute\n"
		" -f  --fullscreen  Fullscreen window\n"
		" -h  --help        Show this help\n"
		" -H  --height      Window height\n"
		" -l  --linger      Don't exit when top-level process exits\n"
		" -n  --name        Name of chart, shows in window title and output filenames\n"
		" -o  --output-dir  Directory to store saved output to (\".\" if unspecified)\n"
		" -p  --pid         PID of the top-level process to monitor (1 if unspecified)\n"
		" -i  --snapshots   Save a PNG snapshot every N seconds (SIGUSR1 also snapshots)\n"
		" -s  --snapshot    Save a PNG snapshot upon receiving SIGCHLD\n"
		" -v  --version     Print version\n"
		" -W  --width       Window width\n"
		" -z  --hertz       Sample rate in hertz\n"
		"-------------------------------------------------------------------------------"
	);
}


static void print_version(void)
{
	puts("vmon " VERSION);
}


static void print_copyright(void)
{
	puts(
		"\n"
		"Copyright (C) 2012-2024 Vito Caputo <vcaputo@pengaru.com>\n"
		"\n"
		"This program comes with ABSOLUTELY NO WARRANTY.  This is free software, and\n"
		"you are welcome to redistribute it under certain conditions.  For details\n"
		"please see the LICENSE file included with this program.\n"
	);
}


/* collect status of child */
static void handle_sigchld(int signum)
{
	got_sigchld = 1;
}


static void handle_sigusr1(int signum)
{
	got_sigusr1 = 1;
}


static void handle_sigint(int signum)
{
	got_sigint++;
}


static void handle_sigquit(int signum)
{
	got_sigquit++;
}


/* sanitize name so it's usable as a filename */
static char * filenamify(const char *name)
{
	char	*filename;

	assert(name);

	filename = calloc(strlen(name) + 1, sizeof(*name));
	if (!filename) {
		VWM_PERROR("unable to allocate filename for name \"%s\"", name);
		return NULL;
	}

	for (size_t i = 0; name[i]; i++) {
		char	c = name[i];

		switch (c) {
		/* replace characters relevant to path interpolation */
		case '/':
			c = '\\';
			break;

		case '.':
			/* no leading dot, and no ".." */
			if (i == 0 || (i == 1 && !name[2]))
				c = '_';
			break;
		}

		filename[i] = c;
	}

	return filename;
}


/* return an interpolated copy of arg */
static char * arg_interpolate(const vmon_t *vmon, const char *arg)
{
	FILE	*memfp;
	char	*xarg = NULL;
	size_t	xarglen;
	int	fmt = 0;

	assert(vmon);
	assert(arg);

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
		case 'W':	/* vmon's X window id in hex */
			fprintf(memfp, "%#x", (unsigned)vmon->window);
			break;

		case 'n':	/* --name verbatim */
			if (!vmon->name) {
				VWM_ERROR("%%n requires --name");
				goto _err;
			}

			fprintf(memfp, "%s", vmon->name);
			break;

		case 'N': {	/* --name sanitized for filename use */
			char	*filename;

			if (!vmon->name) {
				VWM_ERROR("%%N requires --name");
				goto _err;
			}

			filename = filenamify(vmon->name);
			if (!filename)
				goto _err;

			fprintf(memfp, "%s", filename);
			free(filename);
			break;
		}

		case 'O':	/* --output-dir */
			fprintf(memfp, "%s", vmon->output_dir);
			break;

		case 'P':	/* getpid() of vmon, convenient for triggering png snapshots on SIGUSR1 */
				/* XXX: note this assumes arg_interpolate() occurs pre-fork, true ATM  */
			fprintf(memfp, "%li", (long)getpid());
			break;

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


/* parse and apply argv, implementing an strace-like cli, mutates vmon. */
static int vmon_handle_argv(vmon_t *vmon, int argc, const char * const *argv)
{
	const char * const *end = &argv[argc - 1], * const *last = argv;

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
		} else if (is_flag(*argv, "-W", "--width")) {
			if (!parse_flag_int(argv, end, argv + 1, WIDTH_MIN, INT_MAX, &vmon->width))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-H", "--height")) {
			if (!parse_flag_int(argv, end, argv + 1, HEIGHT_MIN, INT_MAX, &vmon->height))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-o", "--output-dir")) {
			if (!parse_flag_str(argv, end, argv + 1, 1, &vmon->output_dir))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-n", "--name")) {
			if (!parse_flag_str(argv, end, argv + 1, 1, &vmon->name))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-i", "--snapshots")) {
			if (!parse_flag_int(argv, end, argv + 1, 1, INT_MAX, &vmon->snapshots_interval))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-s", "--snapshot")) {
			vmon->snapshot = 1;
			last = argv;
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
		} else if (is_flag(*argv, "-v", "--version")) {
			print_version();
			exit(EXIT_SUCCESS);
		} else if ((*argv)[0] == '-') {
			VWM_ERROR("Unrecognized argument: \"%s\", try --help\n", argv[0]);
			exit(EXIT_FAILURE);
		} else {
			/* stop looking for more flags on first unrecognized thing, assume we're in command territory now */
			break;
		}
	}

	/* if more argv remains, treat as a command to run */
	if (last != end) {
		last++;
		vmon->n_execv = end - last + 1;
		vmon->execv = last;
	}

	return 1;
}


/* turn vmon->execv into a new process and execute what's described there after interpolation. */
int vmon_execv(vmon_t *vmon)
{
	char	**xargv;
	pid_t	pid;

	assert(vmon);
	assert(vmon->execv);
	assert(vmon->n_execv);

	xargv = calloc(vmon->n_execv + 1, sizeof(*xargv));
	if (!xargv) {
		VWM_PERROR("unable to allocate interpolated argv");
		return 0;
	}

	/* TODO: clean up xargv on failures perhaps?  we just exit anyways */

	/* clone args into xargs, performing any interpolations while at it */
	for (unsigned i = 0; i < vmon->n_execv; i++) {
		xargv[i] = arg_interpolate(vmon, vmon->execv[i]);
		if (!xargv[i]) {
			VWM_ERROR("unable to allocate interpolated arg");
			return 0;
		}
	}

	if (signal(SIGCHLD, handle_sigchld) == SIG_ERR) {
		VWM_PERROR("unable to set SIGCHLD handler");
		return 0;
	}

	if (signal(SIGINT, handle_sigint) == SIG_ERR) {
		VWM_PERROR("unable to set SIGTERM handler");
		return 0;
	}

	if (signal(SIGQUIT, handle_sigquit) == SIG_ERR) {
		VWM_PERROR("unable to set SIGQUIT handler");
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
		if (execvp(*xargv, xargv) == -1) {
			VWM_PERROR("unable to exec \"%s\"", *xargv);
			exit(EXIT_FAILURE);
		}
	}

	vmon->pid = pid;

	return 1;
}


/* parse argv, connect to X, create window, attach libvmon to monitored process */
static vmon_t * vmon_startup(int argc, const char * const *argv)
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

	vmon->start_time = time(NULL);
	vmon->width = WIDTH_DEFAULT;
	vmon->height = HEIGHT_DEFAULT;
	vmon->output_dir = strdup(".");
	if (!vmon->output_dir) {
		VWM_ERROR("unable to alloc output dir");
		goto _err_free;
	}

	vmon->xserver = vwm_xserver_open();
	if (!vmon->xserver) {
		VWM_ERROR("unable to open xserver");
		goto _err_free;
	}

	vmon->wm_delete_atom = XInternAtom(vmon->xserver->display, "WM_DELETE_WINDOW", False);
	vmon->wm_protocols_atom = XInternAtom(vmon->xserver->display, "WM_PROTOCOLS", False);

	vmon->charts = vwm_charts_create(vmon->xserver);
	if (!vmon->charts) {
		VWM_ERROR("unable to create charts instance");
		goto _err_xserver;
	}

	if (!vmon_handle_argv(vmon, argc, argv)) {
		VWM_ERROR("unable to handle arguments");
		goto _err_xserver;
	}

	if (signal(SIGUSR1, handle_sigusr1) == SIG_ERR) {
		VWM_PERROR("unable to set SIGUSR1 handler");
		goto _err_xserver;
	}

	if (signal(SIGALRM, handle_sigusr1) == SIG_ERR) {
		VWM_PERROR("unable to set SIGALRM handler");
		goto _err_xserver;
	}

	if (vmon->snapshots_interval) {
		int	r;

		r = setitimer(ITIMER_REAL,
				&(struct itimerval){
					.it_interval.tv_sec = vmon->snapshots_interval,
					.it_value.tv_sec = vmon->snapshots_interval,
				}, NULL);
		if (r < 0) {
			VWM_PERROR("unable to set interval timer");
			goto _err_xserver;
		}
	}

	vmon->window = XCreateSimpleWindow(vmon->xserver->display, XSERVER_XROOT(vmon->xserver), 0, 0, vmon->width, vmon->height, 1, 0, 0);
	if (vmon->name)
		XStoreName(vmon->xserver->display, vmon->window, vmon->name);
	XGetWindowAttributes(vmon->xserver->display, vmon->window, &wattr);
	vmon->picture = XRenderCreatePicture(vmon->xserver->display, vmon->window, XRenderFindVisualFormat(vmon->xserver->display, wattr.visual), 0, &pattr);
	XMapWindow(vmon->xserver->display, vmon->window);
	XSelectInput(vmon->xserver->display, vmon->window, StructureNotifyMask|ExposureMask);
	XSync(vmon->xserver->display, False);

	if (vmon->execv) {
		if (vmon->pid) {
			VWM_ERROR("combining --pid with a command to run is not yet supported (TODO)\n");
			goto _err_win;
		}

		if (!vmon_execv(vmon))
			goto _err_win;
	}

	vmon->chart = vwm_chart_create(vmon->charts, vmon->pid ? : 1, vmon->width, vmon->height, vmon->name);
	if (!vmon->chart) {
		VWM_ERROR("unable to create chart");
		goto _err_win;
	}

	return vmon;

_err_win:
	XDestroyWindow(vmon->xserver->display, vmon->window);
	XRenderFreePicture(vmon->xserver->display, vmon->picture);
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


static int vmon_snapshot_as_png(vmon_t *vmon, FILE *output)
{
	XImage		*chart_as_ximage;
	png_bytepp	row_pointers;
	png_infop	info_ctx;
	png_structp	png_ctx;

	assert(vmon);
	assert(output);

	vwm_chart_render_as_ximage(vmon->charts, vmon->chart, NULL, &chart_as_ximage);

	row_pointers = malloc(sizeof(void *) * chart_as_ximage->height);
	if (!row_pointers) {
		XDestroyImage(chart_as_ximage);

		return -ENOMEM;
	}

	for (unsigned i = 0; i < chart_as_ximage->height; i++)
		row_pointers[i] = &((png_byte *)chart_as_ximage->data)[i * chart_as_ximage->bytes_per_line];

	png_ctx = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ctx) {
		free(row_pointers);
		XDestroyImage(chart_as_ximage);

		return -ENOMEM;
	}

	info_ctx = png_create_info_struct(png_ctx);
	if (!info_ctx) {
		png_destroy_write_struct(&png_ctx, NULL);
		XDestroyImage(chart_as_ximage);
		free(row_pointers);

		return -ENOMEM;
	}

	png_init_io(png_ctx, output);

	if (setjmp(png_jmpbuf(png_ctx)) != 0) {
		png_destroy_write_struct(&png_ctx, &info_ctx);
		XDestroyImage(chart_as_ximage);
		free(row_pointers);

		return -ENOMEM;
	}

	/* XXX: I'm sure this is making flawed assumptions about the color format
	 * and type etc, but this makes it work for me and that's Good Enough for now.
	 * One can easily turn runtime mapping of X color formats, endianness, and packing
	 * details to whatever a file format like PNG can express into a tar-filled rabbithole
	 * of fruitless wankery.
	 */
	png_set_bgr(png_ctx);
	png_set_IHDR(png_ctx, info_ctx,
		chart_as_ximage->width,
		chart_as_ximage->height,
		8,
		PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_BASE,
		PNG_FILTER_TYPE_BASE);

	png_write_info(png_ctx, info_ctx);
	png_write_image(png_ctx, row_pointers);
	png_write_end(png_ctx, NULL);

	XDestroyImage(chart_as_ximage);
	free(row_pointers);

	return 0;
}


static int vmon_snapshot(vmon_t *vmon)
{
	struct tm	*start_time;
	char		start_str[32];
	char		*name = NULL;
	char		path[4096];
	FILE		*output;
	int		r;

	assert(vmon);

	if (vmon->name) {
		name = filenamify(vmon->name);
		if (!name)
			return -errno;
	}

	if (mkdir(vmon->output_dir, 0755) == -1 && errno != EEXIST) {
		free(name);
		return -errno;
	}

	start_time = localtime(&vmon->start_time);
	strftime(start_str, sizeof(start_str), "%m.%d.%y-%T", start_time);
	snprintf(path, sizeof(path), "%s/%s%s%s-%u.png",
		vmon->output_dir,
		name ? name : "",
		name ? "-" : "",
		start_str,
		vmon->n_snapshots++);
	free(name);

	output = fopen(path, "w+");
	if (!output)
		return -errno;

	r = vmon_snapshot_as_png(vmon, output);
	if (r < 0) {
		fclose(output);
		return r;
	}

	fsync(fileno(output));
	fclose(output);

	return 0;
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

		case ClientMessage:
			if (ev.xclient.message_type != vmon->wm_protocols_atom)
				break;

			if (ev.xclient.data.l[0] != vmon->wm_delete_atom)
				break;

			vmon->done = 1;
			break;

		default:
			VWM_TRACE("unhandled event: %x\n", ev.type);
	}
}


int main(int argc, const char * const *argv)
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

		if (got_sigint > 2 || got_sigquit > 2) {
			vmon->done = 1;
		} else if (got_sigint == 1) {
			got_sigint++;

			kill(vmon->pid, SIGINT);
		} else if (got_sigquit == 1) {
			got_sigquit++;

			kill(vmon->pid, SIGQUIT);
		}

		if (got_sigchld) {
			int	status;

			if (vmon->snapshot && vmon_snapshot(vmon) < 0)
				VWM_ERROR("error saving snapshot");

			got_sigchld = 0;
			wait(&status);

			if (WIFEXITED(status)) {
				ret = WEXITSTATUS(status);

				if (!vmon->linger)
					vmon->done = 1;
			}
		}

		if (got_sigusr1) {
			if (vmon_snapshot(vmon) < 0)
				VWM_ERROR("error saving snapshot");

			got_sigusr1 = 0;
		}
	}

	vmon_shutdown(vmon);

	return ret;
}
