# RembrandtGPU: Radeon 680M (Rembrandt, 1002:1681) bring-up on macOS.
#
#   make hosttest   # src/common against a simulated chip, ASan/UBSan
#   make kext       # build/out/RembrandtProbe.kext (runs hosttest first)
#   make kext LEVEL=2   # same, probe level 2 (1 default, 2 + display, 3 + graphics core)
#   make kext SMU=1     # same, plus the SMU queries (src/common/rb_smu.h)
#   make tools      # build/out/rbdump
#   make plugin     # build/out/RembrandtGPU.kext, the Lilu plugin (src/plugin)
#
# Loading the kext is never done from here; see CLAUDE.md.

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

.PHONY: all hosttest kext tools plugin fbperf fbpresent iosurftex texprobe blitprobe xprobe screenprobe decodescreen permute bitfit surfdump surfwatch mtllinear mtlswizzle usageprobe cavecheck dispclk vramtest vramprobe lawtest noop ownership-test clean

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
	clang -dynamiclib -O2 -fno-objc-arc -o $@ $< \
	      -framework Metal -framework IOSurface -framework Foundation

# lawtest (2mn): the law in one fast instrument - six geometries, n=12, ~1.5 s.
lawtest: $(OUT)/lawtest
$(OUT)/lawtest: tools/lawtest.swift | $(OUT)
	swiftc -O $< -o $@ -framework Metal

# ownership-test (2mr): crashes if the interposers break Metal's +1 ownership.
# Run it before inserting the dylib into anything.
ownership-test: $(OUT)/iosurftex.dylib $(OUT)/ownership-test
	@echo "  control (no dylib):" && $(OUT)/ownership-test
	@echo "  with iosurftex:" && \
	  DYLD_INSERT_LIBRARIES=$(OUT)/iosurftex.dylib $(OUT)/ownership-test
	@echo "  with iosurftex disabled:" && \
	  DYLD_INSERT_LIBRARIES=$(OUT)/iosurftex.dylib RBGPU_IOSURFTEX=0 $(OUT)/ownership-test
$(OUT)/ownership-test: tools/ownership-test.m | $(OUT)
	clang -O1 -fobjc-arc -o $@ $< -framework Metal -framework Foundation

# noop-insert (2mr): an inserted dylib that does nothing, as the control for
# "does this app refuse ANY insertion, or only ours?"
noop: $(OUT)/noop-insert.dylib
$(OUT)/noop-insert.dylib: tools/noop-insert.c | $(OUT)
	clang -dynamiclib -O2 -o $@ $<

# blitprobe (2mx): is a GPU blit into a 64 KB-TILED PRIVATE texture correct?
# texprobe's private-blit paths only ever ran at sizes that land untiled.
blitprobe: $(OUT)/blitprobe
$(OUT)/blitprobe: tools/blitprobe.swift | $(OUT)
	swiftc -O $< -o $@ -framework Metal

# xprobe (2mx): cross-consumer reads - does a surface the GPU wrote read the
# same through a SECOND view of it? Every earlier probe used one view only.
xprobe: $(OUT)/xprobe
$(OUT)/xprobe: tools/xprobe.swift | $(OUT)
	swiftc -O $< -o $@ -framework Metal -framework IOSurface

# screenprobe (2mx): the same pattern on the real screen, CPU-drawn beside
# GPU-drawn. Run it with tools/screenprobe.sh from a Terminal that has Screen
# Recording permission.
screenprobe: $(OUT)/screenprobe
$(OUT)/screenprobe: tools/screenprobe.swift | $(OUT)
	swiftc -O $< -o $@ -framework Cocoa -framework Metal -framework QuartzCore

# decodescreen (2mx): read a screenprobe capture and say what happened to it.
decodescreen: $(OUT)/decodescreen
$(OUT)/decodescreen: tools/decodescreen.swift | $(OUT)
	swiftc -O $< -o $@

# vramprobe (2my): do two surfaces ever land on the same memory?
vramprobe: $(OUT)/vramprobe
$(OUT)/vramprobe: tools/vramprobe.swift | $(OUT)
	swiftc -O $< -o $@ -framework Metal

