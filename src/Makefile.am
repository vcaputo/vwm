SUBDIRS = libvmon
bin_PROGRAMS = vwm vmon

vwm_SOURCES = clickety.c composite.c context.c desktop.c key.c launch.c logo.c charts.c screen.c vwm.c window.c xevent.c xserver.c xwindow.c clickety.h composite.h context.h desktop.h key.h launch.h list.h logo.h charts.h screen.h util.h vwm.h window.h xevent.h xserver.h xwindow.h colors.def launchers.def
vwm_LDADD = @VWM_LIBS@ libvmon/libvmon.a
vwm_CPPFLAGS = @VWM_CFLAGS@

vmon_SOURCES = vmon.c charts.c xserver.c charts.h xserver.h
vmon_LDADD= @VMON_LIBS@ libvmon/libvmon.a
vmon_CPPFLAGS = @VMON_CFLAGS@
