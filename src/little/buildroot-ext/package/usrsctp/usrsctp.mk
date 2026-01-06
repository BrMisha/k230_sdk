################################################################################
#
# usrsctp
#
################################################################################

USRSCTP_VERSION = 0.9.5.0
USRSCTP_SITE = $(call github,sctplab,usrsctp,$(USRSCTP_VERSION))
USRSCTP_LICENSE = BSD-3-Clause
USRSCTP_LICENSE_FILES = LICENSE.md
USRSCTP_INSTALL_STAGING = YES
USRSCTP_CONF_OPTS = \
	-Dsctp_build_programs=OFF \
	-Dsctp_debug=OFF

# Required for RISC-V glibc - BSD types and pthread extensions
USRSCTP_CONF_OPTS += -DCMAKE_C_FLAGS="-D_GNU_SOURCE"

$(eval $(cmake-package))
