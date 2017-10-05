#include "X11/Xlib.h"

typedef struct _vwm_t vwm_t;

void vwm_xevent_handle_key_press(vwm_t *vwm, XKeyPressedEvent *ev);
void vwm_xevent_handle_key_release(vwm_t *vwm, XKeyReleasedEvent *ev);
void vwm_xevent_handle_button_press(vwm_t *vwm, XButtonPressedEvent *ev);
void vwm_xevent_handle_motion_notify(vwm_t *vwm, XMotionEvent *ev);
void vwm_xevent_handle_button_release(vwm_t *vwm, XButtonReleasedEvent *ev);
void vwm_xevent_handle_create_notify(vwm_t *vwm, XCreateWindowEvent *ev);
void vwm_xevent_handle_destroy_notify(vwm_t *vwm, XDestroyWindowEvent *ev);
void vwm_xevent_handle_configure_request(vwm_t *vwm, XConfigureRequestEvent *ev);
void vwm_xevent_handle_configure_notify(vwm_t *vwm, XConfigureEvent *ev);
void vwm_xevent_handle_unmap_notify(vwm_t *vwm, XUnmapEvent *ev);
void vwm_xevent_handle_map_notify(vwm_t *vwm, XMapEvent *ev);
void vwm_xevent_handle_map_request(vwm_t *vwm, XMapRequestEvent *ev);
void vwm_xevent_handle_property_notify(vwm_t *vwm, XPropertyEvent *ev);
void vwm_xevent_handle_focusin(vwm_t *vwm, XFocusInEvent *ev);
void vwm_xevent_handle_mapping_notify(vwm_t *vwm, XMappingEvent *ev);
