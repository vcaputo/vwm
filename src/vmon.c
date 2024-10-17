/*
 *                                  \/\/\
 *
 *  Copyright (C) 2012-2024  Vito Caputo - <vcaputo@pengaru.com>
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

#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "charts.h"
#include "vcr.h"
#include "util.h"

/* vmon exposes the monitoring charts to the shell in an strace-like cli */

typedef struct vmon_t {
	vcr_backend_t	*vcr_backend;
	vcr_dest_t	*vcr_dest;
	vwm_charts_t	*charts;
	vwm_chart_t	*chart;
	int		width, height;
	int		pid;
	int		done;
	int		linger;
	time_t		start_time;
	int		snapshots_interval;
	int		snapshot;
	int		now_names;
	int		headless;
	int		hertz;
	char		*output_dir;
	char		*name;
	char		*wip_name;
	unsigned	n_snapshots;
	const char	* const *execv;
	unsigned	n_execv;
} vmon_t;


/* these are fairly arbitrarily chosen to be sane */
#define WIDTH_DEFAULT	800
#define HEIGHT_DEFAULT	600
#define WIDTH_MIN	200
#define HEIGHT_MIN	28

static volatile int got_sigchld, got_sigusr1, got_sigint, got_sigquit, got_sigterm;

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
#ifdef USE_XLIB
	assert(vmon);

	if (vcr_backend_get_dimensions(vmon->vcr_backend, &vmon->width, &vmon->height) < 0) {
		VWM_ERROR("unable to get vcr_backend dimensions");
		return 0;
	}

	return 1;
#else /* USE_XLIB */
	return -ENOTSUP;
#endif
}


