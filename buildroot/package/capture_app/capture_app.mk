CAPTURE_APP_VERSION = 1.0
CAPTURE_APP_SITE = $(TOPDIR)/../capture_app
CAPTURE_APP_SITE_METHOD = local

define CAPTURE_APP_BUILD_CMDS
    $(MAKE) -C $(@D)
endef

define CAPTURE_APP_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/10Hz $(TARGET_DIR)/usr/bin/10Hz
    $(INSTALL) -D -m 0755 $(@D)/1Hz $(TARGET_DIR)/usr/bin/1Hz
    $(INSTALL) -D -m 0755 $(@D)/10HzAdditional $(TARGET_DIR)/usr/bin/10HzAdditional
endef

$(eval $(generic-package))
