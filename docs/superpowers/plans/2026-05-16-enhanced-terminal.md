# Enhanced Terminal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the raw text terminal with a structured, Java-style debug log viewer featuring filtering, searching, and a maximize mode.

**Architecture:**
- Transform raw server output (`std::string`) into structured `LogEntry` objects via a parser.
- Use `ImGui::BeginTable` for the log display to achieve a columnar "IDE console" look.
- Implement a state-driven overlay window for the Maximize mode.

**Tech Stack:** C++, ImGui

---

### Task 1: Define Log Structure and Update ServerInstance

**Files:**
- Modify: `LlamaManager.hpp`

- [ ] **Step 1: Define `LogEntry` struct**
Add the following struct before `ServerInstance`:
```cpp
struct LogEntry {
    std::string timestamp;
    std::string level;
    std::string message;
    ImVec4 color;
};
```

- [ ] **Step 2: Update `ServerInstance` log storage**
Change `std::deque<std::string> logs` to `std::deque<LogEntry> logs` in the `ServerInstance` struct.

- [ ] **Step 3: Update `get_logs()` method**
Update the return type of `get_logs()` to `std::deque<LogEntry>`.

- [ ] **Step 4: Commit**
```bash
git add LlamaManager.hpp
git commit -m "feat: define LogEntry struct and update ServerInstance storage"
```

---

### Task 2: Implement Log Parsing Logic

**Files:**
- Modify: `LlamaManager.cpp`
- Modify: `LlamaManager.hpp` (Add parser helper)

- [ ] **Step 1: Add `parse_log_line` helper to `LlamaManager.hpp` or as a static helper in `.cpp`**
Implement a function that takes a `std::string` and returns a `LogEntry`.

```cpp
LogEntry parse_log_line(const std::string& line) {
    LogEntry entry;
    entry.message = line;
    entry.level = "DEBUG";
    entry.color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); // Default gray

    // Simple level detection
    if (line.find("error") != std::string::npos || line.find("fail") != std::string::npos || line.find("FAILED") != std::string::npos) {
        entry.level = "ERROR";
        entry.color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    } else if (line.find("warning") != std::string::npos || line.find("WARN") != std::string::npos) {
        entry.level = "WARN";
        entry.color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
    } else if (line.find("loaded model") != std::string::npos || line.find("server is listening") != std::string::npos || line.find("started") != std::string::npos) {
        entry.level = "INFO";
        entry.color = ImVec4(0.2f, 1.0f, 0.4f, 1.0f);
    }

    // Timestamp extraction (simplified: assume [HH:MM:SS] or similar at start)
    // If line starts with '[', try to find matching ']'
    if (!line.empty() && line[0] == '[') {
        size_t end_bracket = line.find(']');
        if (end_bracket != std::string::npos) {
            entry.timestamp = line.substr(1, end_bracket - 1);
            entry.message = line.substr(end_bracket + 1);
            if (!entry.message.empty() && entry.message[0] == ' ') entry.message.erase(0, 1);
        }
    }

    return entry;
}
```

- [ ] **Step 2: Integrate `parse_log_line` into log collection**
Find where `ServerInstance` appends raw strings to `logs`. Update it to call `parse_log_line` before pushing to the deque.

- [ ] **Step 3: Commit**
```bash
git add LlamaManager.cpp LlamaManager.hpp
git commit -m "feat: implement structured log parsing"
```

---

### Task 3: Build the Log Table UI

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Implement `UiDrawLogTable` helper**
Create a function that takes `ServerInstance*` and a filter string, and draws the table.

```cpp
void UiDrawLogTable(ServerInstance* srv, const std::string& filter, bool showInfo, bool showWarn, bool showErr) {
    if (!srv) return;
    
    if (ImGui::BeginTable("LogTable", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto logs = srv->get_logs();
        for (const auto& entry : logs) {
            // Filtering
            if (!filter.empty() && entry.message.find(filter) == std::string::npos) continue;
            if (entry.level == "INFO" && !showInfo) continue;
            if (entry.level == "WARN" && !showWarn) continue;
            if (entry.level == "ERROR" && !showErr) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", entry.timestamp.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(entry.color, "%s", entry.level.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(entry.message.c_str());
        }
        ImGui::EndTable();
    }
}
```

- [ ] **Step 2: Replace `UiDrawServerLogViewport` with `UiDrawLogTable`**
In `main.cpp`, update the terminal view to call the new table helper.

- [ ] **Step 3: Commit**
```bash
git add main.cpp
git commit -m "feat: implement table-based log viewer"
```

---

### Task 4: Add Terminal Header (Search, Filter, Copy)

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Implement Header UI**
Add a search bar and filter toggles above the log table.

```cpp
static std::string log_filter;
static bool show_info = true, show_warn = true, show_err = true;

// Inside the terminal view loop:
ImGui::Text("Filter:"); 
ImGui::SameLine();
ImGui::SetNextItemWidth(-100);
if (ImGui::InputText("##filter", &log_filter)) { /* filter updated */ }
ImGui::SameLine();
if (ImGui::Button("Clear")) log_filter.clear();

ImGui::Spacing();
ImGui::Checkbox("INFO", &show_info); ImGui::SameLine();
ImGui::Checkbox("WARN", &show_warn); ImGui::SameLInE();
ImGui::Checkbox("ERR", &show_err);

ImGui::Spacing();
if (ImGui::Button("Copy All")) {
    std::string all_logs;
    auto logs = srv->get_logs();
    for (const auto& l : logs) all_logs += l.message + "\\n";
    ImGui::SetClipboardText(all_logs.c_str());
}
```

- [ ] **Step 5: Commit**
```bash
git add main.cpp
git commit -m "feat: add search and filter controls to terminal"
```

---

### Task 5: Implement Maximize Overlay

**Files:**
- Modify: `main.cpp`

- [ ] **Step 1: Add state variable for maximize mode**
Add `static bool g_maximize_logs = false;` to the main loop.

- [ ] **Step 2: Implement Maximize Button and logic**
Add a "Maximize" button to the header.
If `g_maximize_logs` is true, render a large `ImGui::Begin` window on top of everything.

```cpp
if (g_maximize_logs) {
    ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("Log Viewer (Maximized)", nullptr, ImGuiWindowFlags_NoCollapse);
    
    // Call the same UiDrawLogTable with filters
    UiDrawLogTable(srv, log_filter, show_info, show_warn, show_err);
    
    if (ImGui::Button("Close")) g_maximize_logs = false;
    ImGui::End();
}
```

- [ ] **Step 6: Commit**
```bash
git add main.cpp
git commit -m "feat: implement log maximize overlay"
```
