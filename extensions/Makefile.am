tools_dir = $(top_srcdir)/tools

AM_CPPFLAGS = $(DISABLE_DEPRECATED)

AM_CFLAGS = \
    -I$(top_srcdir) \
    -I$(top_builddir) \
    $(WARN_CFLAGS) \
    $(EMPATHY_CFLAGS)

EXTRA_DIST = \
    all.xml \
    generic-types.xml \
    misc.xml \
    Logger.xml \
    Channel_Interface_Credentials_Storage.xml \
    $(NULL)

noinst_LTLIBRARIES = libemp-extensions.la

libemp_extensions_la_LIBADD = \
    $(TP_GLIB_LIBS)

# The client-specific parts are built into a separate .o file, so the linker
# can discard them when linking services. The service-specific parts are
# in svc-*.c, so we don't need an extensions-svc.c.
libemp_extensions_la_SOURCES = \
    extensions.c \
    extensions-cli.c \
    extensions.h

nodist_libemp_extensions_la_SOURCES = \
    _gen/register-dbus-glib-marshallers-body.h \
    _gen/enums.h \
    _gen/gtypes.h \
    _gen/gtypes-body.h \
    _gen/interfaces.h \
    _gen/interfaces-body.h \
    _gen/cli-misc.h \
    _gen/cli-misc-body.h \
    _gen/svc-misc.h \
    _gen/svc-misc.c

BUILT_SOURCES = \
    _gen/all.xml \
    _gen/misc.xml \
    $(nodist_libemp_extensions_la_SOURCES) \
    extensions.html

CLEANFILES = $(BUILT_SOURCES)

clean-local:
	rm -rf _gen

XSLTPROCFLAGS = --nonet --novalid

# Generated files which can be generated for all categories simultaneously

_gen/all.xml: all.xml $(wildcard *.xml) $(tools_dir)/xincludator.py
	@$(MKDIR_P) _gen
	$(AM_V_GEN)$(PYTHON) $(tools_dir)/xincludator.py $< > $@

extensions.html: _gen/all.xml $(tools_dir)/doc-generator.xsl
	$(AM_V_GEN)$(XSLTPROC) $(XSLTPROCFLAGS) \
		--param "allow-undefined-interfaces" "true()" \
		$(tools_dir)/doc-generator.xsl \
		$< > $@

_gen/gt%pes.h _gen/gt%pes-body.h: _gen/all.xml \
	$(top_srcdir)/tools/glib-gtypes-generator.py
	$(AM_V_GEN)$(PYTHON) $(top_srcdir)/tools/glib-gtypes-generator.py \
		$< _gen/gtypes Emp

_gen/register-dbus-glib-marshallers-body.h: _gen/all.xml \
	$(tools_dir)/glib-client-marshaller-gen.py
	$(AM_V_GEN)$(PYTHON) $(tools_dir)/glib-client-marshaller-gen.py $< \
		_emp_ext > $@

_gen/enums.h: _gen/all.xml \
	$(tools_dir)/c-constants-gen.py
	$(AM_V_GEN)$(PYTHON) $(tools_dir)/c-constants-gen.py \
		Emp \
		$< _gen/enums

_gen/int%rfaces-body.h _gen/int%rfaces.h: _gen/all.xml \
	$(tools_dir)/glib-interfaces-gen.py
	$(AM_V_GEN)$(PYTHON) $(tools_dir)/glib-interfaces-gen.py \
		Emp _gen/interfaces-body.h _gen/interfaces.h $<

# Generated files which must be generated per "category". Each TpProxy
# subclass you want to use with --subclass will need to have its own category,
# although you can subdivide further if you want.

_gen/misc.xml: misc.xml $(wildcard *.xml) $(tools_dir)/xincludator.py
	@$(MKDIR_P) _gen
	$(AM_V_GEN)$(PYTHON) $(tools_dir)/xincludator.py $< > $@

_gen/cli-misc-body.h: _gen/cli-misc.h

_gen/cli-misc.h: _gen/misc.xml \
	$(tools_dir)/glib-client-gen.py
	$(AM_V_GEN)$(PYTHON) $(tools_dir)/glib-client-gen.py \
		--group=misc \
		--subclass=TpProxy \
		--subclass-assert=TP_IS_PROXY \
		--iface-quark-prefix=EMP_IFACE_QUARK \
		--tp-proxy-api=0.10.0 \
		$< Emp_Cli _gen/cli-misc

# There is no need to execute glib-ginterface-gen.py twice because it
# generates both the .c and .h files in one shot. Therefore, merely
# having one of them (say the .c) depend on the other (say the .h) is
# enough.
#
# Moreover, running it twice breaks parallel builds because one
# glib-ginterface-gen.py process steps on the other.

_gen/svc-misc.c: _gen/svc-misc.h

_gen/svc-misc.h: _gen/misc.xml \
	$(tools_dir)/glib-ginterface-gen.py
	$(AM_V_GEN)$(PYTHON) $(tools_dir)/glib-ginterface-gen.py \
		--filename=_gen/svc-misc \
		--signal-marshal-prefix=_emp_ext \
		--include='<telepathy-glib/telepathy-glib.h>' \
		--not-implemented-func='tp_dbus_g_method_return_not_implemented' \
		--allow-unstable \
		$< Emp_Svc_
