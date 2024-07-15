AC_INIT([vwm], [3.0], [vcaputo@pengaru.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_MACRO_DIRS([m4])
AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB
AM_SILENT_RULES([yes])
AX_APPEND_COMPILE_FLAGS([-Wall])

PKG_CHECK_MODULES(XWM_DEV, x11 xext xinerama xrandr xcomposite xfixes xdamage xrender,
	AM_CONDITIONAL(ENABLE_VWM, declare build_vwm=true)
	AC_DEFINE(HAVE_XWM_DEV, [1], [Define to 1 with X WM deps present]),
	AM_CONDITIONAL(ENABLE_VWM, false)
)

PKG_CHECK_MODULES(XCLIENT_DEV, x11 xext xfixes xrender,
	AM_CONDITIONAL(HAVE_XCLIENT_DEV, true)
	AC_DEFINE(HAVE_XCLIENT_DEV, [1], [Define to 1 with X Client deps present]),
	AM_CONDITIONAL(HAVE_XCLIENT_DEV, false)
)

PKG_CHECK_MODULES(PNG_DEV, libpng,
	AM_CONDITIONAL(HAVE_PNG_DEV, true)
	AC_DEFINE(HAVE_PNG_DEV, [1], [Define to 1 with libpng-dev present]),
	AM_CONDITIONAL(HAVE_PNG_DEV, false)
)

AM_CONDITIONAL(ENABLE_VWM, [test "x$build_vwm" = xtrue])

AC_CONFIG_FILES([
 Makefile
 src/Makefile
 src/libvmon/Makefile
])
AC_OUTPUT
