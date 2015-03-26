NULL =

public_icons_themes = \
	hicolor	\
	$(NULL)

public_icons = \
	hicolor_apps_16x16_empathy.png \
	hicolor_apps_22x22_empathy.png \
	hicolor_apps_24x24_empathy.png \
	hicolor_apps_32x32_empathy.png \
	hicolor_apps_48x48_empathy.png \
	hicolor_apps_256x256_empathy.png \
	hicolor_apps_scalable_empathy-symbolic.svg \
	$(NULL)

private_icons_themes = \
	hicolor	\
	$(NULL)

private_icons = \
	hicolor_actions_16x16_im-message-new.png \
	hicolor_actions_22x22_im-message-new.png \
	hicolor_actions_24x24_im-message-new.png \
	hicolor_status_16x16_im-message.png \
	hicolor_status_22x22_im-message.png \
	hicolor_status_16x16_user-typing.png \
	hicolor_status_16x16_user-available.png \
	hicolor_status_16x16_user-away.png \
	hicolor_status_16x16_user-busy.png \
	hicolor_status_16x16_user-idle.png \
	hicolor_status_16x16_user-offline.png \
	hicolor_status_16x16_empathy-pending.png \
	hicolor_status_22x22_user-available.png \
	hicolor_status_22x22_user-away.png \
	hicolor_status_22x22_user-busy.png \
	hicolor_status_22x22_user-idle.png \
	hicolor_status_22x22_user-offline.png \
	hicolor_status_22x22_empathy-pending.png \
	hicolor_status_24x24_user-available.png \
	hicolor_status_24x24_user-away.png \
	hicolor_status_24x24_user-busy.png \
	hicolor_status_24x24_user-idle.png \
	hicolor_status_24x24_user-offline.png \
	hicolor_status_24x24_empathy-pending.png \
	hicolor_status_32x32_user-available.png \
	hicolor_status_32x32_user-away.png \
	hicolor_status_32x32_user-busy.png \
	hicolor_status_32x32_user-idle.png \
	hicolor_status_32x32_user-offline.png \
	hicolor_status_32x32_empathy-pending.png \
	hicolor_status_48x48_user-available.png \
	hicolor_status_48x48_user-away.png \
	hicolor_status_48x48_user-busy.png \
	hicolor_status_48x48_user-idle.png \
	hicolor_status_48x48_user-offline.png \
	hicolor_status_48x48_empathy-pending.png \
	hicolor_status_scalable_user-available.svg \
	hicolor_status_scalable_user-away.svg \
	hicolor_status_scalable_user-busy.svg \
	hicolor_status_scalable_user-idle.svg \
	hicolor_status_scalable_user-offline.svg \
	hicolor_status_scalable_empathy-pending.svg \
	$(NULL)

EXTRA_DIST = \
	$(public_icons)		\
	$(private_icons)	\
	empathy.svg		\
	$(NULL)

include $(srcdir)/utils.mk

###############################################################################

gtk_update_icon_cache = gtk-update-icon-cache -f -t

update-icon-cache:
	@-if test -z "$(DESTDIR)"; then \
		echo "Updating Gtk icon cache."; \
		for theme in $(public_icons_themes); do \
			$(gtk_update_icon_cache) $(datadir)/icons/$$theme; \
		done; \
		for theme in $(private_icons_themes); do \
			$(gtk_update_icon_cache) $(pkgdatadir)/icons/$$theme; \
		done; \
	else \
		echo "*** Icon cache not updated.  After (un)install, run this:"; \
		for theme in $(public_icons_themes); do \
			echo "***   $(gtk_update_icon_cache) $(datadir)/icons/$$theme"; \
		done; \
		for theme in $(private_icons_themes); do \
			echo "***   $(gtk_update_icon_cache) $(pkgdatadir)/icons/$$theme"; \
		done; \
	fi

install-icons:
	for icon in $(public_icons); do \
		THEME=`echo $$icon | cut -d_ -f1`; \
		CONTEXT=`echo $$icon | cut -d_ -f2`; \
		SIZE=`echo $$icon | cut -d_ -f3`; \
		ICONFILE=`echo $$icon | cut -d_ -f4`; \
		mkdir -p $(DESTDIR)$(datadir)/icons/$$THEME/$$SIZE/$$CONTEXT; \
		$(INSTALL_DATA) $(srcdir)/$$icon $(DESTDIR)$(datadir)/icons/$$THEME/$$SIZE/$$CONTEXT/$$ICONFILE; \
	done; \
	for icon in $(private_icons); do \
		THEME=`echo $$icon | cut -d_ -f1`; \
		CONTEXT=`echo $$icon | cut -d_ -f2`; \
		SIZE=`echo $$icon | cut -d_ -f3`; \
		ICONFILE=`echo $$icon | cut -d_ -f4`; \
		mkdir -p $(DESTDIR)$(pkgdatadir)/icons/$$THEME/$$SIZE/$$CONTEXT; \
		$(INSTALL_DATA) $(srcdir)/$$icon $(DESTDIR)$(pkgdatadir)/icons/$$THEME/$$SIZE/$$CONTEXT/$$ICONFILE; \
	done

uninstall-icons:
	-for icon in $(public_icons); do \
		THEME=`echo $$icon | cut -d_ -f1`; \
		CONTEXT=`echo $$icon | cut -d_ -f2`; \
		SIZE=`echo $$icon | cut -d_ -f3`; \
		ICONFILE=`echo $$icon | cut -d_ -f4`; \
		rm -f $(DESTDIR)$(datadir)/icons/$$THEME/$$SIZE/$$CONTEXT/$$ICONFILE; \
	done; \
	for icon in $(private_icons); do \
		THEME=`echo $$icon | cut -d_ -f1`; \
		CONTEXT=`echo $$icon | cut -d_ -f2`; \
		SIZE=`echo $$icon | cut -d_ -f3`; \
		ICONFILE=`echo $$icon | cut -d_ -f4`; \
		rm -f $(DESTDIR)$(pkgdatadir)/icons/$$THEME/$$SIZE/$$CONTEXT/$$ICONFILE; \
	done

install-data-local:
	$(MAKE) install-icons
	$(MAKE) update-icon-cache

uninstall-local:
	$(MAKE) uninstall-icons
	$(MAKE) update-icon-cache

# Local copy of the "installed" icon. This is used to be able to run empathy
# from source (see bgo #616159)
local_icons_dir = local-copy

# Generate the lists of destination paths
public_icons_local_install_paths = $(foreach obj, $(public_icons), $(local_icons_dir)/$(subst _,/,$(call reorder,_,1 3 2 4,$(obj))))
private_icons_local_install_paths = $(foreach obj, $(private_icons), $(local_icons_dir)/$(subst _,/,$(call reorder,_,1 3 2 4,$(obj))))

# For each destination path we'll need to compute back the original icon
# and create a rule that will ensure that the destination directory exists
# and then copy the original file
define icon_local_install_rule_template
$(local_icons_dir)/$$(subst _,/,$$(call reorder,_,1 3 2 4,$1)): $1
	$$(AM_V_GEN)mkdir -p $$(@D)
	$$(AM_V_GEN)cp $$^ $$@
endef

# Instantiate the template above for each icon
$(foreach icon,$(public_icons),$(eval $(call icon_local_install_rule_template,$(icon))))
$(foreach icon,$(private_icons),$(eval $(call icon_local_install_rule_template,$(icon))))

install-icons-src: $(public_icons_local_install_paths) $(private_icons_local_install_paths)

all: install-icons-src

clean-local:
	-rm -rf $(local_icons_dir)
