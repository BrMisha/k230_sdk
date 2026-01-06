################################################################################
#
# libdatachannel
#
################################################################################

LIBDATACHANNEL_VERSION = v0.23.2
LIBDATACHANNEL_SITE = https://github.com/paullouisageneau/libdatachannel.git
LIBDATACHANNEL_SITE_METHOD = git
LIBDATACHANNEL_GIT_SUBMODULES = YES
LIBDATACHANNEL_LICENSE = MPL-2.0
LIBDATACHANNEL_LICENSE_FILES = LICENSE
LIBDATACHANNEL_INSTALL_STAGING = YES
LIBDATACHANNEL_DEPENDENCIES = usrsctp libjuice openssl

LIBDATACHANNEL_CONF_OPTS = \
	-DNO_EXAMPLES=ON \
	-DNO_TESTS=ON \
	-DNO_WEBSOCKET=ON \
	-DNO_MEDIA=ON \
	-DUSE_SYSTEM_USRSCTP=ON \
	-DUSE_SYSTEM_JUICE=ON \
	-DUSE_NICE=OFF \
	-DUSE_GNUTLS=OFF \
	-DOPENSSL_USE_STATIC_LIBS=OFF

$(eval $(cmake-package))
