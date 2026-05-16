# Design: Enhanced Java-Style Log Terminal
Date: 2026-05-16

## Overview
The goal is to replace the current raw text stream in the LlamaFlow Orchestrator terminal with a structured, professional log viewer similar to Java IDE consoles. This will improve readability, searchability, and the overall user experience when monitoring model servers.

## Requirements
- **User-Friendly**: Clear visual separation of log levels and timestamps.
- **Copyable**: Easy copying of individual lines and all logs.
- **Expandable**: A "Maximize" feature to see logs in a large, focused view.
- **Searchable**: Real-time filtering of logs by keyword.
- **Filterable**: Ability to toggle visibility of log levels (INFO, WARN, ERROR).

## Technical Design

### 1. Data Representation
We will move from `std::deque<std::string>` (raw lines) to a structured `LogEntry` system.

```cpp
struct LogEntry {
    std::string timestamp;
    std::string level;
    std::string message;
    ImVec4 color;
};
```

### 2. Log Parsing Logic
A new parser will process incoming raw strings from the server:
- **Timestamp Extraction**: Detect and isolate date/time patterns at the start of lines.
- **Level Identification**: 
    - `ERROR`: Keywords like "error", "fail", "FAILED".
    - `WARN`: Keywords like "warning", "WARN".
    - `INFO`: Keywords like "info", "started", "listening".
    - `DEBUG`: All other lines.
- **Color Mapping**: Assign colors based on the identified level (Red for ERROR, Yellow for WARN, Blue/Green for INFO).

### 3. UI Implementation

#### A. Terminal Header
Added to the top of the terminal pane:
- `ImGui::InputText`: Search filter for the `message` field.
- `ImGui::Checkbox` or Small Buttons: Toggles for `INFO`, `WARN`, and `ERR` visibility.
- `ImGui::Button("Copy All")`: Copies all currently visible logs to the clipboard.
- `ImGui::Button("Maximize")`: Triggers a full-screen overlay window.

#### B. Log Table
Replaces the current `ImGui::Text` loop with `ImGui::BeginTable`:
- **Column 1 (Time)**: Fixed width, dimmed text.
- **Column 2 (Level)**: Fixed width, color-coded based on `LogEntry::level`.
- **Column 3 (Message)**: Flexible width, supporting text wrapping.

#### C. Maximize Overlay
A separate `ImGui::Begin` window that appears when Maximize is toggled:
- Centered on screen.
- Large dimensions.
- Contains the same table and filter controls as the main terminal.

## Integration Plan
1. Update `ServerInstance` or create a `LogManager` to handle the `LogEntry` storage.
2. Implement the parsing logic in `LlamaManager` or a helper utility.
3. Refactor `UiDrawServerLogViewport` in `main.cpp` to use the new table-based UI.
4. Implement the Maximize overlay state management in the main loop.
