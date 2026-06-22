################################################################################
#
# postmerkos-console
#
################################################################################

POSTMERKOS_CONSOLE_VERSION = 1.1
POSTMERKOS_CONSOLE_SITE = package/postmerkos-console
POSTMERKOS_CONSOLE_SITE_METHOD = local
POSTMERKOS_CONSOLE_INSTALL_TARGET = YES
POSTMERKOS_CONSOLE_DEPENDENCIES = configd fwupdate

define POSTMERKOS_CONSOLE_INSTALL_TARGET_CMDS
	rm -f $(TARGET_DIR)/etc/profile.d/50-postmerkos-cli.sh \
		$(TARGET_DIR)/usr/bin/postmerkos-backup-crypt
	$(INSTALL) -D -m 0755 $(@D)/files/postmerkos-console \
		$(TARGET_DIR)/usr/bin/postmerkos-console
	$(INSTALL) -D -m 0755 $(@D)/files/postmerkos-serial-login \
		$(TARGET_DIR)/usr/sbin/postmerkos-serial-login
	ln -snf postmerkos-console $(TARGET_DIR)/usr/bin/postmerkos-cli
	ln -snf postmerkos-console $(TARGET_DIR)/usr/bin/pmc
	$(INSTALL) -D -m 0644 $(@D)/files/50-postmerkos-console.sh \
		$(TARGET_DIR)/etc/profile.d/50-postmerkos-console.sh
endef

$(eval $(generic-package))
