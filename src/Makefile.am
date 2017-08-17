include $(top_srcdir)/tools/flymake.mk

CPPFLAGS_COMMON =					\
	$(EMPATHY_CFLAGS)				\
	$(TPAW_CFLAGS)					\
	$(ERROR_CFLAGS)					\
	-I$(top_srcdir)/libempathy-gtk			\
	-I$(top_srcdir)/libempathy			\
	-I$(top_srcdir)/extensions			\
	-I$(top_builddir)/libempathy-gtk		\
	-I$(top_builddir)/libempathy			\
	-DG_LOG_DOMAIN=\"empathy\"			\
	-DBIN_DIR=\"$(bindir)\"                         \
	$(DISABLE_DEPRECATED)				\
	$(WARN_CFLAGS)					\
	$(NULL)

AM_LDFLAGS = $(LIBM)
AM_CPPFLAGS =						\
	$(CPPFLAGS_COMMON)				\
	$(LIBCHAMPLAIN_CFLAGS)				\
	$(UOA_CFLAGS)					\
	$(NULL)

LDADD =								\
	$(top_builddir)/libempathy-gtk/libempathy-gtk.la	\
	$(top_builddir)/libempathy/libempathy.la		\
	$(top_builddir)/extensions/libemp-extensions.la		\
	$(GCR_LIBS) 						\
	$(EMPATHY_LIBS)						\
	$(TPAW_LIBS)						\
	$(LIBCHAMPLAIN_LIBS)					\
	$(UOA_LIBS)						\
	$(NULL)

noinst_LTLIBRARIES = libempathy-accounts-common.la

libempathy_accounts_common_la_SOURCES =					\
	empathy-accounts-common.c empathy-accounts-common.h		\
	empathy-accounts-dialog.c empathy-accounts-dialog.h		\
	empathy-import-dialog.c empathy-import-dialog.h			\
	empathy-import-pidgin.c empathy-import-pidgin.h			\
	empathy-import-widget.c empathy-import-widget.h			\
	empathy-import-utils.c empathy-import-utils.h			\
	$(NULL)

libempathy_accounts_common_la_LIBADD =					\
        $(top_builddir)/libempathy-gtk/libempathy-gtk.la		\
        $(EMPATHY_LIBS)							\
        $(LIBCHAMPLAIN_LIBS)						\
	$(NULL)

bin_PROGRAMS =			\
	empathy			\
	empathy-accounts	\
	empathy-debugger	\
	$(NULL)

libexec_PROGRAMS = \
	empathy-auth-client \
	empathy-call

empathy_accounts_SOURCES =						\
	empathy-accounts.c empathy-accounts.h				\
	$(NULL)

empathy_accounts_CPPFLAGS =						\
	$(AM_CPPFLAGS) \
	$(CHEESE_CFLAGS) \
	$(NULL)

empathy_accounts_LDADD =						\
	$(LDADD)							\
	libempathy-accounts-common.la					\
	$(NULL)

empathy_debugger_SOURCES =						\
	empathy-debug-window.c empathy-debug-window.h			\
	empathy-debugger.c		 				\
	$(NULL)

empathy_auth_client_SOURCES =						\
	empathy-sanity-cleaning.c empathy-sanity-cleaning.h \
	empathy-auth-client.c \
	$(NULL)

empathy_call_SOURCES = \
       empathy-call.c \
       empathy-call-factory.c \
       empathy-call-factory.h \
       empathy-call-handler.c \
       empathy-call-handler.h \
       empathy-call-window.c \
       empathy-call-window.h \
       empathy-call-window-fullscreen.c \
       empathy-call-window-fullscreen.h \
       empathy-about-dialog.c \
       empathy-about-dialog.h \
       empathy-audio-sink.c \
       empathy-audio-sink.h \
       empathy-audio-src.c \
       empathy-audio-src.h \
       empathy-audio-utils.c \
       empathy-audio-utils.h \
       empathy-video-src.c \
       empathy-video-src.h \
       empathy-preferences.c \
       empathy-preferences.h \
       empathy-camera-menu.c \
       empathy-camera-menu.h \
       empathy-mic-menu.c \
       empathy-mic-menu.h \
       empathy-rounded-actor.c \
       empathy-rounded-actor.h \
       empathy-rounded-rectangle.c \
       empathy-rounded-rectangle.h \
       empathy-rounded-effect.c \
       empathy-rounded-effect.h \
       empathy-mic-monitor.c \
       empathy-mic-monitor.h

