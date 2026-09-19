# the-black-hole Makefile
# Builds a Windows screensaver (.scr) that renders a Gargantua-style black hole with Vulkan.
# Modelled on Nuke-Saver's build: MinGW-w64 (MSYS2 UCRT64), glslc from shaderc, xxd, windres.
#
# Targets:
#   make            build the-black-hole.scr
#   make clean      remove everything generated
#   make install    install for the current user and select it as the screen saver

CXX      = g++
CC       = gcc
WINDRES  = windres
GLSLC    = glslc
XXD      = xxd

TARGET   = the-black-hole.scr
RES      = $(BUILD)/the-black-hole.res

BUILD    = build
OBJDIR   = $(BUILD)/obj
SPVDIR   = $(BUILD)/spv
GENDIR   = $(BUILD)/gen
BUILDTMP = $(BUILD)/tmp

# Keep compiler/linker temp files inside the project; don't depend on TMP/TEMP.
export TMP    := $(abspath $(BUILDTMP))
export TEMP   := $(TMP)
export TMPDIR := $(TMP)

# VK_NO_PROTOTYPES: nothing static-imports vulkan-1.dll; every entry point comes from volk at
# runtime, so a machine without a Vulkan driver still starts the saver (it just paints black).
# VK_USE_PLATFORM_WIN32_KHR exposes the Win32 surface entry points through volk.
CPPFLAGS = -Isrc -Ithird_party/volk -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_WIN32_KHR \
           -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN
# -MMD -MP: header dependencies, so a changed header rebuilds everything that reads it.
DEPFLAGS = -MMD -MP
WARN     = -Wall -Wextra
CXXFLAGS = -std=c++20 $(WARN) $(DEPFLAGS) -O2 -municode
CFLAGS   = -std=c11 $(WARN) $(DEPFLAGS) -O2

LDFLAGS  = -mwindows -municode -static
LDLIBS   = -lgdi32 -lshell32 -luser32 -lole32 -lwindowscodecs

# ---- sources ---------------------------------------------------------------------------

CXX_SRCS     := $(shell find src -name '*.cpp' | sort)
C_SRCS       := third_party/volk/volk.c

SHADER_SRCS  := $(sort $(wildcard shaders/*.vert shaders/*.frag shaders/*.comp))
SHADER_INCS  := $(sort $(wildcard shaders/*.glsl))
SPVS         := $(patsubst shaders/%,$(SPVDIR)/%.spv,$(SHADER_SRCS))
SHADER_C     := $(GENDIR)/shaders_generated.c

OBJS         := $(CXX_SRCS:%.cpp=$(OBJDIR)/%.o) \
                $(C_SRCS:%.c=$(OBJDIR)/%.o) \
                $(SHADER_C:$(GENDIR)/%.c=$(OBJDIR)/gen/%.o)
DEPS         := $(OBJS:.o=.d)

.PHONY: all clean install
.SUFFIXES:

all: $(TARGET)

# ---- shaders ---------------------------------------------------------------------------
# -Werror: a pipeline never ships with a diagnostic nobody read.

$(SPVDIR)/%.spv: shaders/% $(SHADER_INCS) | $(SPVDIR)
	$(GLSLC) -Werror -O -Ishaders -o $@ $<

$(SHADER_C): $(SPVS) tools/embed_shaders.sh | $(GENDIR)
	sh tools/embed_shaders.sh $@ $(SPVS)

# ---- objects ---------------------------------------------------------------------------

$(OBJDIR)/%.o: %.cpp $(SHADER_C) Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: %.c Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/gen/%.o: $(GENDIR)/%.c Makefile | $(BUILDTMP)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ---- resources -------------------------------------------------------------------------

$(RES): the-black-hole.rc the-black-hole.manifest src/app/resource.h assets/credits-default.txt \
        assets/fonts/Michroma-Regular.ttf assets/fonts/Michroma-OFL.txt \
        assets/earth/earth-day.jpg assets/earth/earth-night.jpg | $(BUILDTMP)
	$(WINDRES) the-black-hole.rc -O coff -o $@

# ---- link ------------------------------------------------------------------------------

$(TARGET): $(OBJS) $(RES) | $(BUILDTMP)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(RES) $(LDFLAGS) $(LDLIBS)

# ---- housekeeping ----------------------------------------------------------------------

$(BUILD) $(SPVDIR) $(GENDIR) $(BUILDTMP):
	@mkdir -p $@

clean:
	rm -rf $(TARGET) $(BUILD)

-include $(DEPS)

# Installed under Local AppData rather than System32: no Administrator needed, and never the
# build output itself, because Windows holds a running .scr open and the next link would fail.
# The destination comes from the registry because MSYS starts make with an almost empty
# environment (LOCALAPPDATA and friends are unset).
SHELLFOLDERS = HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders
DESKTOPKEY   = HKCU\\Control Panel\\Desktop

install: $(TARGET)
	@local=$$(reg.exe query "$(SHELLFOLDERS)" //v "Local AppData" \
		| sed -n 's/.*REG_[A-Z_]*[[:space:]]*//p' | tr -d '\r'); \
	test -n "$$local" || { echo "could not find Local AppData in the registry" >&2; exit 1; }; \
	dir=$$(cygpath -u "$$local")/The-Black-Hole; \
	mkdir -p "$$dir"; \
	cp $(TARGET) "$$dir/$(TARGET)" || { \
		echo "could not write $$dir/$(TARGET); it is probably running" >&2; exit 1; }; \
	win="$$(cygpath -w "$$dir")\\$(TARGET)"; \
	reg.exe add "$(DESKTOPKEY)" //v SCRNSAVE.EXE //t REG_SZ //d "$$win" //f > /dev/null; \
	reg.exe add "$(DESKTOPKEY)" //v ScreenSaveActive //t REG_SZ //d 1 //f > /dev/null; \
	echo "installed $$win and selected it as the screen saver"
