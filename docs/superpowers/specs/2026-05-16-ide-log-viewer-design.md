# Design: IDE-Style Log Viewer Improvements
Date: 2026-05-16

## Overview
The goal is to evolve the current structured log terminal into a professional "IDE Console" experience. This focuses on making the logs easier to select, copy, and read through improved interactivity and visual polish.

## Requirements
- **Row Selection**: Ability to select one or multiple rows for copying.
- **Selective Copying**: A "Copy Selected" button to copy only marked rows.
- **Quick Actions**: Right-click context menu for "Copy Line" and "Filter by Level".
- **Visual Polish**: 
    - Increased row height/spacing.
    - Hover effects for row clarity.
    - "Badge" styling for log levels (INFO, WARN, ERR).
    - Clear visual feedback for selected rows.

## Technical Design

### 1. Selection State Management
We will introduce a `LogSelectionState` to track selected rows per server.

```cpp
struct LogSelectionState {
    std::set<int> selected_indices;
    bool multi_select = false;
};
```

### 2. Interaction Logic
- **Row Clicking**:
    - Single click $\rightarrow$ Clear previous selection and select current row.
    - `Cmd/Ctrl + Click` $\rightarrow$ Toggle selection of current row.
- **Context Menu**:
    - Using `ImGui::OpenPopupContextItem`, a popup will be triggered on the row.
    - Actions: `Copy Line` (immediate copy to clipboard), `Filter by this Level` (sets the filter state to only show this level).

### 3. UI Enhancements (Rendering)
- **Row Rendering**:
    - Use `ImGui::Selectable` or custom `ImDrawList` background fills to handle selection and hover colors.
    - Implement a "Badge" look for the Level column by drawing a filled rounded rectangle behind the text.
- **Header Updates**:
    - Add a "Copy Selected" button next to "Copy All".
    - Add a "Clear Selection" button.

### 4. Integration Plan
1. Update `main.cpp` to include `<set>` and a state tracker for selection.
2. Modify `UiDrawLogTable` to handle selection logic and row highlighting.
3. Implement the context menu logic within the table loop.
4. Update the terminal header to include the new copy/clear buttons.
