# Must init the autoconf setup
# The first parameter is project name
# second is version number
# third is bug report address
AC_INIT([v4l2 video decode application], [1.0])

# Safety checks in case user overwritten --srcdir
AC_CONFIG_SRCDIR([src/main.c])

# Store the auxiliary build tools (e.g., install-sh, config.sub, config.guess)
# in this dir (build-aux)
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

# Init automake, and specify this program use relaxed structures.
# i.e. this program doesn't follow the gnu coding standards, and doesn't have
# ChangeLog, COPYING, AUTHORS, INSTALL, README etc. files.
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

# Check for C compiler
AC_PROG_CC

# Enable quiet compiles on automake 1.11.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# Initialize libtool
AC_PROG_LIBTOOL

AC_CHECK_PROG(PKGCONFIG_CHECK,pkg-config,yes)
if test x"$PKGCONFIG_CHECK" != x"yes" ; then
    AC_MSG_ERROR([Please install pkg-config before continuing])
fi

# We can add more checks in this section
PKG_CHECK_MODULES([AVCODEC], [libavcodec >= 3.0.7])
PKG_CHECK_MODULES([AVFORMAT], [libavformat >= 3.0.7])
PKG_CHECK_MODULES([AVUTIL], [libavutil >= 3.0.7])

PKG_CHECK_MODULES([LIBDRM], [libdrm >= 2.4.67])

# Wayland support is optional

AC_ARG_WITH([wayland], AS_HELP_STRING([--with-wayland], [Build with Wayland support]))

AS_IF([test "x$with_wayland" = "xyes"], [
	PKG_CHECK_MODULES([WAYLAND], [wayland-client], [HAVE_WAYLAND=yes], [HAVE_WAYLAND=no])
	PKG_CHECK_MODULES([WAYLAND_PROTOS], [wayland-protocols], [HAVE_WAYLAND=yes], [HAVE_WAYLAND=no])
])

if test "x$HAVE_WAYLAND" = "xyes"; then
        AC_MSG_NOTICE([Building Wayland display support])
fi

AM_CONDITIONAL(ENABLE_WAYLAND, [test "x$HAVE_WAYLAND" = "xyes"])

AC_DEFUN([PROTOS_DIR], [`pkg-config --variable=pkgdatadir wayland-protocols`])
AC_DEFUN([WAYLAND_GEN_PROTOS],
[ wayland-scanner client-header PROTOS_DIR/stable/viewporter/viewporter.xml src/viewporter-client.h
  wayland-scanner code          PROTOS_DIR/stable/viewporter/viewporter.xml src/viewporter-client.c

  wayland-scanner client-header PROTOS_DIR/stable/presentation-time/presentation-time.xml src/presentation-time-client.h
  wayland-scanner code          PROTOS_DIR/stable/presentation-time/presentation-time.xml src/presentation-time-client.c

  wayland-scanner client-header PROTOS_DIR/unstable/xdg-shell/xdg-shell-unstable-v6.xml src/xdg-shell-unstable-v6-client.h
  wayland-scanner code          PROTOS_DIR/unstable/xdg-shell/xdg-shell-unstable-v6.xml src/xdg-shell-unstable-v6-client.c

  wayland-scanner client-header PROTOS_DIR/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml src/linux-dmabuf-unstable-v1-client.h
  wayland-scanner code          PROTOS_DIR/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml src/linux-dmabuf-unstable-v1-client.c
])

if test "x$HAVE_WAYLAND" = "xyes"; then
	AC_DEFINE([WAYLAND_SUPPORT], [], [Wayland support])

	#generate wayland headers
	AC_CHECK_PROG(SCANNER, [wayland-scanner], [yes], [no])
	if test "$SCANNER" = "no"; then
		AC_MSG_ERROR([Required program 'wayland-scanner' not found.])
	fi

	AC_MSG_NOTICE([Found wayland-scanner.])
	WAYLAND_GEN_PROTOS
fi

AC_CONFIG_HEADERS([config.h])

# Tells automake to create a Makefile
# See https://www.gnu.org/software/automake/manual/html_node/Requirements.html
AC_CONFIG_FILES([Makefile src/Makefile])

# Generate the output
AC_OUTPUT
