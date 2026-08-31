# Tetris — bgfx (Metal) + Cocoa
CXX      := c++
CXXFLAGS := -std=c++17 -O2
BGFX     := third-party/bgfx
BX       := third-party/bx
BIN      := $(BGFX)/.build/osx-arm64/bin

INC := -I $(BGFX)/include -I $(BX)/include -I .
LIB := -L $(BIN) -lbgfxRelease -lbimgRelease -lbxRelease
FW  := -framework Cocoa -framework QuartzCore -framework Metal \
       -framework IOKit -framework CoreVideo -framework CoreMedia \
       -framework VideoToolbox

SHADERS := shaders/vs_quad.h shaders/fs_quad.h

all: tetris

tetris: main.cpp window.mm font.h window.h $(SHADERS)
	$(CXX) $(CXXFLAGS) main.cpp window.mm $(INC) $(LIB) $(FW) -o tetris

# Regenerate the embedded shader headers (run once if you edit the .sc sources).
shaders:
	BIN=$(BIN) BGFX=$(BGFX) python3 scripts/build_shaders.py

clean:
	rm -f tetris

.PHONY: all shaders clean
