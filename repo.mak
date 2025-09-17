RT-SMART_VERSION        = "4096e1730f5e1b8c530a1c4fe05a90d09b2b0782"
MPP_VERSION             = "869e5a1ca170f658d8516122e4d95b8594265418"
UNITTEST_VERSION        = "a07caf5e19a6a215605271cfeedce18111790e9d"
OPENSBI_VERSION         = "1f89326f7dc48d701dbed79f828b80fa28e5b34c"
BUILDROOT-EXT_VERSION   = "3cef3f973d261f96a6c8f7ccb3bc140b972e1f1c"
LINUX_VERSION           = "33d1e773a745e533637447812c9aad924bbea25b"
CDK_VERSION             = "193bd614c422065327ce4d68776e37bef94d19bf"
UBOOT_VERSION           = "9af86a1892e01bc329329a118f322fe77f05c9f7"

RT-SMART_SRC_PATH        = src/big/rt-smart
MPP_SRC_PATH             = src/big/mpp
UNITTEST_SRC_PATH        = src/big/unittest
OPENSBI_SRC_PATH         = src/common/opensbi
BUILDROOT-EXT_SRC_PATH   = src/little/buildroot-ext
LINUX_SRC_PATH           = src/little/linux
UBOOT_SRC_PATH           = src/little/uboot
CDK_SRC_PATH             = src/common/cdk

RT-SMARTR_URL       = "git@g.a-bug.org:maix_sw/maix3_rt_smart.git"
MPP_URL             = "git@g.a-bug.org:maix_sw/mpp.git"
UNITTEST_URL        = "git@g.a-bug.org:maix_sw/maix3_unittest.git"
OPENSBI_URL         = "git@g.a-bug.org:maix_sw/maix3_opensbi.git"
BUILDROOT-EXT_URL   = "git@g.a-bug.org:maix_sw/buildroot-ext.git"
LINUX_URL           = "git@g.a-bug.org:maix_sw/maix3_linux_kernel.git"
UBOOT_URL           = "git@g.a-bug.org:maix_sw/maix3_u-boot.git"
CDK_URL             = "git@g.a-bug.org:maix_sw/cdk.git"

define FETCH_CODE
	set -e; \
	(test -d $(2) || ( \
		echo "clone $(1) into $(2), version $(3)";\
		git clone $(1) $(2) >/dev/null 2>&1;\
		cd $(2);\
		git checkout $(3) >/dev/null 2>&1;\
		git submodule update --init;\
	)) && ( \
		echo "fetch $(1), version $(3)";\
		cd $(2);\
		git fetch;\
		git submodule update;\
		git checkout $(3) >/dev/null 2>&1;\
	); \
	cd - >/dev/null 2>&1;\
	echo "success"
endef
