#ifndef _COMPOSITE_H
#define _COMPOSITE_H

#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

typedef struct _vwm_t vwm_t;
typedef struct _vwm_xwindow_t vwm_xwindow_t;

void vwm_composite_xwin_create(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_composite_xwin_destroy(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_composite_damage_add(vwm_t *vwm, XserverRegion damage);
void vwm_composite_damage_win(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_composite_handle_configure(vwm_t *vwm, vwm_xwindow_t *xwin, XWindowAttributes *new_attrs);
void vwm_composite_handle_map(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_composite_damage_event(vwm_t *vwm, XDamageNotifyEvent *ev);
void vwm_composite_damage_win(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_composite_paint_all(vwm_t *vwm);
void vwm_composite_invalidate_root(vwm_t *vwm);
void vwm_composite_repaint_needed(vwm_t *vwm);
void vwm_composite_toggle(vwm_t *vwm);

#endif
