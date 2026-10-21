# RembrandtGPU: Radeon 680M (Rembrandt, 1002:1681) bring-up on macOS.
#
#   make hosttest   # src/common against a simulated chip, ASan/UBSan
#   make kext       # build/out/RembrandtProbe.kext (runs hosttest first)
#   make kext LEVEL=2   # same, probe level 2 (1 default, 2 + display, 3 + graphics core)
#   make kext SMU=1     # same, plus the SMU queries (src/common/rb_smu.h)
#   make tools      # build/out/rbdump
#   make plugin     # build/out/RembrandtGPU.kext, the Lilu plugin (src/plugin)
#
# Loading the kext is never done from here; see docs/RULES.md.

PROJ_ROOT := .
MKSDK     := $(PROJ_ROOT)/third_party/MacKernelSDK
COMMON    := $(PROJ_ROOT)/src/common
KEXT_SRC  := $(PROJ_ROOT)/src/kext
BUILD_DIR := $(PROJ_ROOT)/build
OUT       := $(BUILD_DIR)/out

SDK   := $(shell xcrun --show-sdk-path 2>/dev/null)
ARCH  := -arch x86_64
MINOS := -mmacosx-version-min=13.0
CC    := xcrun clang
CXX   := xcrun clang++

# Freestanding C shared by the kext and the host tools.
COMMON_SRCS := $(COMMON)/rb_discovery.c $(COMMON)/rb_probe.c $(COMMON)/rb_smu.c $(COMMON)/rb_smu13.c
COMMON_HDRS := $(wildcard $(COMMON)/*.h)

KEXT_CFLAGS := $(ARCH) $(MINOS) -isysroot $(SDK) -nostdinc \
               -std=gnu11 -O2 -fno-strict-aliasing -fwrapv -mkernel \
               -fno-builtin -fno-common -fno-stack-protector -fno-omit-frame-pointer \
               -Wframe-larger-than=1024 \
               -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
               -I$(MKSDK)/Headers -I$(COMMON) -Wall -Wextra -Werror -MMD -MP
KEXT_CXXFLAGS := $(ARCH) $(MINOS) -isysroot $(SDK) -nostdinc \
               -std=gnu++17 -O2 -fno-strict-aliasing -mkernel -fapple-kext -fno-rtti \
               -fno-exceptions -fno-builtin -fno-common -fno-stack-protector \
               -Wframe-larger-than=1024 \
               -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
               -I$(MKSDK)/Headers -I$(COMMON) -Wall -Werror -MMD -MP

HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMMON) \
               -fsanitize=address,undefined -fno-sanitize-recover=all

KEXT_OBJS := $(BUILD_DIR)/kext/rb_discovery.o $(BUILD_DIR)/kext/rb_probe.o $(BUILD_DIR)/kext/rb_smu.o \
             $(BUILD_DIR)/kext/RembrandtProbe.o $(BUILD_DIR)/kext/kmod_info.o
KEXT_BUNDLE := $(OUT)/RembrandtProbe.kext
KEXT_BIN    := $(KEXT_BUNDLE)/Contents/MacOS/RembrandtProbe

.PHONY: all hosttest kext tools plugin iosurftex texprobe lawtest noop clean

all: kext tools

# rb_smu13_test.c checks rb_smu13's table layouts against Linux's header in
# docs/linux-ref. rb_smu13.o (kernel flags) is built only to prove it compiles
# freestanding; nothing links it yet.
HOSTTEST_SRCS := tools/hosttest/hosttest.c tools/hosttest/smu13_test.c

hosttest: $(COMMON_SRCS) $(COMMON_HDRS) $(HOSTTEST_SRCS) $(BUILD_DIR)/kext/rb_smu13.o
	@mkdir -p $(OUT)
	@$(CC) $(HOST_CFLAGS) -Idocs/linux-ref $(HOSTTEST_SRCS) $(COMMON_SRCS) -o $(OUT)/hosttest
	@$(OUT)/hosttest $(wildcard tests/data/*.bin)

tools: $(OUT)/rbdump

# iosurftex (2mo/2mp): backs the textures Apple's driver lays out wrongly with
# IOSurfaces, in the client process. Not part of `all` - it is a userspace
# workaround, loaded per process with DYLD_INSERT_LIBRARIES, and needs no kext.
iosurftex: $(OUT)/iosurftex.dylib
$(OUT)/iosurftex.dylib: tools/iosurftex.m | $(OUT)
	clang -dynamiclib -O2 -fobjc-arc -o $@ $< \
	      -framework Metal -framework IOSurface -framework Foundation

# lawtest (2mn): the law in one fast instrument - six geometries, n=12, ~1.5 s.
lawtest: $(OUT)/lawtest
$(OUT)/lawtest: tools/lawtest.swift | $(OUT)
	swiftc -O $< -o $@ -framework Metal

# noop-insert (2mr): an inserted dylib that does nothing, as the control for
# "does this app refuse ANY insertion, or only ours?"
noop: $(OUT)/noop-insert.dylib
$(OUT)/noop-insert.dylib: tools/noop-insert.c | $(OUT)
	clang -dynamiclib -O2 -o $@ $<

# texprobe: the userspace texture-upload probe the text work is measured with.
texprobe: $(OUT)/texprobe
$(OUT)/texprobe: tools/texprobe.swift | $(OUT)
	swiftc -O $< -o $@ -framework Metal -framework IOSurface

$(OUT)/rbdump: tools/rbdump.c $(COMMON)/rb_discovery.c $(COMMON_HDRS)
	@mkdir -p $(OUT)
	@$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I$(COMMON) tools/rbdump.c $(COMMON)/rb_discovery.c -o $@
	@echo "  TOOL $@"

$(BUILD_DIR)/kext/%.o: $(COMMON)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC   common/$*.c"
	@$(CC) $(KEXT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kext/%.o: $(KEXT_SRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  kext/$*.cpp"
	@$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/kext/%.o: $(KEXT_SRC)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC   kext/$*.c"
	@$(CC) $(KEXT_CFLAGS) -c $< -o $@

kext: hosttest $(KEXT_OBJS) $(KEXT_SRC)/Info.plist
	@rm -rf $(KEXT_BUNDLE)
	@mkdir -p $(dir $(KEXT_BIN))
	@cp $(KEXT_SRC)/Info.plist $(KEXT_BUNDLE)/Contents/Info.plist
	@if [ -n "$(LEVEL)" ]; then plutil -replace IOKitPersonalities.RembrandtProbe.RBProbeLevel \
	    -integer $(LEVEL) $(KEXT_BUNDLE)/Contents/Info.plist && echo "  LEVEL $(LEVEL)"; fi
	@if [ "$(SMU)" = 1 ]; then plutil -replace IOKitPersonalities.RembrandtProbe.RBSMUQuery \
	    -bool true $(KEXT_BUNDLE)/Contents/Info.plist && echo "  SMU queries on"; fi
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(KEXT_OBJS) -lkmod -o $(KEXT_BIN)
	@codesign --force --sign - $(KEXT_BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(KEXT_BUNDLE)"
	@python3 tools/check_imports.py $(KEXT_BUNDLE)

# The Lilu plugin. Built against Lilu's source (third_party/Lilu, the version
# loaded on this machine) the way Lilu's own plugins are: plugin_start.cpp
# supplies the IOService class and the kern_start/kern_stop entry points.
LILU        := $(PROJ_ROOT)/third_party/Lilu/Lilu
PLUGIN_SRC  := $(PROJ_ROOT)/src/plugin
PLUGIN_VER  := 1.0.0
PLUGIN_DEFS := -DPRODUCT_NAME=RembrandtGPU -DMODULE_VERSION=$(PLUGIN_VER) -DPLUGIN_VERSION='"$(PLUGIN_VER)"'
PLUGIN_OBJS := $(BUILD_DIR)/plugin/kern_start.o $(BUILD_DIR)/plugin/RembrandtGPUPeer.o \
               $(BUILD_DIR)/plugin/plugin_start.o $(BUILD_DIR)/plugin/kmod_info.o \
               $(BUILD_DIR)/plugin/rb_smu13.o $(BUILD_DIR)/plugin/rb_discovery.o $(BUILD_DIR)/plugin/fw_yellow_carp.o $(BUILD_DIR)/plugin/fw_yellow_carp_rlc.o
VBIOS_ROM   := firmware/680m-vbios.rom
VBIOS_HDR   := $(BUILD_DIR)/plugin/gen/vbios_data.h
PLUGIN_BUNDLE := $(OUT)/RembrandtGPU.kext
PLUGIN_BIN    := $(PLUGIN_BUNDLE)/Contents/MacOS/RembrandtGPU
PLUGIN_CXXFLAGS := $(filter-out -Werror -I$(COMMON),$(KEXT_CXXFLAGS)) -isystem $(LILU) $(PLUGIN_DEFS) -I$(BUILD_DIR)/plugin/gen -I$(COMMON)

$(BUILD_DIR)/plugin/%.o: $(PLUGIN_SRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  plugin/$*.cpp"
	@$(CXX) $(PLUGIN_CXXFLAGS) -Werror -c $< -o $@

# The VBIOS (B0) goes in as a generated header; firmware/ and build/ are not committed.
$(VBIOS_HDR): $(VBIOS_ROM) tools/vbios_header.py
	@python3 tools/vbios_header.py $< $@

$(BUILD_DIR)/plugin/RembrandtGPUPeer.o: $(VBIOS_HDR)

$(BUILD_DIR)/plugin/plugin_start.o: $(LILU)/Library/plugin_start.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  Lilu/Library/plugin_start.cpp"
	@$(CXX) $(PLUGIN_CXXFLAGS) -c $< -o $@

# The shared, host-tested libraries the plugin uses (src/common).
$(BUILD_DIR)/plugin/%.o: $(COMMON)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC   common/$*.c (plugin)"
	@$(CC) $(KEXT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/plugin/kmod_info.o: $(PLUGIN_SRC)/kmod_info.c
	@mkdir -p $(dir $@)
	@$(CC) $(filter-out -I$(COMMON),$(KEXT_CFLAGS)) $(PLUGIN_DEFS) -c $< -o $@

plugin: hosttest $(PLUGIN_OBJS) $(PLUGIN_SRC)/Info.plist
	@rm -rf $(PLUGIN_BUNDLE)
	@mkdir -p $(dir $(PLUGIN_BIN))
	@sed -e 's/$$(EXECUTABLE_NAME)/RembrandtGPU/g' -e 's/$$(PRODUCT_NAME)/RembrandtGPU/g' \
	    -e 's/$$(PRODUCT_BUNDLE_IDENTIFIER)/com.rembrandtgpu.plugin/g' \
	    -e 's/$$(MODULE_VERSION)/$(PLUGIN_VER)/g' $(PLUGIN_SRC)/Info.plist > $(PLUGIN_BUNDLE)/Contents/Info.plist
	@plutil -lint -s $(PLUGIN_BUNDLE)/Contents/Info.plist
	@! grep -n '\$$(' $(PLUGIN_BUNDLE)/Contents/Info.plist
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(PLUGIN_OBJS) -lkmod -o $(PLUGIN_BIN)
	@codesign --force --sign - $(PLUGIN_BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(PLUGIN_BUNDLE)"

clean:
	rm -rf $(BUILD_DIR)

-include $(KEXT_OBJS:.o=.d) $(PLUGIN_OBJS:.o=.d)
