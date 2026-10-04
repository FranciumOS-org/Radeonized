# RembrandtGPU: Radeon 680M (Rembrandt, 1002:1681) bring-up on macOS.
#
#   make hosttest   # src/common against a simulated chip, ASan/UBSan
#   make kext       # build/out/RembrandtProbe.kext (runs hosttest first)
#   make kext LEVEL=2   # same, probe level 2 (1 default, 2 + display, 3 + graphics core)
#   make tools      # build/out/rbdump
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
COMMON_SRCS := $(COMMON)/rb_discovery.c $(COMMON)/rb_probe.c
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

KEXT_OBJS := $(BUILD_DIR)/kext/rb_discovery.o $(BUILD_DIR)/kext/rb_probe.o \
             $(BUILD_DIR)/kext/RembrandtProbe.o $(BUILD_DIR)/kext/kmod_info.o
KEXT_BUNDLE := $(OUT)/RembrandtProbe.kext
KEXT_BIN    := $(KEXT_BUNDLE)/Contents/MacOS/RembrandtProbe

.PHONY: all hosttest kext tools clean

all: kext tools

hosttest: $(COMMON_SRCS) $(COMMON_HDRS) tools/hosttest/hosttest.c
	@mkdir -p $(OUT)
	@$(CC) $(HOST_CFLAGS) tools/hosttest/hosttest.c $(COMMON_SRCS) -o $(OUT)/hosttest
	@$(OUT)/hosttest $(wildcard tests/data/*.bin)

tools: $(OUT)/rbdump

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
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(KEXT_OBJS) -lkmod -o $(KEXT_BIN)
	@codesign --force --sign - $(KEXT_BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(KEXT_BUNDLE)"
	@python3 tools/check_imports.py $(KEXT_BUNDLE)

clean:
	rm -rf $(BUILD_DIR)

-include $(KEXT_OBJS:.o=.d)
