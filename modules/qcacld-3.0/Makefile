# Wrapper makefile for qca_cld3_wcn7750.ko.
#
# By default the module is NOT built against the tree kernel. The phone boots
# the Kono-Ha GKI kernel, and a module compiled against kernel/xiaomi/sm8735
# (6.6.82) loads but fails its driver probe (-EPERM, no wlan0). The module
# that actually works is the one built the way the konoha-kernel-gki
# build-wlan-injection workflow builds it:
#
#   - MiCode onyx-v-oss source stamped to 6.6.57, LOCALVERSION=-android15-8-4k
#   - gki_defconfig + sun_perf.config + onyx_perf.config, modules_prepare only
#   - Kono-Ha Module.symvers, and the workflow's KCFLAGS
#   - the same module args (profile, MODNAME, chip names, CONFIG_* set)
#
# konoha-abi-prep.sh (next to this file) prepares that ABI tree on demand and
# caches it in kernel/xiaomi/konoha-abi -- five levels up from this directory,
# i.e. outside every git project, so repo syncs and the apply.sh rsync overlay
# never disturb it. The build itself is out-of-tree from the ABI kernel's
# point of view but in-source from ours: object files land next to the sources
# in the overlaid tree copy (kernel/xiaomi/sm8735-modules/...), exactly like
# the konoha workflow's own build.
#
# Set KONOHA_ABI=0 to fall back to building against $(KERNEL_SRC) the way the
# stock MiCode wrapper does.

KONOHA_ABI ?= 1

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

# vendor/lineage/build/tasks/kernel.mk invokes this wrapper with $(M) relative
# to $(KERNEL_SRC); absolutize it once so every later use is unambiguous.
M ?= $(CURDIR)
M_ABS := $(shell if [ -n "$(KERNEL_SRC)" ]; then cd "$(KERNEL_SRC)"; fi; readlink -e "$(M)")

ifeq ($(KONOHA_ABI),1)

# kernel.mk builds external modules with O=<tree KERNEL_OBJ> and M=<relpath>,
# which kbuild resolves to <objtree>/<relpath> for outputs. Reproduce that
# mapping to find the tree-built platform / dataipa symvers.
EXT_ROOT := $(abspath $(M_ABS)/../../../..)
KERNEL_RPATH := $(shell python3 -c "import os,sys;print(os.path.relpath(*(sys.argv[1:])))" "$(EXT_ROOT)" "$(KERNEL_SRC)")
TREE_OUT := $(abspath $(O))
TREE_SYMVERS ?= $(TREE_OUT)/Module.symvers
PLATFORM_SYMVERS ?= $(abspath $(TREE_OUT)/$(KERNEL_RPATH)/qcom/opensource/wlan/platform/Module.symvers)
DATAIPA_SYMVERS ?= $(abspath $(TREE_OUT)/$(KERNEL_RPATH)/qcom/opensource/dataipa/drivers/platform/msm/Module.symvers)

# ABI kernel cache: kernel/xiaomi/konoha-abi (five levels above qcacld-3.0).
ABI_DIR ?= $(abspath $(M_ABS)/../../../../../konoha-abi)
ABI_SRC := $(ABI_DIR)/src
ABI_OUT := $(ABI_DIR)/out
PREP_SH := $(M_ABS)/konoha-abi-prep.sh
BUILT_RELEASE_STAMP := $(ABI_DIR)/.module-built-release

# KCFLAGS from tools/build_wlan_injection_module.sh in konoha-kernel-gki.
KONOHA_KCFLAGS := -w -mno-outline -D__ANDROID_COMMON_KERNEL__ \
	-DCONFIG_ARM_SMMU_MODULE=1 -DCONFIG_CFG80211_MODULE=1 \
	-DCONFIG_NL80211_TESTMODE=1 -DCONFIG_QCOM_IOMMU_UTIL_MODULE=1 \
	-DCONFIG_QCOM_MINIDUMP_MODULE=1 -DCONFIG_QCOM_VA_MINIDUMP_MODULE=1 \
	-DCONFIG_SCHED_WALT_MODULE=1

