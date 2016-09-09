#ifndef _KEY_H
#define _KEY_H

#include <X11/Xlib.h>

typedef struct _vwm_t vwm_t;

void vwm_key_released(vwm_t *vwm, Window win, XKeyReleasedEvent *keyrelease);
void vwm_key_pressed(vwm_t *vwm, Window win, XKeyPressedEvent *keypress);

#endif
