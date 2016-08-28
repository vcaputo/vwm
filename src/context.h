#ifndef _CONTEXT_H
#define _CONTEXT_H

typedef struct _vwm_t vwm_t;

typedef enum _vwm_context_t {
	VWM_CONTEXT_DESKTOP = 0,	/* focus the desktop context */
	VWM_CONTEXT_SHELF,	/* focus the shelf context */
	VWM_CONTEXT_OTHER		/* focus the other context relative to the current one */
} vwm_context_t;

int vwm_context_focus(vwm_t *vwm, vwm_context_t desired_context);

#endif
