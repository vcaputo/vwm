#ifndef _CHARTS_H
#define _CHARTS_H

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include "xserver.h"

typedef struct _vwm_charts_t vwm_charts_t;
typedef struct _vwm_chart_t vwm_chart_t;

vwm_charts_t * vwm_charts_create(vwm_xserver_t *xserver);
void vwm_charts_destroy(vwm_charts_t *charts);
void vwm_charts_rate_increase(vwm_charts_t *charts);
void vwm_charts_rate_decrease(vwm_charts_t *charts);
void vwm_charts_rate_set(vwm_charts_t *charts, unsigned hertz);
int vwm_charts_update(vwm_charts_t *charts, int *desired_delay);

vwm_chart_t * vwm_chart_create(vwm_charts_t *charts, int pid, int width, int height);
void vwm_chart_destroy(vwm_charts_t *charts, vwm_chart_t *chart);
void vwm_chart_reset_snowflakes(vwm_charts_t *charts, vwm_chart_t *chart);
int vwm_chart_set_visible_size(vwm_charts_t *charts, vwm_chart_t *chart, int width, int height);
void vwm_chart_compose(vwm_charts_t *charts, vwm_chart_t *chart, XserverRegion *res_damaged_region);
void vwm_chart_render(vwm_charts_t *charts, vwm_chart_t *chart, int op, Picture dest, int x, int y, int width, int height);

#endif
