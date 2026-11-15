################################################################################
# Makefile for the IFJ26 Optimizer Debugger                                    #
################################################################################
# Builds the dev web server. The ifj26-opt binaries are built by the server    #
# itself from the configured repository sources (see server.c).                #
################################################################################

CC := clang
CFLAGS := -std=c17 -Wall -Wextra -O2 -D_CRT_SECURE_NO_WARNINGS

ifeq ($(OS),Windows_NT)
    EXE := .exe
    LIBS := -lws2_32
else
    EXE :=
    LIBS := -lm -lpthread
endif

# Server binary and build directory
BUILD_DIR := build
SERVER_BIN := $(BUILD_DIR)/server$(EXE)

################################################################################
# Recipes                                                                      #
################################################################################

# Default target: build the server
$(SERVER_BIN): server.c | $(BUILD_DIR)
	@$(CC) $(CFLAGS) -o $@ $< $(LIBS)

$(BUILD_DIR):
ifeq ($(OS),Windows_NT)
	@-cmd.exe /C mkdir $(subst /,\,$@)
else
	@-mkdir -p $@
endif

# Start the playground (reads config.txt from the current directory)
run: $(SERVER_BIN)
	@$(SERVER_BIN)

# Build both ifj26-opt flavors (release + debug) without starting the server,
# using the same build logic the server uses for hot reload
opt: $(SERVER_BIN)
	@$(SERVER_BIN) --build-only

clean:
ifeq ($(OS),Windows_NT)
	@-cmd.exe /C if exist $(BUILD_DIR) rmdir /S /Q $(BUILD_DIR)
else
	@-rm -rf $(BUILD_DIR)
endif

.PHONY: run opt clean