empathy_call_CFLAGS = $(EMPATHY_CALL_CFLAGS) -DGST_USE_UNSTABLE_API
empathy_call_LDFLAGS = $(EMPATHY_CALL_LIBS)

empathy_handwritten_source = \
	empathy-about-dialog.c empathy-about-dialog.h			\
	empathy-chat-window.c empathy-chat-window.h			\
	empathy-chatrooms-window.c empathy-chatrooms-window.h		\
	empathy-event-manager.c empathy-event-manager.h			\
	empathy-ft-manager.c empathy-ft-manager.h			\
	empathy-invite-participant-dialog.c empathy-invite-participant-dialog.h \
	empathy-roster-window.c empathy-roster-window.h			\
	empathy-new-chatroom-dialog.c empathy-new-chatroom-dialog.h	\
	empathy-notifications-approver.c empathy-notifications-approver.h \
	empathy-call-observer.c empathy-call-observer.h			\
	empathy-preferences.c empathy-preferences.h			\
	empathy-status-icon.c empathy-status-icon.h			\
	empathy-chat-manager.c empathy-chat-manager.h			\
	polari-fixed-size-frame.c  polari-fixed-size-frame.h \
	empathy.c

empathy_SOURCES =							\
	$(empathy_handwritten_source)					\
	chat-manager-interface.c chat-manager-interface.h \
	$(NULL)

empathy_LDADD =								\
	libempathy-accounts-common.la					\
        $(top_builddir)/libempathy-gtk/libempathy-gtk.la		\
        $(top_builddir)/libempathy/libempathy.la			\
        $(top_builddir)/extensions/libemp-extensions.la			\
        $(EMPATHY_LIBS)							\
        $(LIBCHAMPLAIN_LIBS)						\
	$(NULL)

check_c_sources = \
    $(empathy_handwritten_source) \
    $(empathy_logs_SOURCES) \
    $(libempathy_accounts_common_la_SOURCES) \
    $(empathy_accounts_SOURCES) \
    $(empathy_debugger_SOURCES) \
    $(empathy_auth_client_SOURCES) \
    $(empathy_call_SOURCES)

include $(top_srcdir)/tools/check-coding-style.mk
check-local: check-coding-style

uidir = $(datadir)/empathy
ui_DATA = 					\
	empathy-accounts-dialog.ui		\
	empathy-call-window-fullscreen.ui	\
	empathy-call-window.ui			\
	empathy-chat-window.ui			\
	empathy-chatrooms-window.ui		\
	empathy-ft-manager.ui			\
	empathy-import-dialog.ui		\
	empathy-roster-window.ui			\
	empathy-roster-window-menubar.ui		\
	empathy-new-chatroom-dialog.ui		\
	empathy-preferences.ui			\
	empathy-status-icon.ui

EXTRA_DIST =			\
	$(autostart_DATA)	\
	$(ui_DATA)		\
	empathy-chat.js		\
	empathy-chat.gresource.xml \
	Chat_Manager.xml

dist_man_MANS =			\
	empathy.1 \
	empathy-accounts.1

chat-manager-interface.c: chat-manager-interface.h
chat-manager-interface.h: Makefile.am Chat_Manager.xml
	gdbus-codegen \
		--interface-prefix org.gnome.Empathy. \
		--c-namespace EmpathyGen \
		--generate-c-code chat-manager-interface \
		Chat_Manager.xml

BUILT_SOURCES = \
	chat-manager-interface.c \
	chat-manager-interface.h \
	$(NULL)

CLEANFILES = $(BUILT_SOURCES)

chat_resource_files: $(shell $(GLIB_COMPILE_RESOURCES) --generate-dependencies --sourcedir=$(srcdir) $(srcdir)/empathy-chat.gresource.xml)

empathy-chat-resources.c: empathy-chat.gresource.xml $(chat_resource_files)
	$(AM_V_GEN)$(GLIB_COMPILE_RESOURCES) --target=$@ --sourcedir=$(srcdir) --generate-source $<

empathy-chat-resources.h: empathy-chat.gresource.xml $(chat_resource_files)
	$(AM_V_GEN)$(GLIB_COMPILE_RESOURCES) --target=$@ --sourcedir=$(srcdir) --generate-header $<
