AC_INIT([vwm], [3.0], [vcaputo@pengaru.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_MACRO_DIRS([m4])
AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB
AM_SILENT_RULES([yes])
AX_APPEND_COMPILE_FLAGS([-Wall])

PKG_CHECK_MODULES(VWM, x11 xext xinerama xrandr xcomposite xfixes xdamage xrender)
PKG_CHECK_MODULES(VMON, x11 xext xfixes xrender)

AC_CONFIG_FILES([
 Makefile
 src/Makefile
 src/libvmon/Makefile
])
AC_OUTPUT
