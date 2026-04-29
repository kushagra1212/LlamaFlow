# LlamaFlow

> A lightweight, native macOS desktop orchestrator for managing and monitoring [llama.cpp](https://github.com/ggerganov/llama.cpp) server instances.

LlamaFlow gives you a visual control panel for your local LLM servers. Launch models, watch live throughput metrics, inspect server logs, and shut down instances — all from a clean, minimal interface built with Dear ImGui and OpenGL.

<img width="1477" height="928" alt="Screenshot 2026-04-29 at 10 52 07 PM" src="https://github.com/user-attachments/assets/b43ec7da-53b2-4ba3-b5ad-f18aaaf4e294" />

---

## Features

- **Model Configuration Manager** — Save and manage multiple `.gguf` model profiles with custom launch arguments.
- **One-Click Launch** — Start any configured model with a single button.
- **Live Metrics Dashboard** — Real-time throughput (prompt/eval tokens per second), token counters, request stats, and slot usage.
- **Terminal Log View** — Color-coded, auto-scrolling server logs inside each active model panel.
- **Auto-Discovery** — Automatically detects and attaches to externally running llama-server processes.
- **Toast Notifications** — In-app feedback for saves, launches, and errors.
- **Resizable Layout** — Draggable splitters for the active models panel and terminal height.
- **Native macOS App** — Standalone `.app` bundle with a custom dock icon and `.dmg` installer.

---

## Download & Install

### Option 1: Download Prebuilt `.dmg` (Recommended)

1. Go to the [Releases](https://github.com/YOUR_USERNAME/LlamaFlow/releases) page.
2. Download the latest `LlamaFlow-x.x.x-macOS.dmg`.
3. Open the `.dmg` and drag **LlamaFlow** into your **Applications** folder.
4. Launch LlamaFlow from Launchpad or Spotlight.

> **Note:** On first launch, macOS may warn that the app is from an unidentified developer. Go to **System Settings → Privacy & Security** and click **Open Anyway**.

### Option 2: Build from Source

See the [Developer Guide](#developer-guide) below.

---

## Using LlamaFlow

### 1. Add a Model Configuration

- Click **+ Add New Model** in the left sidebar.
- Fill in the fields:
  - **Model Name Alias** — e.g. `Llama-3-8B`
  - **Executable Path** — path to your `llama-server` binary
  - **Model File** — path to your `.gguf` file
  - **Network Port** — the port `llama-server` will listen on (default: `8080`)
  - **Custom Arguments** — any extra CLI flags for `llama-server`
- Click **Save Changes**.

### 2. Launch a Server

- Select a model from the sidebar.
- Click **START SERVER**.
- LlamaFlow spawns the process and opens an **ACTIVE MODELS** tab on the right.

### 3. Monitor

- **THROUGHPUT** — Prompt and generation tokens per second.
- **TOKENS** — Total prompt and generated token counts.
- **REQUESTS** — Completed requests and slot utilization.
- **SYSTEM** — Port number and estimated VRAM usage.
- **Progress Bars** — Visual feedback during model loading and context processing.
- **TERMINAL** — Live server stdout with syntax highlighting.

### 4. Stop

- Click **Stop** inside the active model tab to gracefully shut down the server.

### Configuration File

All saved configs are stored in `configs.json` in the same directory as the executable. You can edit this file directly if needed.

---

## Developer Guide

### Prerequisites

- macOS 10.14+
- [Xcode Command Line Tools](https://developer.apple.com/xcode/resources/)
- [CMake](https://cmake.org/download/) 3.15+
- [Homebrew](https://brew.sh/) (optional but recommended)

Install Xcode Command Line Tools:
```bash
xcode-select --install
```

Install CMake via Homebrew:
```bash
brew install cmake
```

### Clone & Build

```bash
# Clone the repo
git clone https://github.com/YOUR_USERNAME/LlamaFlow.git
cd LlamaFlow

# Create build directory
mkdir build && cd build

# Generate build files
cmake .. -DCMAKE_BUILD_TYPE=Release

# Compile
cmake --build . --config Release -j$(sysctl -n hw.ncpu)
```

The resulting binary will be at `build/LlamaFlow`.

### Run from Build Directory

```bash
./build/LlamaFlow
```

### Build the `.app` Bundle Locally

```bash
# After building the binary
APP_NAME="LlamaFlow"
APP_BUNDLE="${APP_NAME}.app"

mkdir -p "${APP_BUNDLE}/Contents/MacOS"
mkdir -p "${APP_BUNDLE}/Contents/Resources"

cp build/LlamaFlow "${APP_BUNDLE}/Contents/MacOS/"

# Generate icon from logo.png
if [ -f logo.png ]; then
  mkdir -p LlamaFlow.iconset
  sips -z 16 16   logo.png --out LlamaFlow.iconset/icon_16x16.png
  sips -z 32 32   logo.png --out LlamaFlow.iconset/icon_16x16@2x.png
  sips -z 32 32   logo.png --out LlamaFlow.iconset/icon_32x32.png
  sips -z 64 64   logo.png --out LlamaFlow.iconset/icon_32x32@2x.png
  sips -z 128 128 logo.png --out LlamaFlow.iconset/icon_128x128.png
  sips -z 256 256 logo.png --out LlamaFlow.iconset/icon_128x128@2x.png
  sips -z 256 256 logo.png --out LlamaFlow.iconset/icon_256x256.png
  sips -z 512 512 logo.png --out LlamaFlow.iconset/icon_256x256@2x.png
  sips -z 512 512 logo.png --out LlamaFlow.iconset/icon_512x512.png
  cp logo.png LlamaFlow.iconset/icon_512x512@2x.png
  iconutil -c icns LlamaFlow.iconset -o "${APP_BUNDLE}/Contents/Resources/${APP_NAME}.icns"
  rm -rf LlamaFlow.iconset
fi

# Create Info.plist
cat > "${APP_BUNDLE}/Contents/Info.plist" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>LlamaFlow</string>
  <key>CFBundleIdentifier</key>
  <string>com.llamaflow.app</string>
  <key>CFBundleName</key>
  <string>LlamaFlow</string>
  <key>CFBundleDisplayName</key>
  <string>LlamaFlow Orchestrator</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0.0</string>
  <key>CFBundleVersion</key>
  <string>1.0.0</string>
  <key>LSMinimumSystemVersion</key>
  <string>10.14</string>
  <key>CFBundleIconFile</key>
  <string>LlamaFlow</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

chmod +x "${APP_BUNDLE}/Contents/MacOS/LlamaFlow"
```

### Build the `.dmg` Installer Locally

Requires [create-dmg](https://github.com/create-dmg/create-dmg):

```bash
brew install create-dmg

create-dmg \
  --volname "LlamaFlow" \
  --window-pos 200 120 \
  --window-size 800 400 \
  --icon-size 100 \
  --app-drop-link 600 185 \
  --icon "LlamaFlow.app" 200 185 \
  "LlamaFlow-macOS.dmg" \
  "LlamaFlow.app"
```

### Project Structure

```
LlamaFlow/
├── main.cpp              # Main application loop + ImGui UI
├── LlamaManager.cpp/hpp  # Config management & server orchestration
├── mac_icon.mm           # macOS dock icon setup (Cocoa)
├── logo.png              # App logo (embedded in binary & .icns)
├── stb_image.h           # Single-header image loader
├── logo_png.h            # Embedded PNG byte array
├── CMakeLists.txt        # Build configuration
├── configs.json          # Saved model configurations (runtime)
├── deps/
│   ├── imgui/            # Dear ImGui (auto-downloaded)
│   └── json/             # nlohmann/json (auto-downloaded)
└── .github/workflows/
    └── release.yml       # CI/CD for GitHub Releases
```

### Dependencies

All dependencies are fetched automatically during the CMake configure step:

| Dependency | Source | License |
|------------|--------|---------|
| [Dear ImGui](https://github.com/ocornut/imgui) | GitHub (auto-clone) | MIT |
| [GLFW](https://github.com/glfw/glfw) | GitHub (FetchContent) | Zlib/libpng |
| [nlohmann/json](https://github.com/nlohmann/json) | GitHub (auto-download) | MIT |
| [stb_image](https://github.com/nothings/stb) | GitHub (header) | MIT/Public Domain |

No Homebrew dependencies are required to build or run the app.

---

## License

This project is licensed under the [MIT License](LICENSE).

---

## Acknowledgements

- [llama.cpp](https://github.com/ggerganov/llama.cpp) by Georgi Gerganov
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut
- [GLFW](https://github.com/glfw/glfw) by the GLFW team
