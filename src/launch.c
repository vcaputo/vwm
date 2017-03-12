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
	/* launching of external processes / X clients */

#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "launch.h"
#include "vwm.h"

#define LAUNCHED_RELATIVE_PRIORITY	10	/* the wm priority plus this is used as the priority of launched processes */

/* launch a child command specified in argv, mode decides if we wait for the child to exit before returning. */
void vwm_launch(vwm_t *vwm, char **argv, vwm_launch_mode_t mode)
{
	/* XXX: in BG mode I double fork and let init inherit the orphan so I don't have to collect the return status */
	if (mode == VWM_LAUNCH_MODE_FG || !fork()) {
		if (!fork()) {
			/* child */
			setpriority(PRIO_PROCESS, getpid(), vwm->priority + LAUNCHED_RELATIVE_PRIORITY);
			execvp(argv[0], argv);
		}
		if (mode == VWM_LAUNCH_MODE_BG)
			exit(0);
	}
	wait(NULL); /* TODO: could wait for the specific pid, particularly in FG mode ... */
}