# permute (2mz): read the exact address permutation out of a scrambled window.
permute: $(OUT)/permute
$(OUT)/permute: tools/permute.swift | $(OUT)
	swiftc -O $< -o $@

# bitfit (2na): is the observed permutation an XOR-linear address map?
bitfit: $(OUT)/bitfit
$(OUT)/bitfit: tools/bitfit.swift | $(OUT)
	swiftc -O $< -o $@

# mtllinear (2nc): force DetermineSwizzleMode to LINEAR in ONE process, with
# no kext and no reboot. Injected per process (rule 21), never via launchd.
mtllinear: $(OUT)/mtllinear.dylib
$(OUT)/mtllinear.dylib: tools/mtllinear.c | $(OUT)
	clang -dynamiclib -O2 -Wall -o $@ $<

# mtlswizzle (2nq): a TRAMPOLINE hook on DetermineSwizzleMode, so the original
# can be called and the override made selective. mtllinear's constant stub
# crashed IntelliJ's Metal pipeline because it made render targets linear too.
mtlswizzle: $(OUT)/mtlswizzle.dylib
$(OUT)/mtlswizzle.dylib: tools/mtlswizzle.c | $(OUT)
	clang -dynamiclib -O2 -Wall -o $@ $<

# usageprobe (2nq): which descriptor field is Metal's `usage`?
usageprobe: $(OUT)/usageprobe
$(OUT)/usageprobe: tools/usageprobe.m | $(OUT)
	clang -O2 -Wall -o $@ $< -framework Metal -framework Foundation

# cavecheck (2oa): did Lilu's page patch actually land in the shared cache?
cavecheck: $(OUT)/cavecheck
$(OUT)/cavecheck: tools/cavecheck.m src/plugin/rbgpu_mtlcave.h | $(OUT)
	clang -O2 -Wall -o $@ $< -framework Metal -framework Foundation

# dispclk (2ot): the ORACLE for the vblank step - what frame clock does macOS
# actually have? Boot 133: 30.00 Hz against a 144 Hz panel. Run it before and
# after publishing a framebuffer with a real vblank.
dispclk: $(OUT)/dispclk
$(OUT)/dispclk: tools/dispclk.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework CoreGraphics \
	      -framework CoreVideo

# vramtest (2ot): the falsifier for "it is running out of the 7 MB VRAM".
# Allocates and reads back 1 GB plus an 8K texture. Run it when idle.
vramtest: $(OUT)/vramtest
$(OUT)/vramtest: tools/vramtest.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework Metal

# surfdump (2ni): what does a CoreAnimation backing store carry that a
# hand-allocated IOSurface does not?
surfdump: $(OUT)/surfdump
$(OUT)/surfdump: tools/surfdump.c | $(OUT)
	clang -O2 -Wall -o $@ $< -framework IOSurface -framework CoreFoundation

# surfwatch (2nj): watch IOSurfaceCreate in-process to see what CoreAnimation
# asks for when it makes a layer backing store.
surfwatch: $(OUT)/surfwatch.dylib
$(OUT)/surfwatch.dylib: tools/surfwatch.c | $(OUT)
	clang -dynamiclib -O2 -Wall -o $@ $< -framework IOSurface -framework CoreFoundation

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

# 2pb: where the display time actually goes. fbperf compares a GPU-side blit
# (the control) against the scanout read path; fbpresent measures the real
# full-screen present path, serialised and pipelined, with CPU-vs-wall so a
# copy can be told apart from a wait. Neither touches hardware.
fbperf:
	@clang -fobjc-arc -O2 -Wall -framework Foundation -framework Metal \
	    -framework CoreGraphics tools/fbperf.m -o $(OUT)/fbperf
	@echo "  OBJC $(OUT)/fbperf"

fbpresent:
	@clang -fobjc-arc -O2 -Wall -framework Cocoa -framework Metal \
	    -framework QuartzCore tools/fbpresent.m -o $(OUT)/fbpresent
	@echo "  OBJC $(OUT)/fbpresent"
