#ifndef _LAUNCH_H
#define _LAUNCH_H

typedef struct _vwm_t vwm_t;

typedef enum _vwm_launch_mode_t {
	VWM_LAUNCH_MODE_FG,
	VWM_LAUNCH_MODE_BG,
} vwm_launch_mode_t;

void vwm_launch(vwm_t *vwm, char **argv, vwm_launch_mode_t mode);

#endif
