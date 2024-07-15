SUBDIRS = libvmon
bin_PROGRAMS = vwm vmon

vwm_SOURCES = clickety.c composite.c context.c desktop.c key.c launch.c logo.c charts.c screen.c vcr.c vwm.c window.c xevent.c xserver.c xwindow.c clickety.h composite.h context.h desktop.h direction.h key.h launch.h list.h logo.h charts.h screen.h util.h vcr.h vwm.h window.h xevent.h xserver.h xwindow.h colors.def context_colors.def launchers.def
vwm_LDADD = @VWM_LIBS@ libvmon/libvmon.a
vwm_CPPFLAGS = @VWM_CFLAGS@ -DUSE_XLIB

vmon_SOURCES = charts.c vcr.c vmon.c xserver.c charts.h vcr.h xserver.h
vmon_LDADD= @VMON_LIBS@ libvmon/libvmon.a
vmon_CPPFLAGS = @VMON_CFLAGS@ -DUSE_PNG -DUSE_XLIB