# Everything the workflow passes on the qcacld make line. The Kono-Ha
# Module.symvers only carries vmlinux exports -- cfg80211 and the other
# vendor-module symbols resolve from a filtered combination of the tree
# kernel / platform / dataipa symvers, i.e. with the CRCs of the modules this
# ROM actually ships in vendor_dlkm. modpost errors out on duplicate exports,
# so every symbol the Kono-Ha symvers already provides is dropped (and the
# remaining ones deduped) into $(KONOHA_EXTRA_SYMVERS).
KONOHA_EXTRA_SYMVERS := $(ABI_DIR)/extra.symvers
KONOHA_MODULE_ARGS := \
	ARCH=arm64 SUBARCH=arm64 \
	LD=ld.lld LLVM=1 LLVM_IAS=1 \
	CONFIG_CNSS_OUT_OF_TREE=y \
	KCFLAGS="$(KONOHA_KCFLAGS)" \
	KERNEL_SRC=$(ABI_SRC) \
	WLAN_ROOT=$(M_ABS) \
	WLAN_PROFILE=sun_gki_wcn7750 \
	MODNAME=qca_cld3_wcn7750 \
	CHIP_NAME=wcn7750 DEVNAME=wcn7750 WLAN_CTRL_NAME=wlan \
	QCA_WIFI_FTM_NL80211=y \
	CONFIG_QCA_WIFI_ISOC=0 CONFIG_QCA_WIFI_2_0=1 CONFIG_QCA_CLD_WLAN=m \
	CONFIG_ARM_SMMU=m CONFIG_CFG80211=m CONFIG_QCOM_IOMMU_UTIL=m \
	CONFIG_QCOM_VA_MINIDUMP=m CONFIG_SCHED_WALT=m \
	CONFIG_CNSS2_SSR_DRIVER_DUMP=y CONFIG_WLAN_FEATURE_WMI_DIAG_OVER_CE7=y \
	KBUILD_EXTRA_SYMBOLS="$(KONOHA_EXTRA_SYMVERS)"
ifneq ($(CC),)
KONOHA_MODULE_ARGS += CC="$(CC)"
endif

# Release of the tree kernel (from the inherited O=); used to relocate the
# installed module from the ABI release dir into the tree release dir, which
# is where kernel.mk's packaging `find` looks. Empty when run by hand.
TREE_RELEASE := $(shell cat $(TREE_OUT)/include/config/kernel.release 2>/dev/null)

.PHONY: all modules_install clean

# kernel.mk invokes this wrapper with no explicit target, so `all` must stay
# the default goal despite the rule below.

all: $(KONOHA_EXTRA_SYMVERS)
	$(call konoha-ensure-prepared)
	$(MAKE) -C $(ABI_SRC) O=$(ABI_OUT) OUT_DIR=$(ABI_OUT) M=$(M_ABS) \
		$(KONOHA_MODULE_ARGS) modules
	@cat "$(ABI_OUT)/include/config/kernel.release" > "$(BUILT_RELEASE_STAMP)"

# Symbols from the tree builds that the Kono-Ha kernel symvers doesn't already
# export, deduped by symbol name (first provider wins; the kernel's own
# symvers is consulted first by modpost either way).
#
# The prep runs in the recipe, not as a prerequisite: make evaluates the whole
# prerequisite graph before running any recipe, so a $(ABI_OUT)/Module.symvers
# prerequisite would abort with "No rule to make target" on a machine where
# the konoha-abi cache doesn't exist yet (it is only ever created by the prep).
$(KONOHA_EXTRA_SYMVERS): $(TREE_SYMVERS) $(PLATFORM_SYMVERS) $(DATAIPA_SYMVERS)
	$(call konoha-ensure-prepared)
	@mkdir -p "$(ABI_DIR)"
	@echo "[konoha-abi] generating $(KONOHA_EXTRA_SYMVERS)"
	@awk 'NR==FNR { k[$$2]=1; next } \
		!($$2 in k) && !($$2 in seen) { seen[$$2]=1; print }' \
		"$(ABI_OUT)/Module.symvers" "$(TREE_SYMVERS)" "$(PLATFORM_SYMVERS)" "$(DATAIPA_SYMVERS)" > "$@"

modules_install: $(KONOHA_EXTRA_SYMVERS)
	@if [ -z "$(INSTALL_MOD_PATH)" ]; then \
		echo "INSTALL_MOD_PATH is not set; refusing to install into /lib/modules" >&2; \
		exit 1; \
	fi
	$(call konoha-ensure-prepared)
	$(MAKE) -C $(ABI_SRC) O=$(ABI_OUT) OUT_DIR=$(ABI_OUT) M=$(M_ABS) \
		$(KONOHA_MODULE_ARGS) \
		INSTALL_MOD_PATH=$(INSTALL_MOD_PATH) INSTALL_MOD_STRIP=1 \
		KERNEL_UAPI_HEADERS_DIR=$(ABI_OUT) modules_install
	@if [ -n "$(TREE_RELEASE)" ] && [ -f "$(ABI_OUT)/include/config/kernel.release" ]; then \
		arel=$$(cat "$(ABI_OUT)/include/config/kernel.release"); \
		if [ "$$arel" != "$(TREE_RELEASE)" ]; then \
			adb="$(INSTALL_MOD_PATH)/lib/modules/$$arel"; \
			tdb="$(INSTALL_MOD_PATH)/lib/modules/$(TREE_RELEASE)"; \
			if [ -d "$$adb" ]; then \
				echo "[konoha-abi] relocating module install: $$arel -> $(TREE_RELEASE)"; \
				(cd "$$adb" && find . -type f -name '*.ko' | while read -r f; do \
					mkdir -p "$$tdb/$$(dirname "$$f")"; \
					mv "$$adb/$$f" "$$tdb/$$f"; \
				done); \
				rm -rf "$$adb"; \
			fi; \
		fi; \
	fi

