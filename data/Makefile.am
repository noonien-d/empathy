SUBDIRS = icons themes

desktopdir = $(datadir)/applications
desktop_in_files = \
	empathy.desktop.in \
	$(NULL)
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)
@INTLTOOL_DESKTOP_RULE@

gsettings_SCHEMAS = org.gnome.Empathy.gschema.xml
@GSETTINGS_RULES@

# We need to compile schemas at make time
# to run from source tree
gschemas.compiled: $(gsettings_SCHEMAS:.xml=.valid)
	$(AM_V_GEN) $(GLIB_COMPILE_SCHEMAS) --targetdir=. .

all-local: gschemas.compiled

convertdir = $(datarootdir)/GConf/gsettings
convert_DATA = \
	empathy.convert

servicefiledir = $(datadir)/dbus-1/services
servicefile_in_files = \
	org.freedesktop.Telepathy.Client.Empathy.Chat.service.in \
	org.freedesktop.Telepathy.Client.Empathy.Auth.service.in \
	org.freedesktop.Telepathy.Client.Empathy.Call.service.in \
	org.freedesktop.Telepathy.Client.Empathy.FileTransfer.service.in

servicefile_DATA = $(servicefile_in_files:.service.in=.service)

org.freedesktop.Telepathy.Client.Empathy.Chat.service: org.freedesktop.Telepathy.Client.Empathy.Chat.service.in
	$(AM_V_GEN)sed -e "s|[@]libexecdir[@]|$(libexecdir)|" $< > $@

org.freedesktop.Telepathy.Client.Empathy.Auth.service: org.freedesktop.Telepathy.Client.Empathy.Auth.service.in
	$(AM_V_GEN)sed -e "s|[@]libexecdir[@]|$(libexecdir)|" $< > $@

org.freedesktop.Telepathy.Client.Empathy.Call.service: org.freedesktop.Telepathy.Client.Empathy.Call.service.in
	$(AM_V_GEN)sed -e "s|[@]libexecdir[@]|$(libexecdir)|" $< > $@

org.freedesktop.Telepathy.Client.Empathy.FileTransfer.service: org.freedesktop.Telepathy.Client.Empathy.FileTransfer.service.in
	$(AM_V_GEN)sed -e "s|[@]bindir[@]|$(bindir)|" $< > $@

clientfiledir = $(datarootdir)/telepathy/clients
clientfile_DATA = \
	Empathy.Chat.client \
	Empathy.Auth.client \
	Empathy.Call.client \
	Empathy.FileTransfer.client

htmldir = $(datadir)/empathy
html_DATA = \
	Template.html \
	empathy-log-window.html \
	$(NULL)

cssdir = $(datadir)/empathy
css_DATA = \
	empathy.css \
	$(NULL)

appdatadir = $(datadir)/metainfo
appdata_in_files = empathy.appdata.xml.in
appdata_DATA = $(appdata_in_files:.xml.in=.xml)
@INTLTOOL_XML_RULE@

EXTRA_DIST = 				\
	$(appdata_in_files)		\
	$(convert_DATA)			\
	$(desktop_in_files)		\
	$(desktop_DATA)			\
	$(schemas_DATA)			\
	$(html_DATA)			\
	$(clientfile_DATA)		\
	$(servicefile_in_files)		\
	$(gsettings_SCHEMAS)		\
	$(css_DATA)			\
	$(NULL)
CLEANFILES = \
	$(appdata_DATA)


DISTCLEANFILES = 		\
	$(desktop_DATA)		\
	$(schemas_DATA)		\
	$(servicefile_DATA)	\
	$(css_DATA)		\
	gschemas.compiled	\
	$(NULL)
