#ifndef _CHARTS_H
#define _CHARTS_H

#ifdef USE_XLIB
#include <X11/extensions/Xfixes.h> /* this is just for XserverRegion/vwm_chart_compose_xdamage() */
#include "xserver.h"
#endif

#include "vcr.h"

typedef struct _vwm_charts_t vwm_charts_t;
typedef struct _vwm_chart_t vwm_chart_t;

vwm_charts_t * vwm_charts_create(vcr_backend_t *vbe);
void vwm_charts_destroy(vwm_charts_t *charts);
void vwm_charts_rate_increase(vwm_charts_t *charts);
void vwm_charts_rate_decrease(vwm_charts_t *charts);
void vwm_charts_rate_set(vwm_charts_t *charts, unsigned hertz);
int vwm_charts_update(vwm_charts_t *charts, int *desired_delay);

vwm_chart_t * vwm_chart_create(vwm_charts_t *charts, int pid, int width, int height, const char *name);
void vwm_chart_destroy(vwm_charts_t *charts, vwm_chart_t *chart);
void vwm_chart_reset_snowflakes(vwm_charts_t *charts, vwm_chart_t *chart);
int vwm_chart_set_visible_size(vwm_charts_t *charts, vwm_chart_t *chart, int width, int height);
void vwm_chart_compose(vwm_charts_t *charts, vwm_chart_t *chart);
#ifdef USE_XLIB
/* XXX: this is annoying, and frankly could probably go away if I don't ever actually bother with producing
 * an accurate damaged region.  Right now it's just a visible area of the composed charts rectangle,
 * which could just as well be served by a simple x,y,w,h visible area description the caller could then
 * turn into an XserverRegion is desired.
 */
void vwm_chart_compose_xdamage(vwm_charts_t *charts, vwm_chart_t *chart, XserverRegion *res_damaged_region);
#endif
void vwm_chart_render(vwm_charts_t *charts, vwm_chart_t *chart, vcr_present_op_t op, vcr_dest_t *dest, int x, int y, int width, int height);

#endif
