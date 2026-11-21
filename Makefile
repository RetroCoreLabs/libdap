# ND100X Makefile - CMake wrapper
# This Makefile provides backward compatibility by calling CMake

# Build directories
BUILD_DIR = build
BUILD_DIR_DEBUG = build
BUILD_DIR_RELEASE = build
BUILD_DIR_SANITIZE = build_sane


# Commands with full paths
CMAKE := $(shell command -v cmake 2>/dev/null)

# SDL3 / ImGui graphical DAP client (separate sub-project)
GUI_DIR    = tools/dap-debugger
GUI_BUILD  = $(GUI_DIR)/build
GUI_BINARY = $(GUI_BUILD)/dap_gui_debugger
GUI_HOST  ?= localhost
GUI_PORT  ?= 4711

# Default target is debug
.PHONY: all
all: debug

# Build targets
.PHONY: debug release sanitize clean rundbg runsrv gui rungui help

debug: 
	@echo "Building debug version..."
	$(CMAKE) -B $(BUILD_DIR_DEBUG) -S . -DCMAKE_BUILD_TYPE=Debug -DBUILD_EXECUTABLES=ON
	$(CMAKE) --build $(BUILD_DIR_DEBUG) -- -j$$(nproc 2>/dev/null || echo 4)

release: 
	@echo "Building release version..."
	$(CMAKE) -B $(BUILD_DIR_RELEASE) -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_EXECUTABLES=ON
	$(CMAKE) --build $(BUILD_DIR_RELEASE) -- -j$$(nproc 2>/dev/null || echo 4)

sanitize:
	@echo "Building with sanitizers..."
	$(CMAKE) -B $(BUILD_DIR_SANITIZE) -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" -DBUILD_EXECUTABLES=ON
	$(CMAKE) --build $(BUILD_DIR_SANITIZE) -- -j$$(nproc 2>/dev/null || echo 4)

clean:
	@echo "Cleaning build directories..."
	rm -rf $(BUILD_DIR) $(BUILD_DIR_DEBUG) $(BUILD_DIR_RELEASE) $(BUILD_DIR_SANITIZE) $(BUILD_DIR_WASM)

runsrv: debug	
	@echo "Running mock server..."
	valgrind --leak-check=full $(BUILD_DIR_DEBUG)/bin/dap_mock_server  --debug

run: debug
	@echo "Running debugger..."
	valgrind --leak-check=full  $(BUILD_DIR)/bin/dap_debugger ../tests/test_program.exe --debug

# Build the SDL3/ImGui graphical DAP client.
# SDL3 + Dear ImGui are fetched automatically by its CMakeLists.txt.
gui:
	@echo "Building SDL3/ImGui graphical DAP client..."
	$(CMAKE) -B $(GUI_BUILD) -S $(GUI_DIR) -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(GUI_BUILD) -- -j$$(nproc 2>/dev/null || echo 4)
	@echo ""
	@echo "Built: $(GUI_BINARY)"
	@echo "Start it with:  make rungui        (connects to $(GUI_HOST):$(GUI_PORT))"
	@echo "            or:  $(GUI_BINARY) --host HOST --port PORT"

# Build and launch the graphical client, connecting to a running DAP server.
# Override the target with: make rungui GUI_HOST=localhost GUI_PORT=5555
# (Start a server first, e.g. in another terminal:  make runsrv)
rungui: gui
	@echo "Launching graphical client -> $(GUI_HOST):$(GUI_PORT) ..."
	$(GUI_BINARY) --host $(GUI_HOST) --port $(GUI_PORT)

help:
	@echo "libDAP makefile - CMake wrapper"	
	@echo "-------------------------------------------------------------------------------"
	@echo "Build targets:"
	@echo "  all (default) - Same as 'debug'"
	@echo "  debug         - Build debug version (library + CLI executables)"
	@echo "  release       - Build release version"
	@echo "  sanitize      - Build with AddressSanitizer (-fsanitize=address) in $(BUILD_DIR_SANITIZE)/"
	@echo "  gui           - Build the SDL3/ImGui graphical DAP client"
	@echo "  clean         - Remove build directories"
	@echo ""
	@echo "Run targets:"
	@echo "  runsrv        - Build and run the mock server (port 4711)"
	@echo "  run           - Build and run the CLI debugger (under valgrind)"
	@echo "  rungui        - Build and launch the graphical DAP client"
	@echo "                  Connects to $(GUI_HOST):$(GUI_PORT) by default; override with:"
	@echo "                    make rungui GUI_HOST=localhost GUI_PORT=5555"
	@echo "  help          - Show this help"
	@echo ""
	@echo "Quick start for the graphical client:"
	@echo "  1) Terminal A:  make runsrv        # start a DAP server (mock, port 4711)"
	@echo "  2) Terminal B:  make rungui        # build + launch the GUI and connect"
	@echo ""
	@echo "This Makefile is a wrapper around CMake. If you prefer, you can use CMake directly:"
	@echo "  cmake -B build"
	@echo "  cmake --build build"