clean:
	$(MAKE) -C $(ABI_SRC) O=$(ABI_OUT) OUT_DIR=$(ABI_OUT) M=$(M_ABS) \
		$(KONOHA_MODULE_ARGS) clean
	rm -f "$(BUILT_RELEASE_STAMP)"

# Make sure the ABI kernel exists and is up to date, and drop stale module
# objects when the ABI release changes (kbuild would happily relink .o files
# compiled against a different kernel).
define konoha-ensure-prepared
	@if [ ! -f "$(PREP_SH)" ]; then \
		echo "FATAL: $(PREP_SH) not found" >&2; exit 1; \
	fi
	@if [ ! -f "$(PLATFORM_SYMVERS)" ]; then \
		echo "FATAL: platform symvers missing: $(PLATFORM_SYMVERS)" >&2; \
		echo "       (wlan/platform must be built before qcacld-3.0)" >&2; exit 1; \
	fi
	@rel_prev=$$(cat "$(BUILT_RELEASE_STAMP)" 2>/dev/null); \
	KONOHA_ABI_DIR="$(ABI_DIR)" bash "$(PREP_SH)" --check-only || exit $$?; \
	rel_now=$$(cat "$(ABI_OUT)/include/config/kernel.release"); \
	if [ -n "$$rel_prev" ] && [ "$$rel_prev" != "$$rel_now" ]; then \
		echo "[konoha-abi] kernel release changed ($$rel_prev -> $$rel_now); cleaning module objects"; \
		$(MAKE) -C "$(ABI_SRC)" O="$(ABI_OUT)" OUT_DIR="$(ABI_OUT)" M="$(M_ABS)" \
			$(KONOHA_MODULE_ARGS) clean || exit $$?; \
	fi
endef

else # KONOHA_ABI=0: stock MiCode wrapper, builds against $(KERNEL_SRC)

# The Make variable $(M) must point to the directory that contains the module
# source code (which includes this Makefile). It can either be an absolute or
# a relative path. If it is a relative path, then it must be relative to the
# kernel source directory (KERNEL_SRC). An absolute path can be obtained very
# easily through $(shell pwd). Generating a path relative to KERNEL_SRC is
# difficult and we accept some outside help by letting the caller override the
# variable $(M). Allowing a relative path for $(M) enables us to have the build
# system put output/object files (.o, .ko.) into a directory different from the
# module source directory.

ifeq ($(WLAN_ROOT),)

ifeq ($(WLAN_ROOT_MODULE_SOURCE_PATH),)
# WLAN_ROOT must contain an absolute path (i.e. not a relative path)
KBUILD_OPTIONS := WLAN_ROOT=$(shell cd $(KERNEL_SRC); readlink -e $(M))
else
# Honor what is passed from Make variable $(M), which could be a relative path.
KBUILD_OPTIONS := WLAN_ROOT=$(M)
endif

# MODNAME should be qca_cld3_wlan for helium based wear target
ifeq (qca_cld3, $(WLAN_WEAR_CHIPSET))
KBUILD_OPTIONS += MODNAME?=$(WLAN_WEAR_CHIPSET)_wlan
else
KBUILD_OPTIONS += MODNAME?=wlan
endif

KBUILD_EXTRA := \
    CONFIG_IPA_OUT_OF_TREE=y \
    WLAN_PROFILE=sun_gki_wcn7750 \
    MODNAME=qca_cld3_wcn7750

KBUILD_OPTIONS += \
    KBUILD_EXTRA_SYMBOLS=$(M)/../../dataipa/drivers/platform/msm/Module.symvers \
    KBUILD_EXTRA_SYMBOLS+=$(M)/../platform/Module.symvers

#By default build for CLD
WLAN_SELECT := CONFIG_QCA_CLD_WLAN=m
KBUILD_OPTIONS += CONFIG_QCA_WIFI_ISOC=0
KBUILD_OPTIONS += CONFIG_QCA_WIFI_2_0=1
KBUILD_OPTIONS += $(WLAN_SELECT)
KBUILD_OPTIONS += $(KBUILD_EXTRA) # Extra config if any
endif

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean $(KBUILD_OPTIONS)

endif
