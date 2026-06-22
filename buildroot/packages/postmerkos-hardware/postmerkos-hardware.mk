################################################################################
# postmerkos-hardware
################################################################################
POSTMERKOS_HARDWARE_VERSION = 2.0
POSTMERKOS_HARDWARE_SITE = package/postmerkos-hardware
POSTMERKOS_HARDWARE_SITE_METHOD = local
POSTMERKOS_HARDWARE_DEPENDENCIES = configd fwupdate json-c

define POSTMERKOS_HARDWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/files/postmerkos-ledctl $(TARGET_DIR)/usr/sbin/postmerkos-ledctl
	$(INSTALL) -D -m 0755 $(@D)/files/postmerkos-hwprobe $(TARGET_DIR)/usr/sbin/postmerkos-hwprobe
	$(INSTALL) -D -m 0755 $(@D)/files/postmerkos-poe-prune $(TARGET_DIR)/usr/sbin/postmerkos-poe-prune
	$(INSTALL) -D -m 0755 $(@D)/files/S16postmerkos-hardware $(TARGET_DIR)/etc/init.d/S16postmerkos-hardware
	$(INSTALL) -D -m 0644 $(@D)/files/hardware-controls-default.conf $(TARGET_DIR)/usr/share/postmerkos/hardware-controls/default.conf
	mkdir -p $(TARGET_DIR)/usr/share/postmerkos/hardware-controls
	cp -a $(@D)/files/profiles/*.conf $(TARGET_DIR)/usr/share/postmerkos/hardware-controls/
endef
$(eval $(generic-package))
