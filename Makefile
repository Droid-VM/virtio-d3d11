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
DEFINES=VK_USE_PLATFORM_WIN32_KHR
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

UMD_DLL_OBJS=$(addprefix $(BUILD_DIR)/,adapter.o device.o dxgi.o resource.o dxvk.o)

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
