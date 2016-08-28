#ifndef _CLICKETY_H
#define _CLICKETY_H

#include <X11/Xlib.h>

#include "vwm.h"

void vwm_clickety_motion(vwm_t *vwm, Window win, XMotionEvent *motion);
void vwm_clickety_released(vwm_t *vwm, Window win, XButtonPressedEvent *terminus);
int vwm_clickety_pressed(vwm_t *vwm, Window win, XButtonPressedEvent *impetus);

#endif
