# --- Configuration ---
BUILD_DIR = build
EXECUTABLE = $(BUILD_DIR)/LlamaFlow

# --- Default Target ---
# Typing just 'make' will trigger the build process
all: build

# --- Build Process ---
# This tells CMake to compile the project
build: $(BUILD_DIR)/Makefile
	@echo "=> Building LlamaFlow..."
	cmake --build $(BUILD_DIR)

# --- CMake Generation ---
# This creates the build folder and generates the CMake files if they don't exist
$(BUILD_DIR)/Makefile:
	@echo "=> Configuring CMake..."
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR)

# --- Run Application ---
# This builds the app (if needed) and then immediately runs it
run: build
	@echo "=> Running LlamaFlow..."
	./$(EXECUTABLE)

# --- Cleanup ---
# Deletes the build directory
clean:
	@echo "=> Cleaning build directory..."
	rm -rf $(BUILD_DIR)

# Deletes both the build directory AND the downloaded libraries
clean-deps: clean
	@echo "=> Cleaning dependencies..."
	rm -rf deps

.PHONY: all build run clean clean-deps