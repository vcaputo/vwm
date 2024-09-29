#ifndef _VCR_H
#define _VCR_H

#include <stdio.h> /* for FILE* */

#ifdef USE_XLIB
#include <X11/extensions/Xrender.h> /* for Picture */
#endif /* USE_XLIB */

#define VCR_DRAW_TEXT_N_STRS_MAX	512
#define VCR_ROW_HEIGHT			15	/* this should always be larger than the font height */

typedef enum vcr_backend_type_t {
#ifdef USE_XLIB
	VCR_BACKEND_TYPE_XLIB,
#endif /* USE_XLIB */
	VCR_BACKEND_TYPE_MEM,
} vcr_backend_type_t;

/* there are very minimal backend events plumbed out from the X events */
typedef enum vcr_backend_event_t {
	VCR_BACKEND_EVENT_NOOP,
	VCR_BACKEND_EVENT_RESIZE,
	VCR_BACKEND_EVENT_REDRAW,
	VCR_BACKEND_EVENT_QUIT,
} vcr_backend_event_t;

typedef enum vcr_present_op_t {
	VCR_PRESENT_OP_SRC,	/* equivalent to XRender PictOpSrc */
	VCR_PRESENT_OP_OVER,	/* equivaletn to XRender PictOpOver */
} vcr_present_op_t;

typedef enum vcr_layer_t {
	VCR_LAYER_TEXT,		/* the text layer for threadName/argv/wchan/state/pid etc. */
	VCR_LAYER_SHADOW,	/* the shadow layer below the text (XXX: this must be kept after text) */
	VCR_LAYER_GRAPHA,	/* the graph A layer below the shadow layer */
	VCR_LAYER_GRAPHB,	/* the graph B layer below the shadow layer */
	VCR_LAYER_CNT,
} vcr_layer_t;

typedef struct vcr_backend_t vcr_backend_t;
typedef struct vcr_dest_t vcr_dest_t;
typedef struct vcr_t vcr_t;

typedef struct vcr_str_t {
	const char	*str;
	size_t		len;
} vcr_str_t;

vcr_backend_t * vcr_backend_new(vcr_backend_type_t backend, ...);
int vcr_backend_get_dimensions(vcr_backend_t *vbe, int *res_width, int *res_height);
int vcr_backend_poll(vcr_backend_t *vbe, int timeout_us);
vcr_backend_event_t vcr_backend_next_event(vcr_backend_t *vbe, int *res_width, int *res_height);
vcr_backend_t * vcr_backend_free(vcr_backend_t *vbe);

#ifdef USE_XLIB
vcr_dest_t * vcr_dest_xwindow_new(vcr_backend_t *vbe, const char *name, unsigned width, unsigned height);
unsigned vcr_dest_xwindow_get_id(vcr_dest_t *dest);
vcr_dest_t * vcr_dest_xpicture_new(vcr_backend_t *vbe, Picture picture);
#endif /* USE_XLIB */
#ifdef USE_PNG
vcr_dest_t * vcr_dest_png_new(vcr_backend_t *vbe, FILE *output);
#endif /* USE_PNG */
vcr_dest_t * vcr_dest_free(vcr_dest_t *dest);

vcr_t * vcr_new(vcr_backend_t *vbe, int *hierarchy_end_ptr, int *snowflakes_cnt_ptr);
vcr_t * vcr_free(vcr_t *vcr);
int vcr_resize_visible(vcr_t *vcr, int width, int height);
void vcr_draw_text(vcr_t *vcr, vcr_layer_t layer, int x, int row, const vcr_str_t *strs, int n_strs, int *res_width);
void vcr_draw_ortho_line(vcr_t *vcr, vcr_layer_t layer, int x1, int y1, int x2, int y2);
void vcr_mark_finish_line(vcr_t *vcr, vcr_layer_t layer, int row);
void vcr_draw_bar(vcr_t *vcr, vcr_layer_t layer, int row, double t, int min_height);
void vcr_clear_row(vcr_t *vcr, vcr_layer_t layer, int row, int x, int width);
void vcr_shift_below_row_up_one(vcr_t *vcr, int row);
void vcr_shift_below_row_down_one(vcr_t *vcr, int row);
void vcr_shadow_row(vcr_t *vcr, vcr_layer_t layer, int row);
void vcr_stash_row(vcr_t *vcr, vcr_layer_t layer, int row);
void vcr_unstash_row(vcr_t *vcr, vcr_layer_t layer, int row);
void vcr_advance_phase(vcr_t *vcr, int delta);
int vcr_compose(vcr_t *vcr);
#ifdef USE_XLIB
int vcr_get_composed_xdamage(vcr_t *vcr, XserverRegion *res_damaged_region);
#endif /* USE_XLIB */
int vcr_present(vcr_t *vcr, vcr_present_op_t op, vcr_dest_t *dest, int x, int y, int width, int height);

#endif /* _VCR_H */
