PREFIX?=aarch64-w64-mingw32
BUILD_DIR?=build
LTO?=-flto

CC=$(PREFIX)-gcc
CXX=$(PREFIX)-g++
AR=$(PREFIX)-ar
LD=$(PREFIX)-ld
OBJCOPY=$(PREFIX)-objcopy
STRIP=$(PREFIX)-strip

INCLUDES=$(BUILD_DIR) include/winddk include
# SERIAL (2026-10-25): 0 = one EndFrame runs send -> fetch -> upload on the DDI
# thread (the P4-13 behaviour, default), 1 = the two-stage pipeline of
# 04-测试记录/10-流水线.md.  The pipeline source stays in the tree either way;
# this only selects which path DecoderEndFrame takes.  The runtime
# VIRTIO_WDDM_PIPELINE environment variable can still opt out of a 1 build, but
# it can no longer opt INTO a 0 build.
VIRTIO_WDDM_PIPELINE?=0
DEFINES=VK_USE_PLATFORM_WIN32_KHR VIRTIO_WDDM_PIPELINE=$(VIRTIO_WDDM_PIPELINE)
LIBS=version gdi32
CFLAGS=-std=gnu23 -O2 -fpic -ffunction-sections -fdata-sections -g -MMD -MP $(LTO) -ffile-prefix-map=$(PWD)=./
CXXFLAGS=-std=gnu++17
LDFLAGS=-g -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -Wl,--gc-sections
CFLAGS+=$(addprefix -I,$(INCLUDES)) $(addprefix -D,$(DEFINES))
LDFLAGS+=$(addprefix -l,$(LIBS))

#CXXFLAGS+=-pg -no-pie
#CFLAGS+=-pg -no-pie
#LDFLAGS+=-pg -Wl,--disable-dynamicbase

# Stolen from https://stackoverflow.com/questions/2483182/recursive-wildcards-in-gnu-make/18258352#18258352
rwildcard=$(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

DEPS := $(call rwildcard,$(BUILD_DIR),*.d)
ifneq ($(DEPS),)
include $(DEPS)
endif

include thirdparty/Makefile.dxvk
include thirdparty/Makefile.triton

UMD_NAME := dx11um_virtio

.DEFAULT_GOAL := $(BUILD_DIR)/dist/$(UMD_NAME).dll

.PHONY: format
format:
	clang-format -i *.cpp

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

DXVK_LIBS=$(BUILD_DIR)/dxvk-d3d11.a $(BUILD_DIR)/dxvk-dxgi.a $(BUILD_DIR)/dxvk.a

UMD_DLL_OBJS=$(addprefix $(BUILD_DIR)/,adapter.o d3d11_video.o d3d11_video_dxvk.o device.o dxgi.o resource.o vk_bootstrap.o dxvk.o vmedia_client.o)

# P2-2a: the virtio-media client is compiled into the UMD, not linked as
# vmedia_client.dll. VMEDIA_CLIENT_STATIC keeps its vm_* symbols out of the
# export table (which stays OpenAdapter10_2-only), avoids a runtime DLL
# dependency for every D3D11 process on the system, and makes the declarations
# in d3d11_video.c plain C instead of dllimport (there is no import library).
$(BUILD_DIR)/vmedia_client.o $(BUILD_DIR)/d3d11_video.o: CFLAGS += -DVMEDIA_CLIENT_STATIC

$(BUILD_DIR)/$(UMD_NAME).dll: $(UMD_DLL_OBJS) $(UMD_NAME).def $(DXVK_LIBS) $(BUILD_DIR)/triton.a | $(BUILD_DIR)
	$(CXX) $(CFLAGS) -shared -o $@ $(UMD_DLL_OBJS) $(DXVK_LIBS) $(BUILD_DIR)/triton.a -Wl,$(UMD_NAME).def $(LDFLAGS)

$(BUILD_DIR)/dist/$(UMD_NAME).dll $(BUILD_DIR)/dist/$(UMD_NAME).debug: $(BUILD_DIR)/$(UMD_NAME).dll | $(BUILD_DIR)/dist
	$(OBJCOPY) --only-keep-debug $< $(BUILD_DIR)/dist/$(UMD_NAME).debug
	cp $(BUILD_DIR)/$(UMD_NAME).dll $(BUILD_DIR)/dist/$(UMD_NAME).dll
	$(STRIP) --strip-debug --strip-unneeded $(BUILD_DIR)/dist/$(UMD_NAME).dll
	$(OBJCOPY) --add-gnu-debuglink=$(BUILD_DIR)/dist/$(UMD_NAME).debug $(BUILD_DIR)/dist/$(UMD_NAME).dll

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -c -o $@

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CXXFLAGS) $< -c -o $@

$(BUILD_DIR):
	mkdir -pv $@

$(BUILD_DIR)/dist: | $(BUILD_DIR)
	mkdir -pv $@
