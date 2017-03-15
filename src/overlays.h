#ifndef _OVERLAYS_H
#define _OVERLAYS_H

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include "xserver.h"

typedef struct _vwm_overlays_t vwm_overlays_t;
typedef struct _vwm_overlay_t vwm_overlay_t;

vwm_overlays_t * vwm_overlays_create(vwm_xserver_t *xserver);
void vwm_overlays_destroy(vwm_overlays_t *overlays);
void vwm_overlays_rate_increase(vwm_overlays_t *overlays);
void vwm_overlays_rate_decrease(vwm_overlays_t *overlays);
int vwm_overlays_update(vwm_overlays_t *overlays, int *desired_delay);

vwm_overlay_t * vwm_overlay_create(vwm_overlays_t *overlays, int pid, int width, int height);
void vwm_overlay_destroy(vwm_overlays_t *overlays, vwm_overlay_t *overlay);
void vwm_overlay_reset_snowflakes(vwm_overlays_t *overlays, vwm_overlay_t *overlay);
int vwm_overlay_set_visible_size(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int width, int height);
void vwm_overlay_compose(vwm_overlays_t *overlays, vwm_overlay_t *overlay, XserverRegion *res_damaged_region);
void vwm_overlay_render(vwm_overlays_t *overlays, vwm_overlay_t *overlay, int op, Picture dest, int x, int y, int width, int height);

#endif
