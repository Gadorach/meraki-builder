################################################################################
#
# configd
#
################################################################################

CONFIGD_VERSION = 0.2
CONFIGD_SITE = package/configd
CONFIGD_SITE_METHOD = local
CONFIGD_INSTALL_TARGET = YES
CONFIGD_DEPENDENCIES = json-c pd690xx
CONFIGD_WEBSOCKET = 0

ifeq ($(BR2_PACKAGE_CONFIGD_WEBSOCKET),y)
CONFIGD_DEPENDENCIES += libwebsockets
CONFIGD_WEBSOCKET = 1
endif

define CONFIGD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CPPFLAGS="$(TARGET_CPPFLAGS)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		ENABLE_WEBSOCKET="$(CONFIGD_WEBSOCKET)" \
		PD690XX_DIR="$(TOPDIR)/package/pd690xx" \
		POSTMERKOS_DIR="$(TOPDIR)/package/postmerkos"
endef

define CONFIGD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/configd $(TARGET_DIR)/bin/configd
	$(INSTALL) -D -m 0755 $(@D)/postmerkosctl $(TARGET_DIR)/usr/bin/postmerkosctl
endef

$(eval $(generic-package))
