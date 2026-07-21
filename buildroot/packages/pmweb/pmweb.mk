################################################################################
#
# pmweb
#
################################################################################

PMWEB_VERSION = 0.1
PMWEB_SITE = package/pmweb
PMWEB_SITE_METHOD = local
PMWEB_INSTALL_TARGET = YES
PMWEB_DEPENDENCIES = mongoose mbedtls

define PMWEB_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu11 \
		-o $(@D)/pmweb $(@D)/pmweb.c $(TARGET_LDFLAGS) \
		-lmongoose -lmbedtls -lmbedx509 -lmbedcrypto
endef

define PMWEB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pmweb $(TARGET_DIR)/usr/sbin/pmweb
endef

$(eval $(generic-package))
