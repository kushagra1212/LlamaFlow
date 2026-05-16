# IDE-Style Log Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enhance the terminal log viewer with row selection, a dedicated "Copy Selected" feature, context menus, and a polished "IDE console" aesthetic.

**Architecture:**
- Introduce a `LogSelectionState` to track selected indices per server.
- Use `ImGui::Selectable` and custom `ImDrawList` calls within the log table to manage visual highlighting and selection events.
- Implement `ImGui::OpenPopupContextItem` for row-specific actions.

**Tech Stack:** C++, ImGui

---

### Task 1: Selection State Infrastructure

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Define `LogSelectionState` struct**
Add this struct near `ServerFilterState`:
```cpp
struct LogSelectionState {
    std::set<int> selected_indices;
};
```

- [ ] **Step 2: Integrate selection state into the filter map**
Update the map in `UiDrawServerLogViewport` to track both filter and selection state.
Modify:
```cpp
struct ServerUIState {
    ServerFilterState filter;
    LogSelectionState selection;
};
static std::map<ServerInstance*, ServerUIState> server_states;
```
And update the `auto& state = filters[srv];` line to `auto& state = server_states[srv];`.

- [ ] **Step 3: Commit**
```bash
git add main.cpp
git commit -m "feat: add log selection state infrastructure"
```

---

### Task 2: Interactive Row Rendering and Selection

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Implement row selection logic in `UiDrawLogTable`**
Within the log loop, use `ImGui::Selectable` or a custom check to handle clicks.

```cpp
// Inside the loop for each entry in logs:
int idx = // current index of the log entry in the original deque
bool is_selected = state.selection.selected_indices.count(idx) > 0;

if (ImGui::Selectable(entry.message.c_str(), is_selected)) {
    if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyCmd) {
        if (is_selected) state.selection.selected_indices.erase(idx);
        else state.selection.selected_indices.insert(idx);
    } else {
        state.selection.selected_indices.clear();
        state.selection.selected_indices.insert(idx);
    }
}
```

- [ ] **Step 2: Add hover and selection highlighting**
Ensure the `ImGuiTableFlags_RowBg` is still present, but overlay the selection color for selected rows.

- [ ] **Step 3: Commit**
```bash
git add main.cpp
git commit -m "feat: implement interactive row selection in log table"
```

---

### Task 3: Context Menu and Quick Actions

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Implement right-click context menu on rows**
Add `ImGui::OpenPopupContextItem` inside the table loop.

```cpp
if (ImGui::BeginPopupContextItem("RowContext")) {
    if (ImGui::MenuItem("Copy Line")) {
        std::string line = entry.timestamp + " [" + entry.level + "] " + entry.message;
        ImGui::SetClipboardText(line.c_str());
    }
    if (ImGui::MenuItem("Filter by this Level")) {
        state.filter.show_info = (entry.level == "INFO");
        state.filter.show_warn = (entry.level == "WARN");
        state.filter.show_err = (entry.level == "ERROR");
    }
    ImGui::EndPopup();
}
```

- [ ] **Step 2: Commit**
```bash
git add main.cpp
git commit -m "feat: add row context menu for copying and filtering"
```

---

### Task 4: Header Actions (Copy Selected & Clear)

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Add "Copy Selected" and "Clear Selection" buttons to `UiDrawServerLogViewport`**

```cpp
if (ImGui::Button("Copy Selected")) {
    std::string selected_logs;
    auto logs = srv->get_logs();
    for (int idx : state.selection.selected_indices) {
        if (idx >= 0 && idx < logs.size()) {
            const auto& entry = logs[idx];
            selected_logs += entry.timestamp + " [" + entry.level + "] " + entry.message + "\\n";
        }
    }
    ImGui::SetClipboardText(selected_logs.c_str());
    ToastPush(ToastKind::Success, "Selected logs copied to clipboard.");
}
ImGui::SameLine();
if (ImGui::Button("Clear Selection")) {
    state.selection.selected_indices.clear();
}
```

- [ ] **Step 2: Commit**
```bash
git add main.cpp
git commit -m "feat: add copy selected and clear selection buttons"
```

---

### Task 5: Visual Polish (Badges and Spacing)

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Implement "Badge" styling for the Level column**
Instead of `ImGui::TextColored`, use a custom draw call to create a rounded background for the level text.

```cpp
// In the Level column:
ImVec4 level_col = entry.color;
ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
ImGui::PushStyleColor(ImGuiCol_Button, level_col);
if (ImGui::Button(entry.level.c_str(), ImVec2(60, 20))) { /* no action */ }
ImGui::PopStyleColor(2);
```
(Note: For a true badge, use `ImDrawList` to draw a rounded rect behind the text).

- [ ] **Step 2: Increase row height/spacing**
Use `ImGui::SetCursorPosY` or adjust `style.ItemSpacing` locally within the table to increase the vertical gap between rows.

- [ ] **Step 3: Commit**
```bash
git add main.cpp
git commit -m "style: apply IDE-style visual polish to log terminal"
```