static void print_help(void)
{
	puts(
		"\n"
		"-------------------------------------------------------------------------------\n"
		" Flag              Description\n"
		"-------------------------------------------------------------------------------\n"
		" --                Sentinel, subsequent arguments form command to execute\n"
		" -f  --fullscreen  Fullscreen window (X only; no effect with --headless) \n"
		" -d  --headless    Headless mode; no X, only snapshots (default on no-X builds)\n"
		" -h  --help        Show this help\n"
		" -H  --height      Chart height\n"
		" -l  --linger      Don't exit when top-level process exits\n"
		" -n  --name        Name of chart, shows in window title and output filenames\n"
		" -N  --now-names   Use current time in filenames instead of start time\n"
		" -o  --output-dir  Directory to store saved output to (\".\" if unspecified)\n"
		" -p  --pid         PID of the top-level process to monitor (1 if unspecified)\n"
		" -i  --snapshots   Save a PNG snapshot every N seconds (SIG{TERM,USR1} also snapshots)\n"
		" -s  --snapshot    Save a PNG snapshot upon receiving SIG{CHLD,TERM,USR1}\n"
		" -w  --wip-name    Name to use for work-in-progress snapshot filename\n"
		" -v  --version     Print version\n"
		" -W  --width       Chart width\n"
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


/* collect status of child, triggering snapshot if snapshotting active */
static void handle_sigchld(int signum)
{
	got_sigchld = 1;
}


/* trigger a snapshot */
static void handle_sigusr1(int signum)
{
	got_sigusr1 = 1;
}


/* trigger a snapshot and exit immediately after it's been written */
static void handle_sigterm(int signum)
{
	got_sigterm = 1;
}


/* propagates to child first time, quits second time */
static void handle_sigint(int signum)
{
	got_sigint++;
}


/* propagates to child first time, quits second time */
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
#ifdef USE_XLIB
		case 'W':	/* vmon's X window id in hex */
			assert(!vmon->headless);
			fprintf(memfp, "%#x", vcr_dest_xwindow_get_id(vmon->vcr_dest));
			break;
#endif
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
		} else if (is_flag(*argv, "-N", "--now-names")) {
			vmon->now_names = 1;
			last = argv;
		} else if (is_flag(*argv, "-d", "--headless")) {
			vmon->headless = 1;
			last = argv;
		} else if (is_flag(*argv, "-i", "--snapshots")) {
			if (!parse_flag_int(argv, end, argv + 1, 1, INT_MAX, &vmon->snapshots_interval))
				return 0;

			last = ++argv;
		} else if (is_flag(*argv, "-s", "--snapshot")) {
			vmon->snapshot = 1;
			last = argv;
		} else if (is_flag(*argv, "-w", "--wip-name")) {
			if (!parse_flag_str(argv, end, argv + 1, 1, &vmon->wip_name))
				return 0;

			if (strchr(vmon->wip_name, '/')) {
				VWM_ERROR("invalid --wip-name: \"%s\"", vmon->wip_name);
				return 0;
			}

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
			if (!parse_flag_int(argv, end, argv + 1, 1, 1000, &vmon->hertz))
				return 0;

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


/* parse argv, init charts/vcr_backend/vcr_dest, attach libvmon to monitored process via vwm_chart_create() */
static vmon_t * vmon_startup(int argc, const char * const *argv)
{
	vcr_backend_type_t	backend_type;
	vmon_t			*vmon;

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

	if (!vmon_handle_argv(vmon, argc, argv)) {
		VWM_ERROR("unable to handle arguments");
		goto _err_vcr;
	}

#ifdef USE_XLIB
	if (!vmon->headless)
		backend_type = VCR_BACKEND_TYPE_XLIB;
#else
	/* force headless without X support */
	vmon->headless = 1;
#endif
	if (vmon->headless)
		backend_type = VCR_BACKEND_TYPE_MEM;

	vmon->vcr_backend = vcr_backend_new(backend_type, NULL);
	if (!vmon->vcr_backend) {
		VWM_ERROR("unable to create vcr backend");
		goto _err_free;
	}

	vmon->charts = vwm_charts_create(vmon->vcr_backend, VWM_CHARTS_FLAG_DEFER_MAINTENANCE);
	if (!vmon->charts) {
		VWM_ERROR("unable to create charts instance");
		goto _err_vcr;
	}

	if (vmon->hertz)
		vwm_charts_rate_set(vmon->charts, vmon->hertz);

	if (signal(SIGUSR1, handle_sigusr1) == SIG_ERR) {
		VWM_PERROR("unable to set SIGUSR1 handler");
		goto _err_vcr;
	}

	if (signal(SIGALRM, handle_sigusr1) == SIG_ERR) {
		VWM_PERROR("unable to set SIGALRM handler");
		goto _err_vcr;
	}

	if (signal(SIGTERM, handle_sigterm) == SIG_ERR) {
		VWM_PERROR("unable to set SIGTERM handler");
		goto _err_vcr;
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
			goto _err_vcr;
		}
	}

#ifdef USE_XLIB
	if (!vmon->headless) {
		vmon->vcr_dest = vcr_dest_xwindow_new(vmon->vcr_backend, vmon->name, vmon->width, vmon->height);
		if (!vmon->vcr_dest) {
			VWM_ERROR("unable to create destination XWindow");
			goto _err_vcr;
		}
	}
#endif

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
	(void) vcr_dest_free(vmon->vcr_dest);
_err_vcr:
	(void) vcr_backend_free(vmon->vcr_backend);
_err_free:
	free(vmon);
_err:
	return NULL;
}


/* tear everything down */
static void vmon_shutdown(vmon_t *vmon)
{
	assert(vmon);

	vwm_chart_destroy(vmon->charts, vmon->chart);
	vwm_charts_destroy(vmon->charts);
	(void) vcr_dest_free(vmon->vcr_dest);
	(void) vcr_backend_free(vmon->vcr_backend);
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


static int vmon_snapshot(vmon_t *vmon)
{
#ifdef USE_PNG
	time_t		now, *t_ptr = &now;
	struct tm	*t;
	char		t_str[32];
	char		*name = NULL;
	char		path[4096], tmp_path[4096];
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

	if (vmon->now_names)
		now = time(NULL);
	else
		t_ptr = &vmon->start_time;

	t = localtime(t_ptr);
	strftime(t_str, sizeof(t_str), "%m.%d.%y-%T", t);
	snprintf(path, sizeof(path), "%s/%s%s%s-%u.png",
		vmon->output_dir,
		name ? name : "",
		name ? "-" : "",
		t_str,
		vmon->n_snapshots++);
	if (vmon->wip_name) {
		snprintf(tmp_path, sizeof(tmp_path), "%s/%s", vmon->output_dir, vmon->wip_name);
	} else {
		snprintf(tmp_path, sizeof(tmp_path), "%s/.%s%s%s-%u.png-WIP",
			vmon->output_dir,
			name ? name : "",
			name ? "-" : "",
			t_str,
			vmon->n_snapshots);
	}
	free(name);

	output = fopen(tmp_path, "w+");
	if (!output)
		return -errno;

	{
		vcr_dest_t	*png_dest;

		png_dest = vcr_dest_png_new(vmon->vcr_backend, output);
		if (!png_dest) {
			(void) unlink(tmp_path);
			(void) fclose(output);
			return -ENOMEM;
		}

		if (vmon->headless)
			vwm_chart_compose(vmon->charts, vmon->chart);

		/* FIXME: render/libpng errors need to propagate and be handled */
		vwm_chart_render(vmon->charts, vmon->chart, VCR_PRESENT_OP_SRC, png_dest, -1, -1, -1, -1);
		png_dest = vcr_dest_free(png_dest);
	}

	fflush(output);
	fsync(fileno(output));
	fclose(output);
	r = rename(tmp_path, path);
	if (r < 0)
		return -errno;

	return 0;
#else
	return -ENOTSUP;
#endif
}


/* handle the next backend event, may block */
static void vmon_process_event(vmon_t *vmon)
{
	int			width, height;
	vcr_backend_event_t	ev;

	assert(vmon);

	ev = vcr_backend_next_event(vmon->vcr_backend, &width, &height);
	switch (ev) {
		case VCR_BACKEND_EVENT_RESIZE:
			vmon_resize(vmon, width, height);
			vwm_chart_compose(vmon->charts, vmon->chart);
			vwm_chart_render(vmon->charts, vmon->chart, VCR_PRESENT_OP_SRC, vmon->vcr_dest, -1, -1, -1, -1);
			break;

		case VCR_BACKEND_EVENT_REDRAW:
			vwm_chart_render(vmon->charts, vmon->chart, VCR_PRESENT_OP_SRC, vmon->vcr_dest, -1, -1, -1, -1);
			break;

		case VCR_BACKEND_EVENT_QUIT:
			vmon->done = 1;
			break;

		case VCR_BACKEND_EVENT_NOOP:
			break;

		default:
			VWM_TRACE("unhandled event: %x\n", ev);
	}
}


int main(int argc, const char * const *argv)
{
	vmon_t	*vmon;
	int	ret = EXIT_SUCCESS;

	vmon = vmon_startup(argc, argv);
	if (!vmon) {
		VWM_ERROR("error starting vmon");
		return EXIT_FAILURE;
	}

	while (!vmon->done) {
		int	delay_us;

		/* update only actually updates when enough time has passed, and always returns how much time
		 * to sleep before calling update again (-1 for infinity (paused)).
		 *
		 * if 0 is returned, no update was performed/no changes occured.
		 */
		if (vwm_charts_update(vmon->charts, &delay_us)) {
			if (!vmon->headless) {
				vwm_chart_compose(vmon->charts, vmon->chart);
				vwm_chart_render(vmon->charts, vmon->chart, VCR_PRESENT_OP_SRC, vmon->vcr_dest, -1, -1, -1, -1);
			}
		}

		if (vcr_backend_poll(vmon->vcr_backend, delay_us) > 0)
			vmon_process_event(vmon);

		if (got_sigint > 2 || got_sigquit > 2) {
			vmon->done = 1;
		} else if (got_sigint == 1) {
			got_sigint++;

			kill(vmon->pid, SIGINT);
		} else if (got_sigquit == 1) {
			got_sigquit++;

			kill(vmon->pid, SIGQUIT);
		} else if (got_sigterm) {
			if (vmon->snapshot || vmon->snapshots_interval) {
				/* simulate sigusr1 to trigger a final snapshot, */
				got_sigusr1 = 1;
			}

			vmon->done = 1;
		}

		if (got_sigchld) {
			int	status, r;

			if (vmon->snapshot && (r = vmon_snapshot(vmon)) < 0)
				VWM_ERROR("error saving snapshot: %s", strerror(-r));

			got_sigchld = 0;
			wait(&status);

			if (WIFEXITED(status)) {
				ret = WEXITSTATUS(status);

				if (!vmon->linger)
					vmon->done = 1;
			}
		}

		if (got_sigusr1 || (vmon->snapshots_interval && !vmon->n_snapshots)) {
			int	r;

			if ((r = vmon_snapshot(vmon)) < 0)
				VWM_ERROR("error saving snapshot: %s", strerror(-r));

			got_sigusr1 = 0;
		}
	}

	vmon_shutdown(vmon);

	return ret;
}
