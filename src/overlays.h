#ifndef _OVERLAYS_H
#define _OVERLAYS_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>

typedef struct _vwm_t vwm_t;
typedef struct _vwm_xwindow_t vwm_xwindow_t;

/* everything needed by the per-window overlay's context */
typedef struct _vwm_overlay_t {
	Pixmap	text_pixmap;		/* pixmap for overlayed text (kept around for XDrawText usage) */
	Picture	text_picture;		/* picture representation of text_pixmap */
	Picture	shadow_picture;		/* text shadow layer */
	Picture	grapha_picture;		/* graph A layer */
	Picture	graphb_picture;		/* graph B layer */
	Picture	tmp_picture;		/* 1 row worth of temporary picture space */
	Picture	picture;		/* overlay picture derived from the pixmap, for render compositing */
	int	width;			/* current width of the overlay */
	int	height;			/* current height of the overlay */
	int	phase;			/* current position within the (horizontally scrolling) graphs */
	int	heirarchy_end;		/* row where the process heirarchy currently ends */
	int	snowflakes_cnt;		/* count of snowflaked rows (reset to zero to truncate snowflakes display) */
	int	gen_last_composed;	/* the last composed vmon generation */
	int	redraw_needed;		/* if a redraw is required (like when the window is resized...) */
} vwm_overlay_t;

int vwm_overlay_xwin_composed_height(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_overlay_xwin_reset_snowflakes(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_overlay_xwin_create(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_overlay_xwin_destroy(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_overlay_xwin_compose(vwm_t *vwm, vwm_xwindow_t *xwin);
void vwm_overlay_rate_increase(vwm_t *vwm);
void vwm_overlay_rate_decrease(vwm_t *vwm);
void vwm_overlay_update(vwm_t *vwm, int *desired_delay);

#endif
