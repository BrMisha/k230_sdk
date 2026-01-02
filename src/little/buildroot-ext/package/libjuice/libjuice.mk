################################################################################
#
# libjuice
#
################################################################################

LIBJUICE_VERSION = v1.7.0
LIBJUICE_SITE = $(call github,paullouisageneau,libjuice,$(LIBJUICE_VERSION))
LIBJUICE_LICENSE = MPL-2.0
LIBJUICE_LICENSE_FILES = LICENSE
LIBJUICE_INSTALL_STAGING = YES
LIBJUICE_CONF_OPTS = \
	-DNO_TESTS=ON \
	-DJUICE_BUILD_SHARED=ON \
	-DJUICE_BUILD_STATIC=ON

$(eval $(cmake-package))
