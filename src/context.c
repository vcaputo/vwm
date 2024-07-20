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
	/*  contexts (this is derived from desktops) */

#include <assert.h>
#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "context.h"
#include "desktop.h"
#include "vwm.h"
#include "xwindow.h"

/* make the specified context the most recently used one */
vwm_context_t * vwm_context_mru(vwm_t *vwm, vwm_context_t *context)
{
	VWM_TRACE("MRU context: %p", context);
	list_move(&context->contexts_mru, &vwm->contexts_mru);

	return  context;
}


/* return next MRU context relative to the supplied context */
vwm_context_t * vwm_context_next_mru(vwm_t *vwm, vwm_context_t *context, vwm_direction_t direction)
{
	list_head_t	*next;

	switch (direction) {
	case VWM_DIRECTION_FORWARD:
		if (context->contexts_mru.next == &vwm->contexts_mru) {
			next = context->contexts_mru.next->next;
		} else {
			next = context->contexts_mru.next;
		}
		break;

	case VWM_DIRECTION_REVERSE:
		if (context->contexts_mru.prev == &vwm->contexts_mru) {
			next = context->contexts_mru.prev->prev;
		} else {
			next = context->contexts_mru.prev;
		}
		break;

	default:
		assert(0);
	}

	return list_entry(next, vwm_context_t, contexts_mru);
}


/* return next context spatially relative to the supplied context, no wrap-around */
vwm_context_t * vwm_context_next(vwm_t *vwm, vwm_context_t *context, vwm_direction_t direction)
{
	switch (direction) {
	case VWM_DIRECTION_FORWARD:
		if (context->contexts.next != &vwm->contexts)
			context = list_entry(context->contexts.next, vwm_context_t, contexts);
		break;

	case VWM_DIRECTION_REVERSE:
		if (context->contexts.prev != &vwm->contexts)
			context = list_entry(context->contexts.prev, vwm_context_t, contexts);
		break;
	}

	return context;
}


/* helper for automatically assigning context colors */
static int next_context_color_idx(vwm_t *vwm)
{
	int		counts[VWM_CONTEXT_COLOR_MAX] = {};
	vwm_context_t	*context;
	int		color = 0;

	/* TODO: contexts should probably keep a window count,
	 * so this could skip empty contexts, then those
	 * would be automatically recycled.
	 */
	list_for_each_entry(context, &vwm->contexts, contexts)
		counts[context->color]++;

	for (int i = 0; i < NELEMS(counts); i++) {
		if (counts[i] < counts[color])
			color = i;
	}

	return color;
}


/* create a context */
/* if color = -1 one is automatically assigned,
 * otherwise the supplied color is used.
 */
vwm_context_t * vwm_context_create(vwm_t *vwm, int color, vwm_desktop_t *desktop)
{
	vwm_context_t	*context;

	context = calloc(1, sizeof(vwm_context_t));
	if (context == NULL) {
		VWM_PERROR("Failed to allocate context");
		goto _fail;
	}

	if (color < 0)
		color = next_context_color_idx(vwm);

	assert(color < NELEMS(vwm->context_colors));

	context->color = color;

	list_add_tail(&context->contexts, &vwm->contexts);
	list_add_tail(&context->contexts_mru, &vwm->contexts_mru);

	if (!desktop)
		 desktop = vwm_desktop_create(vwm, context);

	context->focused_desktop = desktop;

	return context;

_fail:
	return NULL;
}


/* destroy a context */
void vwm_context_destroy(vwm_t *vwm, vwm_context_t *context)
{
	/* silently refuse to destroy a context having windows (for now) */
	/* there's _always_ a focused window on a context having mapped windows */
	if (context->focused_desktop && context->focused_desktop->focused_window)
		return;

	/* also silently refuse to destroy the last context (for now) */
	if (context->contexts.next == context->contexts.prev)
		return;

	list_del(&context->contexts);
	list_del(&context->contexts_mru);
}


/* find a context by color, creating one if needed */
vwm_context_t * vwm_context_by_color(vwm_t *vwm, unsigned color)
{
	vwm_context_t	*context;

	list_for_each_entry(context, &vwm->contexts, contexts) {
		if (context->color == color)
			return context;
	}

	return vwm_context_create(vwm, color, NULL);
}
