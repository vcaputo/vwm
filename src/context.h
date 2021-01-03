#ifndef _CONTEXT_H
#define _CONTEXT_H

#include "direction.h"
#include "list.h"

typedef struct _vwm_t vwm_t;
typedef struct _vwm_desktop_t vwm_desktop_t;

/* contexts and desktops are *very* similar, they should likely share code,
 * simply duplicating for now.
 */
typedef struct _vwm_context_t {
	list_head_t	contexts;		/* global list of contexts in spatial created-in order */
	list_head_t	contexts_mru;		/* global list of contexts in MRU order */
	vwm_desktop_t	*focused_desktop;	/* the focused desktop on this context */
	unsigned	color;			/* color used for focused border on this context */
} vwm_context_t;

vwm_context_t * vwm_context_mru(vwm_t *vwm, vwm_context_t *context);
vwm_context_t * vwm_context_create(vwm_t *vwm, int color, vwm_desktop_t *desktop);
void vwm_context_destroy(vwm_t *vwm, vwm_context_t *context);
vwm_context_t * vwm_context_next_mru(vwm_t *vwm, vwm_context_t *context, vwm_direction_t direction);
vwm_context_t * vwm_context_next(vwm_t *vwm, vwm_context_t *context, vwm_direction_t direction);
vwm_context_t * vwm_context_by_color(vwm_t *vwm, unsigned color);

#endif
