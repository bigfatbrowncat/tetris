# Tetris — bgfx (Metal) + Cocoa
CXX      := c++
CXXFLAGS := -std=c++17 -O2
BGFX     := third-party/bgfx
BX       := third-party/bx
BIN      := $(BGFX)/.build/osx-arm64/bin

# Third-party build: bgfx's GENie-generated gmake project (release config).
TP_PROJ := $(BGFX)/.build/projects/gmake-osx-arm64
TP_CFG  := release
TP_LIBS := $(BIN)/libbgfxRelease.a $(BIN)/libbimgRelease.a $(BIN)/libbxRelease.a

INC := -I $(BGFX)/include -I $(BX)/include -I .
LIB := -L $(BIN) -lbgfxRelease -lbimgRelease -lbxRelease
FW  := -framework Cocoa -framework QuartzCore -framework Metal \
       -framework IOKit -framework CoreVideo -framework CoreMedia \
       -framework VideoToolbox

SHADERS := shaders/vs_quad.h shaders/fs_quad.h

all: tetris

# --- third-party (bgfx / bimg / bx / shaderc) -----------------------------

# Library targets, built on demand when the .a file is missing (e.g. fresh
# clone). bgfx and bimg pull in bx automatically.
$(BIN)/libbgfxRelease.a:
	$(MAKE) -C $(TP_PROJ) config=$(TP_CFG) bgfx

$(BIN)/libbimgRelease.a:
	$(MAKE) -C $(TP_PROJ) config=$(TP_CFG) bimg

$(BIN)/libbxRelease.a:
	$(MAKE) -C $(TP_PROJ) config=$(TP_CFG) bx

# Shader compiler (heavy: pulls in glslang / SPIRV-Tools / tint). Only needed
# by the `shaders` target.
.PHONY: shaderc
shaderc:
	$(MAKE) -C $(TP_PROJ) config=$(TP_CFG) shaderc

# Everything the project needs from third-party.
.PHONY: third-party
third-party: $(TP_LIBS) shaderc

# --- app --------------------------------------------------------------------

tetris: main.cpp window.mm font.h window.h $(SHADERS) $(TP_LIBS)
	$(CXX) $(CXXFLAGS) main.cpp window.mm $(INC) $(LIB) $(FW) -o tetris

# Regenerate the embedded shader headers (run once if you edit the .sc sources).
shaders: shaderc
	BIN=$(BIN) BGFX=$(BGFX) python3 scripts/build_shaders.py

clean:
	rm -f tetris
	-$(MAKE) -C $(TP_PROJ) config=$(TP_CFG) clean

.PHONY: all third-party shaderc shaders clean
