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

.PHONY: all hosttest kext fbkext tools plugin fbperf fbpresent iosurftex texprobe blitprobe xprobe screenprobe decodescreen permute bitfit surfdump surfwatch mtllinear mtlswizzle usageprobe cavecheck dispclk vramtest accelport slquery skyshaders cdchain useiop ioptest agdcinfo vramprobe lawtest noop ownership-test clean

all: kext tools

# rb_smu13_test.c checks rb_smu13's table layouts against Linux's header in
# docs/linux-ref. rb_smu13.o (kernel flags) is built only to prove it compiles
# freestanding; nothing links it yet.
HOSTTEST_SRCS := tools/hosttest/hosttest.c tools/hosttest/smu13_test.c tools/hosttest/agdc_test.c tools/hosttest/otg_test.c tools/hosttest/hubp_test.c

hosttest: $(COMMON_SRCS) $(COMMON_HDRS) $(HOSTTEST_SRCS) $(BUILD_DIR)/kext/rb_smu13.o tools/hosttest/agdc_lookup_test.cpp src/plugin/rbgpu_agdc_lookup.hpp tools/hosttest/otg_kernel_test.cpp tools/hosttest/otg_kernel_shim.hpp src/plugin/rbgpu_agdc_otg.cpp
hosttest: tools/hosttest/fb_vbl_test.cpp tools/hosttest/fb_vbl_shim.hpp src/fbkext/RBAMDFramebufferVBL.cpp
	@mkdir -p $(OUT)
	@$(CC) $(HOST_CFLAGS) -Idocs/linux-ref $(HOSTTEST_SRCS) $(COMMON_SRCS) -o $(OUT)/hosttest
	@$(OUT)/hosttest $(wildcard tests/data/*.bin)
	@$(CXX) $(filter-out -std=gnu11,$(HOST_CFLAGS)) -std=c++17 -Isrc/plugin tools/hosttest/agdc_lookup_test.cpp -o $(OUT)/agdc_lookup_test
	@$(OUT)/agdc_lookup_test
	@$(CXX) $(filter-out -std=gnu11,$(HOST_CFLAGS)) -std=c++17 -DRBGPU_OTG_HOSTTEST -Itools/hosttest tools/hosttest/otg_kernel_test.cpp src/plugin/rbgpu_agdc_otg.cpp -o $(OUT)/otg_kernel_test
	@$(OUT)/otg_kernel_test
	@$(CXX) $(filter-out -std=gnu11,$(HOST_CFLAGS)) -std=c++17 -DRBGPU_FB_VBL_HOSTTEST -Itools/hosttest tools/hosttest/fb_vbl_test.cpp src/fbkext/RBAMDFramebufferVBL.cpp -o $(OUT)/fb_vbl_test
	@$(OUT)/fb_vbl_test

tools: $(OUT)/rbdump

.PHONY: fbvbl
fbvbl: $(OUT)/fbvbl
$(OUT)/fbvbl: tools/fbvbl.c | $(OUT)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@ -framework IOKit -framework CoreFoundation

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

# 2r9: observe the backing resource independently of the Metal texture layout.
.PHONY: resourcewatch texturepath
resourcewatch: $(OUT)/resourcewatch.dylib
$(OUT)/resourcewatch.dylib: tools/resourcewatch.c | $(OUT)
	clang -dynamiclib -O2 -Wall -Wextra -Werror -undefined dynamic_lookup -o $@ $<

texturepath: $(OUT)/texturepath
$(OUT)/texturepath: tools/texturepath.m | $(OUT)
	clang -O2 -Wall -Wextra -Werror -o $@ $< -framework Metal -framework Foundation -framework IOSurface

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

# accelport (2py): the ORACLE for "why software compositing?" - reproduces
# CoreDisplay::FindAcceleratorWithMTLDevice(), whose return IS
# mpPriv->accelerator, which SkyLight routes to CompositorSW when it is 0.
# Prints its own precondition: an unarmed boot has no accelerator and every
# number is 0 for an unrelated reason.
metal_probe: $(OUT)/metal_probe
$(OUT)/metal_probe: tools/metal_probe.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework Metal

accelport: $(OUT)/accelport
$(OUT)/accelport: tools/accelport.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework Metal \
	      -framework IOKit

# skyshaders (2qb): the ORACLE for 2qa's survivor - can this GPU build the
# things CompositorMetal::new_compositor needs?  Public Metal APIs only, on
# the selectors and CFStrings decoded from that function's own slide-info
# slots.  A failure is the answer; a success does NOT clear WindowServer.
skyshaders: $(OUT)/skyshaders
$(OUT)/skyshaders: tools/skyshaders.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework Metal \
	      -framework IOKit

# useiop (2ql): the ORACLE for the gate that picks between CoreDisplay's two
# surface worlds.  Display::GetNextSurface calls UseIOPresentment() and falls
# back to the legacy CGXAcquireDisplayDeviceSurface path - the one 2qk proved
# dead - whenever it is false.  Reproduces that function exactly, including
# the IOObjectCopyClass vendor classifier, and calls Apple's own if it is
# exported so the two can be compared (rule 14).
useiop: $(OUT)/useiop
$(OUT)/useiop: tools/useiop.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework IOKit

# ioptest (2qm): the de-risking experiment for 2ql's lever - can an
# IOPresentment display pipe be created for OUR IONDRVFramebuffer and OUR AMD
# accelerator?  Arity of CreateForRegistryService was read from its
# disassembly.  A NULL is INCONCLUSIVE (entitlements), pre-stated in the tool.
ioptest: $(OUT)/ioptest
$(OUT)/ioptest: tools/ioptest.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework IOKit

# agdcinfo (2r5): the ORACLE for the last link in the display chain - it
# reproduces ioPresentmentFindAGDCMatch's own query byte for byte, including
# the exact 17-byte "AppleGPUWrangler" name test that decides whether the
# association map gets any entries at all, and it checks the measured
# 0x10000003 against IOServiceOpen on a null port.  Needs no root for the two
# questions that matter; selector 1 on the wrangler may answer NotPermitted
# unprivileged (2qm), which is pre-stated in the tool as INCONCLUSIVE.
agdcinfo: $(OUT)/agdcinfo
$(OUT)/agdcinfo: tools/agdcinfo.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation -framework IOKit

# cdchain (2qc): CALL the ops-table thunks the compositor decision calls,
# rather than reading a field out of a state dump. Arity of each was read
# from its disassembly first. An empty device list in a client process is the
# expected outcome and is reported as INCONCLUSIVE.
cdchain: $(OUT)/cdchain
$(OUT)/cdchain: tools/cdchain.m | $(OUT)
	clang -fobjc-arc -O2 -o $@ $< -framework Foundation

# slquery (2pz): what SkyLight itself says about THIS display - whether its
# accelerator id resolves and whether it hands back a Metal device. Arity of
# each export was read from its disassembly before it was called.
slquery: $(OUT)/slquery
$(OUT)/slquery: tools/slquery.m | $(OUT)
	clang -O2 -o $@ $< -framework Metal -framework ApplicationServices

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

# The framebuffer kext (src/fbkext): an IOFramebuffer subclass whose CLASS NAME
# carries "AMD", which is the one thing that opens CoreDisplay's IOPresentment
# gate (2ql). It is a SEPARATE kext from the Lilu plugin on purpose: the plugin
# is injected by OpenCore into BootKernelExtensions.kc, where a dependency on
# IOGraphicsFamily cannot bind (the 2oz trap), while this one is loaded at
# runtime or from the auxiliary collection, where it can (2sl, measured with
# kmutil libraries). So `make kext` must NOT build it into the plugin and
# check_imports.py's 0-from-SystemKernelExtensions rule does NOT apply here -
# this kext is REQUIRED to resolve against the system collection.
FBKEXT_SRC    := $(PROJ_ROOT)/src/fbkext
FBKEXT_OBJS   := $(BUILD_DIR)/fbkext/RBAMDFramebuffer.o $(BUILD_DIR)/fbkext/RBAMDFramebufferVBL.o $(BUILD_DIR)/fbkext/RBAMDFramebufferHUBP.o $(BUILD_DIR)/fbkext/kmod_info.o
FBKEXT_BUNDLE := $(OUT)/RBAMDFramebuffer.kext
FBKEXT_BIN    := $(FBKEXT_BUNDLE)/Contents/MacOS/RBAMDFramebuffer

$(BUILD_DIR)/fbkext/%.o: $(FBKEXT_SRC)/%.cpp $(FBKEXT_SRC)/RBAMDFramebuffer.hpp
	@mkdir -p $(dir $@)
	@echo "  CXX  fbkext/$*.cpp"
	@$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/fbkext/%.o: $(FBKEXT_SRC)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC   fbkext/$*.c"
	@$(CC) $(KEXT_CFLAGS) -c $< -o $@

fbkext: hosttest $(FBKEXT_OBJS) $(FBKEXT_SRC)/Info.plist
	@rm -rf $(FBKEXT_BUNDLE)
	@mkdir -p $(dir $(FBKEXT_BIN))
	@cp $(FBKEXT_SRC)/Info.plist $(FBKEXT_BUNDLE)/Contents/Info.plist
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(FBKEXT_OBJS) -lkmod -o $(FBKEXT_BIN)
	@codesign --force --sign - $(FBKEXT_BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(FBKEXT_BUNDLE)"
	@# Every import must resolve, and the IOGraphicsFamily ones must come from
	@# the SYSTEM collection - the opposite of the plugin's requirement. Fail
	@# the build if IOFramebuffer does not resolve, so a broken dependency is
	@# never discovered on hardware.
	@kmutil libraries -p $(FBKEXT_BUNDLE) > $(BUILD_DIR)/fbkext/libraries.txt 2>&1 || \
	    { echo "  FAIL kmutil libraries refused the bundle:"; cat $(BUILD_DIR)/fbkext/libraries.txt; exit 1; }
	@grep -q 'IOFramebuffer.*SystemKernelExtensions.kc.*IOGraphicsFamily' \
	    $(BUILD_DIR)/fbkext/libraries.txt || \
	    { echo "  FAIL no IOFramebuffer symbol resolved from SystemKernelExtensions.kc"; \
	      cat $(BUILD_DIR)/fbkext/libraries.txt; exit 1; }
	@echo "  LINK $$(grep -c SystemKernelExtensions.kc $(BUILD_DIR)/fbkext/libraries.txt) symbols from the system collection, $$(grep -c BootKernelExtensions.kc $(BUILD_DIR)/fbkext/libraries.txt) from boot"

# The Lilu plugin. Built against Lilu's source (third_party/Lilu, the version
# loaded on this machine) the way Lilu's own plugins are: plugin_start.cpp
# supplies the IOService class and the kern_start/kern_stop entry points.
LILU        := $(PROJ_ROOT)/third_party/Lilu/Lilu
PLUGIN_SRC  := $(PROJ_ROOT)/src/plugin
PLUGIN_VER  := 1.0.0
PLUGIN_DEFS := -DPRODUCT_NAME=RembrandtGPU -DMODULE_VERSION=$(PLUGIN_VER) -DPLUGIN_VERSION='"$(PLUGIN_VER)"'
PLUGIN_OBJS := $(BUILD_DIR)/plugin/kern_start.o $(BUILD_DIR)/plugin/RembrandtGPUPeer.o \
               $(BUILD_DIR)/plugin/rbgpu_agdc.o $(BUILD_DIR)/plugin/rbgpu_agdc_otg.o \
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

# dstream (2xb): the BRIDGE. 2xa measured that an active display stream takes
# RBFBAmtrSets from 0.05-0.08 to 71.30 sets/s, reversibly, with two controls -
# the same rate a connected AnyDesk session gives. This holds such a stream
# open and discards every frame.
#
# CGDisplayStream is marked `unavailable: obsoleted in macOS 15.0` by the SDK,
# so the calls will not compile at all and -Wno-deprecated-declarations does
# not help - but every symbol RESOLVES at runtime here (checked with
# CGMainDisplayID as a positive control and a made-up name as a negative one),
# and AnyDesk requires the same ones. So dstream.m binds them with dlsym.
#
# The .app bundle exists only for TCC: Screen Recording is keyed to a code
# identity, and a bare CLI binary inherits whichever terminal launched it. The
# bundle is ad-hoc signed with a FIXED identifier so the grant survives a
# rebuild of the executable inside it. **A rebuild still rewrites the bundle,
# so re-sign is part of the target, not a manual step** (2x5 lost framepump's
# grant to exactly this).
.PHONY: dstream
dstream: $(OUT)/RBDisplayStream.app
$(OUT)/dstream: tools/dstream.m | $(OUT)
	clang -O2 -Wall -Wextra -Wno-deprecated-declarations -fobjc-arc -o $@ $< \
	      -framework CoreGraphics -framework CoreFoundation -framework IOSurface \
	      -framework Foundation -framework ScreenCaptureKit -framework CoreMedia
$(OUT)/RBDisplayStream.app: $(OUT)/dstream tools/dstream-Info.plist | $(OUT)
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	cp $(OUT)/dstream $@/Contents/MacOS/RBDisplayStream
	cp tools/dstream-Info.plist $@/Contents/Info.plist
	codesign --force --sign - --identifier com.rembrandtgpu.displaystream $@
	@codesign -dv $@ 2>&1 | grep -E 'Identifier|Signature' | sed 's/^/  /'
