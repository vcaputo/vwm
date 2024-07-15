SUBDIRS = libvmon
bin_PROGRAMS = vmon

vmon_SOURCES = ascii.c charts.c vcr.c vmon.c ascii.h charts.h vcr.h
vmon_LDADD = libvmon/libvmon.a
vmon_CPPFLAGS = -O2

if HAVE_XCLIENT_DEV
vmon_SOURCES += xserver.c xserver.h
vmon_LDADD += @XCLIENT_DEV_LIBS@
vmon_CPPFLAGS += @XCLIENT_DEV_CFLAGS@ -DUSE_XLIB
endif

if HAVE_PNG_DEV
vmon_CPPFLAGS += @PNG_DEV_CFLAGS@ -DUSE_PNG
vmon_LDADD += @PNG_DEV_LIBS@
endif


if ENABLE_VWM
bin_PROGRAMS += vwm

vwm_SOURCES = ascii.c clickety.c composite.c context.c desktop.c key.c launch.c logo.c charts.c screen.c vcr.c vwm.c window.c xevent.c xserver.c xwindow.c ascii.h clickety.h composite.h context.h desktop.h direction.h key.h launch.h list.h logo.h charts.h screen.h util.h vcr.h vwm.h window.h xevent.h xserver.h xwindow.h colors.def context_colors.def launchers.def 
vwm_LDADD = @XWM_DEV_LIBS@ libvmon/libvmon.a
vwm_CPPFLAGS = @XWM_DEV_CFLAGS@ -DUSE_XLIB
endif
