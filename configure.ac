dnl If not 1, append datestamp to the version number
m4_define(empathy_released, 1)

m4_define([empathy_major_version], [3])
m4_define([empathy_minor_version], [12])
m4_define([empathy_micro_version], [10])
m4_define([empathy_nano_version], [0])

dnl Display the nano_version only if it's not '0'
m4_define([empathy_base_version],
          [empathy_major_version.empathy_minor_version.empathy_micro_version])
m4_define([empathy_full_version],
          [m4_if(empathy_nano_version, 0, [empathy_base_version],
              [empathy_base_version].[empathy_nano_version])])

m4_define(empathy_maybe_datestamp,
          m4_esyscmd([if test x]empathy_released[ != x1; then date +.%Y%m%d | tr -d '\n\r'; fi]))

m4_define(empathy_version, empathy_full_version[]empathy_maybe_datestamp)


AC_INIT([Empathy],
        [empathy_version],
        [http://bugzilla.gnome.org/browse.cgi?product=empathy],
        [empathy],
        [https://live.gnome.org/Empathy])

AC_PREREQ([2.64])
AC_COPYRIGHT([
  Copyright (C) 2003-2007 Imendio AB
  Copyright (C) 2007-2010 Collabora Ltd.
])

# Minimal version required

# Hardp deps
FOLKS_REQUIRED=0.9.5
GNUTLS_REQUIRED=2.8.5

GLIB_REQUIRED=2.37.6
AC_DEFINE(GLIB_VERSION_MIN_REQUIRED, GLIB_VERSION_2_30, [Ignore post 2.30 deprecations])
AC_DEFINE(GLIB_VERSION_MAX_ALLOWED, GLIB_VERSION_2_38, [Prevent post 2.38 APIs])

GTK_REQUIRED=3.9.4
AC_DEFINE(GDK_VERSION_MIN_REQUIRED, GDK_VERSION_3_8, [Ignore post 3.8 deprecations])
AC_DEFINE(GDK_VERSION_MAX_ALLOWED, GDK_VERSION_3_10, [Prevent post 3.10 APIs])

CLUTTER_REQUIRED=1.10.0
AC_DEFINE(CLUTTER_VERSION_MIN_REQUIRED, CLUTTER_VERSION_1_8, [Ignore post 1.8 deprecations])
AC_DEFINE(CLUTTER_VERSION_MAX_ALLOWED, CLUTTER_VERSION_1_10, [Prevent post 1.10 APIs])

CLUTTER_GTK_REQUIRED=1.1.2

TELEPATHY_GLIB_REQUIRED=0.23.2
AC_DEFINE(TP_VERSION_MIN_REQUIRED, TP_VERSION_0_20, [Ignore post 0.20 deprecations])
AC_DEFINE(TP_VERSION_MAX_ALLOWED, TP_VERSION_0_24, [Prevent post 0.24 APIs])
AC_DEFINE(TP_SEAL_ENABLE, 1, [Prevent to use sealed variables])
AC_DEFINE(TP_DISABLE_SINGLE_INCLUDE, 1, [Disable single include header])

COGL_REQUIRED=1.14
AC_DEFINE(COGL_VERSION_MIN_REQUIRED, COGL_VERSION_1_14, [Ignore post 1.14 deprecations])
AC_DEFINE(COGL_VERSION_MAX_ALLOWED, COGL_VERSION_1_14, [Ignore post 1.14 deprecations])

GSTREAMER_REQUIRED=0.10.32
TP_FS_REQUIRED=0.6.0
LIBSECRET_REQUIRED=0.5
GCR_REQUIRED=2.91.4
LIBCANBERRA_GTK_REQUIRED=0.25
LIBNOTIFY_REQUIRED=0.7.0
TELEPATHY_LOGGER=0.8.0
WEBKIT_REQUIRED=1.9.1
GOA_REQUIRED=3.5.1

# Optional deps
ENCHANT_REQUIRED=1.2.0
GEOCLUE_REQUIRED=2.1.0
GEOCODE_GLIB_REQUIRED=0.99.1
ISO_CODES_REQUIRED=0.35
NAUTILUS_SENDTO_REQUIRED=2.90.0
NETWORK_MANAGER_REQUIRED=0.7.0
CHAMPLAIN_REQUIRED=0.12.1
CHEESE_GTK_REQUIRED=3.4.0
LIBACCOUNTS_REQUIRED=1.4
LIBSIGNON_REQUIRED=1.8
MC_PLUGINS_REQUIRED=5.13.1

# Use --enable-maintainer-mode to disable deprecated symbols,
# disable single include and enable GSEAL. If this is not a released empathy,
# maintainer mode is forced
# FIXME: don't disable deprecated symbols until bgo #636654 has been fixed
#ifelse(empathy_released, 1, [], [enable_maintainer_mode="yes"])
#GNOME_MAINTAINER_MODE_DEFINES

# tp-account-widgets
prev_top_build_prefix=$ac_top_build_prefix
prev_ac_configure_args=$ac_configure_args
ac_configure_args="$ac_configure_args --with-pkgdatadir=${datadir}/empathy"
ac_configure_args="$ac_configure_args --with-gettext-package=empathy-tpaw"
ifelse(empathy_released, 1, [],
    [
        ac_configure_args="$ac_configure_args --enable-unreleased-checks"
    ])
AX_CONFIG_DIR([telepathy-account-widgets])
ac_top_build_prefix=$prev_top_build_prefix
ac_configure_args=$prev_ac_configure_args

export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:"$ac_top_build_prefix"telepathy-account-widgets/
PKG_CHECK_MODULES(TPAW, telepathy-account-widgets)

EMPATHY_UOA_PROVIDER=`pkg-config --variable=uoa_provider telepathy-account-widgets`
AC_DEFINE_UNQUOTED(EMPATHY_UOA_PROVIDER, "$EMPATHY_UOA_PROVIDER",
   [Name of provider for accounts imported from libaccounts])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_AUX_DIR(.)

AM_INIT_AUTOMAKE([1.11 no-define -Wno-portability tar-ustar no-dist-gzip dist-xz])

# Support silent build rules. Disable
# by either passing --disable-silent-rules to configure or passing V=1
# to make
AM_SILENT_RULES([yes])

# Check for programs
AC_PROG_CC
AC_HEADER_STDC
AC_PROG_MKDIR_P
AM_PATH_GLIB_2_0
AC_PATH_XTRA

# Initialize libtool
LT_PREREQ([2.2])
LT_INIT

AC_PATH_PROG(DBUS_BINDING_TOOL, dbus-binding-tool)
GLIB_GSETTINGS

GLIB_COMPILE_RESOURCES=`$PKG_CONFIG gio-2.0 --variable=glib_compile_resources`
AC_SUBST(GLIB_COMPILE_RESOURCES)
GLIB_GENMARSHAL=`$PKG_CONFIG glib-2.0 --variable=glib_genmarshal`
AC_SUBST(GLIB_GENMARSHAL)

AC_CHECK_PROGS([XSLTPROC], [xsltproc])
if test -z "$XSLTPROC"; then
  AC_MSG_ERROR([xsltproc (from libxslt) is required])
fi
AM_PATH_PYTHON([2.3])

EMPATHY_ARG_VALGRIND


# -----------------------------------------------------------
# Error flags
# -----------------------------------------------------------
AS_COMPILER_FLAG(-Wall, ERROR_CFLAGS="-Wall", ERROR_CFLAGS="")
AS_COMPILER_FLAG(-Werror, werror=yes, werror=no)

AC_ARG_ENABLE(Werror,
  AS_HELP_STRING([--disable-Werror],
                 [compile without -Werror (normally enabled in development builds)]),
    werror=$enableval, :)

AS_COMPILER_FLAG(-Wextra, wextra=yes, wextra=no)
AS_COMPILER_FLAG(-Wno-missing-field-initializers,
        wno_missing_field_initializers=yes,
        wno_missing_field_initializers=no)
AS_COMPILER_FLAG(-Wno-unused-parameter,
        wno_unused_parameter=yes,
        wno_unused_parameter=no)

ifelse(empathy_released, 1, [],
    [
        if test x$werror = xyes; then
            ERROR_CFLAGS="$ERROR_CFLAGS -Werror"
        fi
        if test x$wextra = xyes -a \
            x$wno_missing_field_initializers = xyes -a \
            x$wno_unused_parameter = xyes; then
            ERROR_CFLAGS="$ERROR_CFLAGS -Wextra -Wno-missing-field-initializers -Wno-unused-parameter"
        fi
    ])

AS_COMPILER_FLAG(-Wdeclaration-after-statement, ERROR_CFLAGS="$ERROR_CFLAGS -Wdeclaration-after-statement")
AS_COMPILER_FLAG(-Wshadow, ERROR_CFLAGS="$ERROR_CFLAGS -Wshadow")
AS_COMPILER_FLAG(-Wmissing-prototypes, ERROR_CFLAGS="$ERROR_CFLAGS -Wmissing-prototypes")
AS_COMPILER_FLAG(-Wmissing-declarations, ERROR_CFLAGS="$ERROR_CFLAGS -Wmissing-declarations")
AS_COMPILER_FLAG(-Wstrict-prototypes, ERROR_CFLAGS="$ERROR_CFLAGS -Wstrict-prototypes")

AC_SUBST(ERROR_CFLAGS)

AC_ARG_ENABLE(gprof,
            AS_HELP_STRING([--enable-gprof=@<:@no/yes@:>@],
            [build with support for gprof]),,
            [enable_gprof=no])
if test "x$enable_gprof" != "xno" ; then
    CFLAGS="$CFLAGS -pg"
    LDFLAGS="$LDFLAGS -pg"
fi

AC_SUBST(LDFLAGS)

# -----------------------------------------------------------
# Pkg-Config dependency checks
# -----------------------------------------------------------

AC_CHECK_LIBM
AC_SUBST(LIBM)

PKG_CHECK_MODULES(EMPATHY,
[
   dbus-glib-1
   folks >= $FOLKS_REQUIRED
   folks-telepathy >= $FOLKS_REQUIRED
   glib-2.0 >= $GLIB_REQUIRED
   gio-2.0 >= $GLIB_REQUIRED
   gio-unix-2.0 >= $GLIB_REQUIRED
   libsecret-1 >= $LIBSECRET_REQUIRED
   gnutls >= $GNUTLS_REQUIRED
   gmodule-export-2.0
   gobject-2.0
   gsettings-desktop-schemas
   libxml-2.0
   telepathy-glib >= $TELEPATHY_GLIB_REQUIRED
   telepathy-logger-0.2 >= $TELEPATHY_LOGGER
   x11
   gtk+-3.0 >= $GTK_REQUIRED
   libcanberra-gtk3 >= $LIBCANBERRA_GTK_REQUIRED
   libnotify >= $LIBNOTIFY_REQUIRED
   gcr-3 >= $GCR_REQUIRED
   libpulse
   libpulse-mainloop-glib
   webkitgtk-3.0 >= $WEBKIT_REQUIRED
   libsoup-2.4
   gee-0.8
])

# -----------------------------------------------------------
# GStreamer
# -----------------------------------------------------------
PKG_CHECK_MODULES(EMPATHY_CALL,
[
   farstream-0.2
   telepathy-farstream >= $TP_FS_REQUIRED
   clutter-1.0 >= $CLUTTER_REQUIRED
   clutter-gtk-1.0 >= $CLUTTER_GTK_REQUIRED
   clutter-gst-2.0
   gstreamer-1.0
   gstreamer-audio-1.0
   gstreamer-video-1.0
   cogl-1.0 >= $COGL_REQUIRED
])

AC_DEFINE(EMPATHY_GOA_PROVIDER, "org.gnome.OnlineAccounts",
   [Name of provider for accounts imported from GOA])

# -----------------------------------------------------------
# Enable debug
# -----------------------------------------------------------

AC_ARG_ENABLE(debug,
  AS_HELP_STRING([--disable-debug],
                 [compile without debug code]),
    enable_debug=$enableval, enable_debug=yes )

if test x$enable_debug = xyes; then
  AC_DEFINE(ENABLE_DEBUG, [], [Enable debug code])
fi

# -----------------------------------------------------------
# Language Support
# -----------------------------------------------------------
IT_PROG_INTLTOOL([0.50.0])

GETTEXT_PACKAGE=empathy
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE",[Gettext package name])

# -----------------------------------------------------------
# gudev
# -----------------------------------------------------------
AC_ARG_ENABLE(gudev,
              AS_HELP_STRING([--enable-gudev=@<:@no/yes/auto@:>@],
                             [build with gudev support]), ,
                             enable_gudev=auto)

if test "x$enable_gudev" != "xno"; then

   PKG_CHECK_MODULES(UDEV, [gudev-1.0],
        have_gudev="yes", have_gudev="no")

   if test "x$have_gudev" = "xyes"; then
      AC_DEFINE(HAVE_UDEV, 1, [Define if you have gudev])
   fi
else
   have_gudev=no
fi

if test "x$enable_gudev" = "xyes" -a "x$have_gudev" != "xyes"; then
   AC_MSG_ERROR([Could not find gudev dependencies:

$UDEV_PKG_ERRORS])
fi
AM_CONDITIONAL(HAVE_UDEV, test "x$have_gudev" = "xyes")

# -----------------------------------------------------------
# spellchecking checks: enchant and iso-codes
# -----------------------------------------------------------
AC_ARG_ENABLE(spell,
              AS_HELP_STRING([--enable-spell=@<:@no/yes/auto@:>@],
                             [Enable spell checking]), ,
                             enable_spell=auto)

if test "x$enable_spell" != "xno"; then
   PKG_CHECK_MODULES(ENCHANT,
   [
      enchant >= $ENCHANT_REQUIRED,
      iso-codes >= $ISO_CODES_REQUIRED
   ], have_enchant="yes", have_enchant="no")

   if test "x$have_enchant" = "xyes"; then
      AC_MSG_CHECKING([whether iso-codes has iso-639 and iso-3166 domains])
      if $PKG_CONFIG --variable=domains iso-codes | grep 639 > /dev/null && \
         $PKG_CONFIG --variable=domains iso-codes | grep 3166 > /dev/null ; then
            AC_DEFINE_UNQUOTED(ISO_CODES_PREFIX, ["`$PKG_CONFIG --variable=prefix iso-codes`"], [ISO codes prefix])
            AC_DEFINE(HAVE_ENCHANT, [], [Define if you have libenchant])
            result=yes
      else
            result=no
            have_enchant="no"
      fi
      AC_MSG_RESULT([$result])
   fi
else
   have_enchant=no
fi

if test "x$enable_spell" = "xyes" -a "x$have_enchant" != "xyes"; then
   AC_MSG_ERROR([Could not find spell-checking dependencies:

$ENCHANT_PKG_ERRORS])
fi

AM_CONDITIONAL(HAVE_ENCHANT, test "x$have_enchant" = "xyes")

# -----------------------------------------------------------
# Map view checks: libchamplain
# -----------------------------------------------------------
AC_ARG_ENABLE(map,
              AS_HELP_STRING([--enable-map=@<:@no/yes/auto@:>@],
                             [Enable map view]), ,
                             enable_map=auto)

if test "x$enable_map" != "xno"; then

   PKG_CHECK_MODULES(LIBCHAMPLAIN,
    [
       clutter-gtk-1.0
       champlain-gtk-0.12
       champlain-0.12 >= $CHAMPLAIN_REQUIRED
    ], have_libchamplain="yes", have_libchamplain="no")

   if test "x$have_libchamplain" = "xyes"; then
      AC_DEFINE(HAVE_LIBCHAMPLAIN, 1, [Define if you have libchamplain])
   fi
else
   have_libchamplain=no
fi

if test "x$enable_map" = "xyes" -a "x$have_libchamplain" != "xyes"; then
   AC_MSG_ERROR([Could not find map view dependencies:

$LIBCHAMPLAIN_PKG_ERRORS])
fi

AM_CONDITIONAL(HAVE_LIBCHAMPLAIN, test "x$have_libchamplain" = "xyes")

# -----------------------------------------------------------
# location checks: geoclue
# -----------------------------------------------------------
AC_ARG_ENABLE(location,
              AS_HELP_STRING([--enable-location=@<:@no/yes/auto@:>@],
                             [Enable location awareness]), ,
                             enable_location=auto)

if test "x$enable_location" != "xno"; then
   PKG_CHECK_MODULES(GEOCLUE,
   [
      geoclue-2.0 >= $GEOCLUE_REQUIRED
   ], have_geoclue="yes", have_geoclue="no")

   if test "x$have_geoclue" = "xyes"; then
      GEOCLUE_XML_FILE=`pkg-config --variable=dbus_interface geoclue-2.0`
      if test "x$GEOCLUE_XML_FILE" = "x"; then
        echo "Can't find dbus_interface variable in geoclue-2.0.pc"
        have_geoclue="no"
      else
        AC_DEFINE(HAVE_GEOCLUE, 1, [Define if you have geoclue-2])
        AC_SUBST(GEOCLUE_XML_FILE)
      fi
   fi
else
   have_geoclue="no"
fi

if test "x$enable_location" = "xyes" -a "x$have_geoclue" != "xyes"; then
   AC_MSG_ERROR([Could not find location dependencies:

$GEOCLUE_PKG_ERRORS])
fi

AM_CONDITIONAL(HAVE_GEOCLUE, test "x$have_geoclue" = "xyes")
AC_SUBST(GEOCLUE_CFLAGS)
AC_SUBST(GEOCLUE_LIBS)

# -----------------------------------------------------------
# location checks: geocode-glib
# -----------------------------------------------------------
AC_ARG_ENABLE(geocode,
              AS_HELP_STRING([--enable-geocode=@<:@no/yes/auto@:>@],
                             [Enable geocode support]), ,
                             enable_geocode=auto)

if test "x$enable_geocode" != "xno"; then
   PKG_CHECK_MODULES(GEOCODE,
   [
      geocode-glib-1.0 >= $GEOCODE_GLIB_REQUIRED
   ], have_geocode="yes", have_geocode="no")

   if test "x$have_geocode" = "xyes"; then
      AC_DEFINE(HAVE_GEOCODE, 1, [Define if you have geocode])
   fi
else
   have_geocode="no"
fi

if test "x$enable_geocode" = "xyes" -a "x$have_geocode" != "xyes"; then
   AC_MSG_ERROR([Could not find geocode dependencies:

$GEOCODE_PKG_ERRORS])
fi

AM_CONDITIONAL(HAVE_GEOCODE, test "x$have_geocode" = "xyes")
AC_SUBST(GEOCODE_CFLAGS)
AC_SUBST(GEOCODE_LIBS)

# -----------------------------------------------------------
# goa-mc-plugin
# -----------------------------------------------------------
AC_ARG_ENABLE(goa,
              AS_HELP_STRING([--enable-goa=@<:@no/yes/auto@:>@],
                             [build GOA MC plugin]), ,
                             enable_goa=auto)

if test "x$enable_goa" != "xno"; then
   PKG_CHECK_MODULES(GOA,
    [
       mission-control-plugins
       goa-1.0 >= $GOA_REQUIRED
    ], have_goa="yes", have_goa="no")
else
   have_goa=no
fi

if test "x$enable_goa" = "xyes" -a "x$have_goa" != "xyes"; then
   AC_MSG_ERROR([Could not find GOA dependencies:

$GOA_PKG_ERRORS])
fi

if test "x$have_goa" = "xyes"; then
   AC_DEFINE(HAVE_GOA, 1, [Define to 1 to enable GNOME Online Accounts])
fi

AM_CONDITIONAL(HAVE_GOA, test "x$have_goa" = "xyes")

# Optional dependency for avatar selection
AC_ARG_WITH([cheese],
            AS_HELP_STRING([--with-cheese],
                           [enable cheese webcam support]),,
            with_cheese=auto)

if test x"$with_cheese" != x"no" ; then
   PKG_CHECK_MODULES(CHEESE, cheese-gtk >= CHEESE_GTK_REQUIRED, [have_cheese=yes], [have_cheese=no])
   if test x${have_cheese} = xyes; then
     AC_DEFINE(HAVE_CHEESE, 1, [Define to 1 to enable cheese webcam support])
   fi
   if test x${with_cheese} = xyes && test x${have_cheese} = xno; then
     AC_MSG_ERROR([Cheese configured but not found:

$CHEESE_PKG_ERRORS])
   fi
else
   have_cheese=no
fi
AM_CONDITIONAL(BUILD_CHEESE, test x${have_cheese} = xyes)

# -----------------------------------------------------------
# ubuntu-online-accounts support
# -----------------------------------------------------------
AC_ARG_ENABLE(ubuntu-online-accounts,
              AS_HELP_STRING([--enable-ubuntu-online-accounts=@<:@no/yes/auto@:>@],
                             [build Ubuntu Online Accounts plugins]), ,
                             enable_ubuntu_online_accounts=auto)

if test "x$enable_ubuntu_online_accounts" != "xno"; then
   PKG_CHECK_MODULES(UOA,
    [
       account-plugin
       mission-control-plugins >= $MC_PLUGINS_REQUIRED
       libaccounts-glib >= $LIBACCOUNTS_REQUIRED
       libsignon-glib >= $LIBSIGNON_REQUIRED
    ], have_uoa="yes", have_uoa="no")

   # provider plugin dir
   AC_MSG_CHECKING([Accounts provider plugin dir])
   ACCOUNTS_PROVIDER_PLUGIN_DIR=`pkg-config --variable=provider_plugindir account-plugin`

   AC_MSG_RESULT([$ACCOUNTS_PROVIDER_PLUGIN_DIR])
   AC_SUBST(ACCOUNTS_PROVIDER_PLUGIN_DIR)

   # app plugin dir
   AC_MSG_CHECKING([Accounts provider app plugin dir])
   ACCOUNTS_APP_PLUGIN_DIR=`pkg-config --variable=application_plugindir account-plugin`

   AC_MSG_RESULT([$ACCOUNTS_APP_PLUGIN_DIR])
   AC_SUBST(ACCOUNTS_APP_PLUGIN_DIR)

   # provider files dir
   AC_MSG_CHECKING([Accounts provider files dir])
   ACCOUNTS_PROVIDER_FILES_DIR=`pkg-config --variable=providerfilesdir libaccounts-glib`

   AC_MSG_RESULT([$ACCOUNTS_PROVIDER_FILES_DIR])
   AC_SUBST(ACCOUNTS_PROVIDER_FILES_DIR)

   # service files dir
   AC_MSG_CHECKING([Accounts service files dir])
   ACCOUNTS_SERVICE_FILES_DIR=`pkg-config --variable=servicefilesdir libaccounts-glib`

   AC_MSG_RESULT([$ACCOUNTS_SERVICE_FILES_DIR])
   AC_SUBST(ACCOUNTS_SERVICE_FILES_DIR)

   # application files dir
   AC_MSG_CHECKING([Accounts applications files dir])
   ACCOUNTS_APPLICATION_FILES_DIR=`pkg-config --variable=applicationfilesdir libaccounts-glib`

   AC_MSG_RESULT([$ACCOUNTS_APPLICATION_FILES_DIR])
   AC_SUBST(ACCOUNTS_APPLICATION_FILES_DIR)

else
   have_uoa=no
fi

if test "x$enable_ubuntu_online_accounts" = "xyes" -a "x$have_uoa" != "xyes"; then
   AC_MSG_ERROR([Could not find Ubuntu Online Accounts dependencies:

$UOA_PKG_ERRORS])
fi

if test "x$have_uoa" = "xyes"; then
   AC_DEFINE(HAVE_UOA, 1, [Define to 1 to build Ubuntu Online Accounts plugins])
fi

AM_CONDITIONAL(HAVE_UOA, test "x$have_uoa" = "xyes")

if test "x$have_uoa" = "xyes" -o "x$have_goa" = "xyes"; then
   AC_MSG_CHECKING([Mission Control plugins dir])
   MISSION_CONTROL_PLUGINS_DIR=`pkg-config --define-variable=libdir='${libdir}' --variable=plugindir mission-control-plugins`

   AC_MSG_RESULT([$MISSION_CONTROL_PLUGINS_DIR])
   AC_SUBST(MISSION_CONTROL_PLUGINS_DIR)
fi


# Help documentation
YELP_HELP_INIT

# -----------------------------------------------------------
# Coding style checks
# -----------------------------------------------------------
AC_ARG_ENABLE(coding-style-checks,
  AS_HELP_STRING([--disable-coding-style-checks],
                 [do not check coding style using grep]),
    [ENABLE_CODING_STYLE_CHECKS=$enableval],
    [ENABLE_CODING_STYLE_CHECKS=yes])

AC_SUBST([ENABLE_CODING_STYLE_CHECKS])
# -----------------------------------------------------------

AC_CONFIG_FILES([
   Makefile
   data/Makefile
   data/empathy.desktop.in
   data/icons/Makefile
   data/themes/Makefile
   extensions/Makefile
   po/Makefile.in
   libempathy/Makefile
   libempathy-gtk/Makefile
   src/Makefile
   goa-mc-plugin/Makefile
   ubuntu-online-accounts/Makefile
   ubuntu-online-accounts/mc-plugin/Makefile
   ubuntu-online-accounts/cc-plugins/Makefile
   ubuntu-online-accounts/cc-plugins/account-plugins/Makefile
   ubuntu-online-accounts/cc-plugins/providers/Makefile
   ubuntu-online-accounts/cc-plugins/services/Makefile
   ubuntu-online-accounts/cc-plugins/app-plugin/Makefile
   help/Makefile
   tests/Makefile
   tests/interactive/Makefile
   tests/xml/Makefile
   tests/certificates/Makefile
   tools/Makefile
])
AC_OUTPUT

echo "
Configure summary:

	Compiler....................:  ${CC}
	Compiler Flags..............:  ${CFLAGS} ${ERROR_CFLAGS}
	Linker Flags................:  ${LDFLAGS}
	Prefix......................:  ${prefix}
	Coding style checks.........:  ${ENABLE_CODING_STYLE_CHECKS}

    Features:
	Spell checking (enchant)....:  ${have_enchant}
	Display maps (libchamplain).:  ${have_libchamplain}
	Location awareness (Geoclue):  ${have_geoclue}
	Geocode support (Geocode)...:  ${have_geocode}
	Cheese webcam support ......:  ${have_cheese}
	Camera monitoring...........:  ${have_gudev}

    Extras:
	GOA MC plugin...............:  ${have_goa}
	Ubuntu Online plugins.......:  ${have_uoa}
"
