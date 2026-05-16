#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include "LlamaManager.hpp"
#include <vector>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <deque>
#include <string>
#include <algorithm>
#include <map>
#include <set>

#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "logo_png.h"

#ifdef __APPLE__
extern "C" void SetMacDockIcon(const unsigned char* png_data, unsigned int len);
#endif

// ============================================================
//  Helper: load embedded logo PNG into OpenGL texture
// ============================================================
static GLuint LoadEmbeddedLogoTexture(int* out_width, int* out_height) {
    int w, h, channels;
    unsigned char* data = stbi_load_from_memory(
        _Users_apple_code_per_LlamaFlow_logo_png,
        _Users_apple_code_per_LlamaFlow_logo_png_len,
        &w, &h, &channels, 4);
    if (!data)
        return 0;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    return tex;
}

// ============================================================
//  Helper: std::string with ImGui InputText
// ============================================================
struct InputTextCallback_UserData {
    std::string* Str;
    ImGuiInputTextCallback ChainCallback;
    void* ChainCallbackUserData;
};

static int InputTextCallback(ImGuiInputTextCallbackData* data) {
    InputTextCallback_UserData* user_data = (InputTextCallback_UserData*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = user_data->Str;
        str->resize(data->BufTextLen);
        data->Buf = (char*)str->c_str();
    }
    return 0;
}

bool ImGui_InputText(const char* label, std::string* str, ImGuiInputTextFlags flags = 0) {
    InputTextCallback_UserData cb_user_data;
    cb_user_data.Str = str;
    return ImGui::InputText(label, (char*)str->c_str(), str->capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, InputTextCallback, &cb_user_data);
}

bool ImGui_InputTextMultiline(const char* label, std::string* str, const ImVec2& size = ImVec2(0, 0), ImGuiInputTextFlags flags = 0) {
    InputTextCallback_UserData cb_user_data;
    cb_user_data.Str = str;
    return ImGui::InputTextMultiline(label, (char*)str->c_str(), str->capacity() + 1, size, flags | ImGuiInputTextFlags_CallbackResize, InputTextCallback, &cb_user_data);
}

// ============================================================
//  Helper: format a large number to human-readable form
// ============================================================
static std::string format_number(uint64_t n) {
    if (n >= 1000000000) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2fB", n / 1e9);
        return buf;
    } else if (n >= 1000000) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2fM", n / 1e6);
        return buf;
    } else if (n >= 1000) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1fK", n / 1e3);
        return buf;
    }
    return std::to_string(n);
}

static std::string format_bytes(uint64_t bytes) {
    if (bytes >= 1073741824ULL) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
        return buf;
    } else if (bytes >= 1048576ULL) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2f MB", bytes / 1048576.0);
        return buf;
    } else if (bytes >= 1024ULL) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2f KB", bytes / 1024.0);
        return buf;
    }
    return std::to_string(bytes) + " B";
}

static std::string format_duration(double seconds) {
    int h = (int)(seconds / 3600);
    int m = (int)(seconds / 60) % 60;
    int s = (int)seconds % 60;
    char buf[32];
    if (h > 0) {
        snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, s);
    } else if (m > 0) {
        snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    } else {
        snprintf(buf, sizeof(buf), "%ds", s);
    }
    return buf;
}

static bool g_maximize_logs = false;
static ServerInstance* g_maximized_srv = nullptr;
static ImFont* g_mono_font = nullptr;   // monospace font for the log terminal

struct ServerFilterState {
    std::string filter;
    bool show_info = true;
    bool show_warn = true;
    bool show_err = true;
};

struct LogSelectionState {
    std::set<int> selected_indices;
};

struct ServerUIState {
    ServerFilterState filter;
    LogSelectionState selection;
    bool raw_view = true;        // raw mode = native drag-select + Cmd/Ctrl+C
    bool newest_first = true;    // reverse order: newest log at the top
    bool autoscroll = true;      // stick to newest line unless user scrolled up
    std::string raw_cache;       // backing buffer for the raw read-only textbox
};

enum class ToastKind { Info, Success, Warn, Err };

struct Toast {
    ToastKind kind{};
    std::string text;
    double expires_at{0};
};
static std::deque<Toast> g_toasts;

static void ToastPush(ToastKind k, std::string msg) {
    const double ttl = std::max(4.0, std::min(12.0, 2.5 + msg.size() / 42.0));
    g_toasts.push_back({k, std::move(msg), ImGui::GetTime() + ttl});
    while (g_toasts.size() > 10)
        g_toasts.pop_front();
}

// Shared filter predicate: matches the existing INFO/WARN/ERROR + substring rules.
static bool LogEntryPasses(const LogEntry& e, const ServerFilterState& f) {
    if (!f.filter.empty() && e.message.find(f.filter) == std::string::npos) return false;
    if (e.level == "INFO"  && !f.show_info) return false;
    if (e.level == "WARN"  && !f.show_warn) return false;
    if (e.level == "ERROR" && !f.show_err)  return false;
    return true;
}

void UiDrawLogTable(ServerInstance* srv, ServerUIState& state) {
    if (!srv) return;

    auto logs = srv->get_logs();

    // ---------- RAW view: real native click-drag selection + Cmd/Ctrl+C ----------
    if (state.raw_view) {
        state.raw_cache.clear();
        state.raw_cache.reserve(logs.size() * 80 + 1);
        auto append = [&](const LogEntry& e) {
            if (!LogEntryPasses(e, state.filter)) return;
            if (!e.timestamp.empty()) { state.raw_cache += '['; state.raw_cache += e.timestamp; state.raw_cache += "] "; }
            state.raw_cache += '['; state.raw_cache += e.level; state.raw_cache += "] ";
            state.raw_cache += e.message;
            state.raw_cache += '\n';
        };
        if (state.newest_first)
            for (auto it = logs.rbegin(); it != logs.rend(); ++it) append(*it);
        else
            for (const auto& e : logs) append(e);
        if (g_mono_font) ImGui::PushFont(g_mono_font);
        ImGui::InputTextMultiline("##rawlogs",
            (char*)state.raw_cache.c_str(), state.raw_cache.size() + 1,
            ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        if (g_mono_font) ImGui::PopFont();
        return;
    }

    // ---------- TABLE view: clipped, uniform single-line rows ----------
    // Pre-filter into a stable index list so ImGuiListClipper can virtualize
    // (essential now that we retain up to 100k lines).
    static std::vector<int> vis;
    vis.clear();
    for (int i = 0; i < (int)logs.size(); ++i)
        if (LogEntryPasses(logs[i], state.filter)) vis.push_back(i);
    if (state.newest_first)
        std::reverse(vis.begin(), vis.end());

    ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg
                          | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (ImGui::BeginTable("LogTable", 3, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn("Level",   ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthFixed, 4000.0f);
        ImGui::TableHeadersRow();

        if (g_mono_font) ImGui::PushFont(g_mono_font);

        ImGuiListClipper clipper;
        clipper.Begin((int)vis.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                int i = vis[row];
                const LogEntry& entry = logs[i];

                ImGui::TableNextRow();
                ImGui::PushID(i);

                // Full-row selectable FIRST. Uniform line height => the hit
                // region and the highlight always line up (fixes the old
                // multi-line overlap garble). Content is drawn on top after.
                ImGui::TableSetColumnIndex(0);
                bool is_selected = state.selection.selected_indices.count(i) > 0;
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::Selectable("##row", is_selected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {
                        if (is_selected) state.selection.selected_indices.erase(i);
                        else state.selection.selected_indices.insert(i);
                    } else {
                        state.selection.selected_indices.clear();
                        state.selection.selected_indices.insert(i);
                    }
                }

                std::string line_text =
                    (entry.timestamp.empty() ? std::string() : "[" + entry.timestamp + "] ")
                    + "[" + entry.level + "] " + entry.message;

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    ImGui::SetClipboardText(line_text.c_str());
                    ToastPush(ToastKind::Success, "Line copied to clipboard.");
                }
                if (ImGui::BeginPopupContextItem("ctx")) {
                    if (ImGui::MenuItem("Copy Line")) {
                        ImGui::SetClipboardText(line_text.c_str());
                        ToastPush(ToastKind::Success, "Line copied to clipboard.");
                    }
                    ImGui::Separator();
                    bool shown = (entry.level == "INFO"  && state.filter.show_info)
                              || (entry.level == "WARN"  && state.filter.show_warn)
                              || (entry.level == "ERROR" && state.filter.show_err);
                    std::string fl = (shown ? "Hide " : "Show ") + entry.level;
                    if (ImGui::MenuItem(fl.c_str())) {
                        if (entry.level == "INFO")       state.filter.show_info = !state.filter.show_info;
                        else if (entry.level == "WARN")  state.filter.show_warn = !state.filter.show_warn;
                        else if (entry.level == "ERROR") state.filter.show_err  = !state.filter.show_err;
                    }
                    ImGui::EndPopup();
                }

                // Overlay content (re-entering each column resets the cursor
                // to that cell's top-left, drawing over the selectable).
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", entry.timestamp.c_str());

                ImGui::TableSetColumnIndex(1);
                ImVec2 bp = ImGui::GetCursorScreenPos();
                float bw = 48.0f, bh = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    bp, ImVec2(bp.x + bw, bp.y + bh), ImColor(entry.color), 3.0f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 5.0f);
                ImGui::TextColored(ImVec4(0, 0, 0, 1), "%s", entry.level.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(entry.message.c_str());

                ImGui::PopID();
            }
        }
        clipper.End();

        if (g_mono_font) ImGui::PopFont();

        // Follow the newest line. With newest-first the newest row is at the
        // top, so snap to top; otherwise stick to bottom — and only while the
        // user hasn't scrolled away to read (no missed lines).
        if (state.autoscroll) {
            if (state.newest_first) {
                if (ImGui::GetScrollY() <= 0.0f) ImGui::SetScrollY(0.0f);
            } else if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }

        ImGui::EndTable();
    }
}

// ============================================================
//  Toast notifications (floating overlay)
// ============================================================


static void ToastDecayAndOverlay() {
    const double now = ImGui::GetTime();
    while (!g_toasts.empty() && g_toasts.front().expires_at < now)
        g_toasts.pop_front();
    if (g_toasts.empty())
        return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pane_w = 440.f;

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 18.f, vp->Pos.y + vp->Size.y - 18.f),
        ImGuiCond_Always,
        ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pane_w, 0.f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.13f, 0.14f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.38f, 0.42f, 0.95f));

    ImGui::Begin("##toast_overlay", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::TextDisabled("NOTIFICATIONS");
    ImGui::Separator();
    ImGui::Spacing();

    auto color_for = [](ToastKind k) -> ImVec4 {
        switch (k) {
            case ToastKind::Success:
                return ImVec4(0.45f, 0.90f, 0.52f, 1.0f);
            case ToastKind::Warn:
                return ImVec4(1.0f, 0.82f, 0.42f, 1.0f);
            case ToastKind::Err:
                return ImVec4(1.0f, 0.45f, 0.42f, 1.0f);
            default:
                return ImVec4(0.78f, 0.86f, 1.0f, 1.0f);
        }
    };

    for (size_t ti = 0; ti < g_toasts.size(); ++ti) {
        const Toast& t = g_toasts[ti];
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color_for(t.kind));
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + pane_w - 36.f);
        ImGui::TextUnformatted(t.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (ti + 1 < g_toasts.size())
            ImGui::Spacing();
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

static std::map<ServerInstance*, ServerUIState> g_server_ui_states;

/** Colored scrolling view of llama-server logs (shown in active-models terminal pane). */
static void UiDrawServerLogViewport(ServerInstance* srv) {
    auto& state = g_server_ui_states[srv];

    auto fmt_line = [](const LogEntry& e) {
        return (e.timestamp.empty() ? std::string() : "[" + e.timestamp + "] ")
             + "[" + e.level + "] " + e.message + "\n";
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 5));

    // --- Row 1: view mode + copy actions (kept first so they never clip off) ---
    if (ImGui::Button(state.raw_view ? "Table View" : "Raw View"))
        state.raw_view = !state.raw_view;
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        std::string out;
        for (const auto& e : srv->get_logs()) out += fmt_line(e);
        ImGui::SetClipboardText(out.c_str());
        ToastPush(ToastKind::Success, "All logs copied to clipboard.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Visible")) {
        std::string out;
        for (const auto& e : srv->get_logs())
            if (LogEntryPasses(e, state.filter)) out += fmt_line(e);
        ImGui::SetClipboardText(out.c_str());
        ToastPush(ToastKind::Success, "Visible logs copied to clipboard.");
    }
    ImGui::SameLine();
    bool has_selection = !state.selection.selected_indices.empty();
    if (!has_selection) ImGui::BeginDisabled();
    if (ImGui::Button("Copy Sel")) {
        std::string out;
        auto logs = srv->get_logs();
        for (int idx : state.selection.selected_indices)
            if (idx >= 0 && idx < (int)logs.size()) out += fmt_line(logs[idx]);
        ImGui::SetClipboardText(out.c_str());
        ToastPush(ToastKind::Success, "Selected logs copied to clipboard.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Sel"))
        state.selection.selected_indices.clear();
    if (!has_selection) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Newest first", &state.newest_first);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &state.autoscroll);

    // --- Row 2: filter + level toggles ---
    ImGui::TextDisabled("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui_InputText("##log_filter", &state.filter.filter);
    ImGui::SameLine();
    if (ImGui::Button("Clear##filter")) state.filter.filter.clear();
    ImGui::SameLine();
    ImGui::Checkbox("INFO", &state.filter.show_info);
    ImGui::SameLine();
    ImGui::Checkbox("WARN", &state.filter.show_warn);
    ImGui::SameLine();
    ImGui::Checkbox("ERR", &state.filter.show_err);

    ImGui::PopStyleVar();
    ImGui::Spacing();

    UiDrawLogTable(srv, state);
}

// ============================================================
//  UI: Health banner + functional test + Issues panel
//  Goal: tell the user in one glance whether the server is OK,
//  whether it actually answers requests, and surface real
//  errors without making them read the verbose terminal.
// ============================================================
static void UiDrawHealthPanel(ServerInstance* srv, const ServerMetrics& sm, bool is_stopping) {
    HealthState hs = (HealthState)srv->health_state.load(std::memory_order_relaxed);
    if (is_stopping) hs = HealthState::Stopped;

    const double hms  = srv->last_health_latency_ms.load(std::memory_order_relaxed);
    const int    fails = srv->health_fail_streak.load(std::memory_order_relaxed);

    // --- Plain-English "what is it doing right now" sentence ---
    char activity[192];
    if (is_stopping) {
        snprintf(activity, sizeof activity, "Shutting the server down...");
    } else if (!sm.model_loaded) {
        snprintf(activity, sizeof activity,
                 "Loading the model into memory... %d%%", (int)(sm.current_progress * 100));
    } else if (sm.context_processing) {
        if (sm.context_total > 0)
            snprintf(activity, sizeof activity,
                     "Reading your prompt... (%llu tokens, %d%%)",
                     (unsigned long long)sm.context_total, (int)(sm.context_progress * 100));
        else
            snprintf(activity, sizeof activity, "Reading your prompt...");
    } else if (sm.active_slots > 0 && sm.eval_tps > 0.5f) {
        snprintf(activity, sizeof activity,
                 "Generating a response... (%.0f tokens/sec)", sm.eval_tps);
    } else if (sm.active_slots > 0) {
        snprintf(activity, sizeof activity, "Handling %d request%s right now...",
                 sm.active_slots, sm.active_slots == 1 ? "" : "s");
    } else {
        snprintf(activity, sizeof activity, "Idle - ready and waiting for requests.");
    }

    // --- Map health state to a word, colors and a one-line explanation ---
    const char* word = "STARTING";
    ImVec4 accent(0.70f, 0.80f, 0.90f, 1.0f);
    ImVec4 bg(0.16f, 0.20f, 0.26f, 1.0f);
    std::string sub = "Waiting for the server to come up...";
    switch (hs) {
        case HealthState::Healthy:
            word = "HEALTHY";
            accent = ImVec4(0.25f, 0.95f, 0.45f, 1.0f);
            bg     = ImVec4(0.09f, 0.24f, 0.14f, 1.0f);
            sub = std::string(activity) + "   (health check OK in "
                + std::to_string((int)hms) + " ms)";
            break;
        case HealthState::Loading:
            word = "LOADING";
            accent = ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
            bg     = ImVec4(0.27f, 0.21f, 0.06f, 1.0f);
            sub = activity;
            break;
        case HealthState::Degraded:
            word = "SLOW";
            accent = ImVec4(1.0f, 0.60f, 0.20f, 1.0f);
            bg     = ImVec4(0.30f, 0.17f, 0.05f, 1.0f);
            sub = "Responding, but slowly (" + std::to_string((int)hms)
                + " ms health check) - heavy load or low memory. " + activity;
            break;
        case HealthState::Down:
            word = "NOT RESPONDING";
            accent = ImVec4(1.0f, 0.32f, 0.32f, 1.0f);
            bg     = ImVec4(0.32f, 0.10f, 0.10f, 1.0f);
            sub = "Port " + std::to_string(srv->config.port)
                + " is not answering health checks (" + std::to_string(fails)
                + " failed in a row). It may be stuck loading, crashed, or out of "
                  "memory - check Issues below.";
            break;
        case HealthState::Stopped:
            word = "STOPPED";
            accent = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
            bg     = ImVec4(0.17f, 0.17f, 0.17f, 1.0f);
            sub = "Server is shutting down or has stopped.";
            break;
        default: break; // Unknown -> STARTING (defaults above)
    }

    // --- Banner ---
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::BeginChild("##healthbanner", ImVec2(0, line_h * 4.0f + 12.0f), true,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetWindowPos();
        dl->AddRectFilled(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y + 4),
                          ImColor(accent));
        ImGui::Spacing();
        ImGui::TextColored(accent, "\xe2\x97\x8f  %s", word);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.90f, 0.92f, 1.0f));
        ImGui::TextWrapped("%s", sub.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // --- Functional test (real request) ---
    const bool can_test = sm.model_loaded && !is_stopping
                       && !srv->probe_in_flight.load(std::memory_order_relaxed);
    if (!can_test) ImGui::BeginDisabled();
    if (ImGui::Button("Test Server")) {
        srv->run_probe(false);
        ToastPush(ToastKind::Info, "Sent a test request to the server...");
    }
    if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("Sends one real request and checks the model actually replies.");
        ImGui::EndTooltip();
    }
    if (srv->has_mmproj()) {
        ImGui::SameLine();
        if (ImGui::Button("Test Vision")) {
            srv->run_probe(true);
            ToastPush(ToastKind::Info, "Sent a test image to the vision model...");
        }
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            ImGui::TextUnformatted("Sends a small test image and shows what the model sees.");
            ImGui::EndTooltip();
        }
    }
    if (!can_test) ImGui::EndDisabled();

    ProbeResult pr = srv->probe_result();
    if (pr.state == ProbeState::Running) {
        int dots = (int)(ImGui::GetTime() * 2) % 4;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Testing%.*s",
                           dots, "...");
    }
    if (pr.state == ProbeState::Pass || pr.state == ProbeState::Fail) {
        const bool ok = pr.state == ProbeState::Pass;
        ImVec4 c = ok ? ImVec4(0.30f, 0.92f, 0.45f, 1.0f)
                      : ImVec4(1.0f, 0.36f, 0.36f, 1.0f);
        ImGui::TextColored(c, "%s %s", ok ? "\xe2\x9c\x94" : "\xe2\x9c\x98",
                           pr.summary.c_str());
        if (!pr.detail.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.82f, 0.85f, 1.0f));
            ImGui::TextWrapped("%s", pr.detail.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();

    // --- Issues panel: real errors/warnings, lifted out of the noise ---
    auto issues = srv->get_recent_issues();
    char hdr[48];
    snprintf(hdr, sizeof hdr, "Issues (%d)###issues", (int)issues.size());
    if (!issues.empty())
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.40f, 1.0f));
    bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
    if (!issues.empty())
        ImGui::PopStyleColor();
    if (open) {
        if (issues.empty()) {
            ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.45f, 1.0f),
                               "No errors or warnings - looking clean.");
        } else {
            if (ImGui::SmallButton("Copy issues")) {
                std::string out;
                for (const auto& e : issues)
                    out += "[" + e.level + "] " + e.message + "\n";
                ImGui::SetClipboardText(out.c_str());
                ToastPush(ToastKind::Success, "Issues copied to clipboard.");
            }
            ImGui::BeginChild("##issuelist", ImVec2(0, line_h * 5.0f), true);
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            for (auto it = issues.rbegin(); it != issues.rend(); ++it) {
                ImGui::PushStyleColor(ImGuiCol_Text, it->color);
                ImGui::TextWrapped("[%s] %s", it->level.c_str(), it->message.c_str());
                ImGui::PopStyleColor();
            }
            if (g_mono_font) ImGui::PopFont();
            ImGui::EndChild();
        }
    }
}

static void UiLaunchOverviewTable(const char* id_scope, const LlamaConfig& cfg) {
    ImGui::PushID(id_scope);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.58f, 0.62f, 1.0f));
    ImGui::TextDisabled("DETECTED / CONFIGURED LAUNCH SETTINGS");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (ImGui::BeginTable("launch_ov", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableHeadersRow();

        auto row = [&](const char* key, const char* val) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(key);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
            ImGui::Text("%s", val);
            ImGui::PopTextWrapPos();
        };

        row("Orchestrator port", std::to_string(cfg.port).c_str());
        if (!cfg.exec_path.empty())
            row("Executable", cfg.exec_path.c_str());
        if (!cfg.model_path.empty())
            row("Model (.gguf)", cfg.model_path.c_str());

        for (const auto& kv : LlamaManager::summarize_launch_args(cfg.custom_args))
            row(kv.first.c_str(), kv.second.c_str());

        ImGui::EndTable();
    }
    ImGui::PopID();
}

// ============================================================
//  THEME
// ============================================================
static void BeginMetricCard(const char* title, const ImVec4& accent_color = ImVec4(0.4f, 0.7f, 1.0f, 1.0f)) {
    ImGui::BeginChild(title, ImVec2(0, 0), true);
    // Draw a small accent bar at the top
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 p_min = ImGui::GetWindowPos();
    ImVec2 p_max = ImVec2(p_min.x + ImGui::GetWindowWidth(), p_min.y + 3);
    draw->AddRectFilled(p_min, p_max, ImColor(accent_color));
    
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, accent_color);
    ImGui::TextDisabled("%s", title);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

static void EndMetricCard() {
    ImGui::EndChild();
}

// ============================================================
//  UI: Themed progress bar with color gradient
// ============================================================
static void ColoredProgressBar(float fraction, const ImVec2& size_arg, const char* overlay,
                                const ImVec4& color_low, const ImVec4& color_high) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size = size_arg;
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x + size.x;
    if (size.y <= 0) size.y = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;

    ImVec2 max_pos(pos.x + size.x, pos.y + size.y);
    ImGui::Dummy(size);

    // Interpolate color
    ImVec4 col4;
    col4.x = color_low.x + (color_high.x - color_low.x) * fraction;
    col4.y = color_low.y + (color_high.y - color_low.y) * fraction;
    col4.z = color_low.z + (color_high.z - color_low.z) * fraction;
    col4.w = 1.0f;
    ImU32 col    = ImGui::ColorConvertFloat4ToU32(col4);
    ImU32 bg_col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.12f, 0.12f, 0.15f, 1.0f));

    float rounding = size.y * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background pill
    dl->AddRectFilled(pos, max_pos, bg_col, rounding);

    // Fill pill
    float fill_w = size.x * fraction;
    fill_w = std::max(fill_w, rounding * 2.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + fill_w, max_pos.y), col, rounding);

    // Overlay text with shadow for readability
    if (overlay && overlay[0]) {
        ImVec2 text_size = ImGui::CalcTextSize(overlay);
        float text_x = pos.x + std::max(6.0f, (size.x - text_size.x) * 0.5f);
        ImVec2 text_pos(text_x, pos.y + (size.y - text_size.y) * 0.5f);
        dl->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), IM_COL32(0, 0, 0, 180), overlay);
        dl->AddText(text_pos, IM_COL32(255, 255, 255, 255), overlay);
    }
}

// ============================================================
//  Toast notifications (floating overlay)
// ============================================================



// ============================================================
//  THEME
// ============================================================
void ApplyMinimalistTheme() {
}

// ============================================================
//  MAIN
// ============================================================
int main() {

    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1480, 900, "LlamaFlow Orchestrator", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImFont* font = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Supplemental/Arial.ttf", 18.0f);
    if (!font) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 18.0f);
    }
    if (!font) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1.4f;
    }

    // Monospace font for the log terminal (aligned columns + IDE feel).
    g_mono_font = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Menlo.ttc", 15.0f);
    if (!g_mono_font)
        g_mono_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);

    ApplyMinimalistTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    int logo_w = 0, logo_h = 0;
    GLuint logo_tex = LoadEmbeddedLogoTexture(&logo_w, &logo_h);

#ifdef __APPLE__
    SetMacDockIcon(_Users_apple_code_per_LlamaFlow_logo_png, _Users_apple_code_per_LlamaFlow_logo_png_len);
#endif

    LlamaManager manager;
    manager.load_configs();
    manager.attach_all();   // auto-detect externally-running servers
    int selected_config_idx = -1;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        manager.drain_async_stop_completions();

        static int inference_tab_selected = 0;
        if (!manager.running_servers.empty()) {
            if (inference_tab_selected >= (int)manager.running_servers.size())
                inference_tab_selected = (int)manager.running_servers.size() - 1;
        } else {
            inference_tab_selected = 0;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Main", nullptr, 
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | 
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar);

        // ============================================================
        //  LEFT SIDEBAR: CONFIGURATION LIST
        // ============================================================
        ImGui::BeginChild("Sidebar", ImVec2(320, -30), true);

        // Logo at top of sidebar
        if (logo_tex) {
            float display_w = 64.0f;
            float aspect = logo_h ? (float)logo_w / (float)logo_h : 1.0f;
            float display_h = display_w / aspect;
            float offset_x = (ImGui::GetContentRegionAvail().x - display_w) * 0.5f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
            ImGui::Image((ImTextureID)(uintptr_t)logo_tex, ImVec2(display_w, display_h));
            ImGui::Spacing();
        }
        
        ImGui::TextDisabled("SAVED MODELS");
        ImGui::Spacing();

        if (ImGui::Button("+ Add New Model", ImVec2(-1, 36))) {
            manager.saved_configs.push_back(LlamaConfig());
            selected_config_idx = manager.saved_configs.size() - 1;
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        
        for (int i = 0; i < manager.saved_configs.size(); ++i) {
            bool is_selected = (selected_config_idx == i);
            
            std::string display_name = manager.saved_configs[i].name;
            if (display_name.empty()) {
                display_name = "<Unnamed>";
            }
            
            std::string selectable_id = display_name + "##" + std::to_string(i);
            
            if (ImGui::Selectable(selectable_id.c_str(), is_selected, 0, ImVec2(0, 28))) {
                selected_config_idx = i;
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ============================================================
        //  MAIN CONTENT: config editor (no active-nodes — they are in the right sidebar)
        //  RIGHT SIDEBAR: active models panel added below if any running
        // ============================================================
        static float g_active_panel_width = 520.f;
        const bool has_active = !manager.running_servers.empty();
        const float right_w = has_active ? g_active_panel_width : 0.f;

        ImGui::BeginChild("Content", ImVec2(-right_w, -30), false);

        LlamaConfig* active_cfg = nullptr;
        if (selected_config_idx >= 0 && selected_config_idx < (int)manager.saved_configs.size())
            active_cfg = &manager.saved_configs[selected_config_idx];

        if (active_cfg) {
            ImGui::BeginChild("TopActions", ImVec2(0, 44), false);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "CONFIG: %s", active_cfg->name.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 330);
            
            if (ImGui::Button("Save Changes", ImVec2(150, 30))) {
                manager.save_configs();
                ToastPush(ToastKind::Success, "Configuration saved to configs.json.");
            }
            ImGui::SameLine();
            
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.60f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.70f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.50f, 0.25f, 1.0f));
            if (ImGui::Button("START SERVER", ImVec2(160, 30))) {
                manager.last_launch_error.clear();
                if (!manager.launch_server(*active_cfg)) {
                    std::string err = manager.last_launch_error.empty() ? std::string("Launch failed.") : manager.last_launch_error;
                    ToastPush(ToastKind::Err, err);
                } else {
                    ToastPush(ToastKind::Success,
                        "Server process started — watch MODEL LOADER progress below once the inference tab opens.");
                }
            }
            ImGui::PopStyleColor(3);
            
            if (!manager.last_launch_error.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextDisabled("⚠  Launch Failed");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", manager.last_launch_error.c_str());
                    ImGui::EndTooltip();
                }
                ImGui::PopStyleColor();
            }

            ImGui::EndChild();

            ImGui::Separator(); 
            ImGui::Spacing();
        }

        // One vertical scroll for config editor; stretches to fill Content.
        ImGui::BeginChild("RightScroll", ImVec2(0, -1), true);

        if (active_cfg) {
            LlamaConfig& cfg = *active_cfg;

            constexpr float CUSTOM_ARGS_LINES_PX = 220.f;

            ImGui::TextDisabled("LAUNCH PARAMETERS");
            ImGui::Spacing();

            ImGui::Text("Model Name Alias");
            ImGui::PushItemWidth(-1);
            ImGui_InputText("##Name", &cfg.name);
            if (ImGui::IsItemDeactivatedAfterEdit())
                ToastPush(ToastKind::Info, "Name updated locally — press Save Changes to persist.");
            ImGui::PopItemWidth();
            ImGui::Spacing();

            ImGui::Text("Executable Path (llama-server)");
            ImGui::PushItemWidth(-1);
            ImGui_InputText("##ExecPath", &cfg.exec_path);
            if (ImGui::IsItemDeactivatedAfterEdit())
                ToastPush(ToastKind::Info, "Executable path updated locally — press Save Changes to persist.");
            ImGui::PopItemWidth();
            ImGui::Spacing();

            ImGui::Text("Model File (.gguf path)");
            ImGui::PushItemWidth(-1);
            ImGui_InputText("##ModelPath", &cfg.model_path);
            if (ImGui::IsItemDeactivatedAfterEdit())
                ToastPush(ToastKind::Info, "Model path updated locally — press Save Changes to persist.");
            ImGui::PopItemWidth();
            ImGui::Spacing();

            ImGui::Text("Network Port");
            ImGui::PushItemWidth(150);
            if (ImGui::InputInt("##Port", &cfg.port)) {
                if (cfg.port < 1) cfg.port = 1;
                if (cfg.port > 65535) cfg.port = 65535;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                ToastPush(ToastKind::Info, "Port updated locally — press Save Changes to persist.");
            ImGui::PopItemWidth();

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextDisabled("CUSTOM ARGUMENTS");
            ImGui::TextWrapped("Passes through to llama-server after -m / --port. This panel scrolls with the page.");
            ImGui::Spacing();
            ImGui_InputTextMultiline("##Args", &cfg.custom_args, ImVec2(-1, CUSTOM_ARGS_LINES_PX),
                ImGuiInputTextFlags_AllowTabInput);
            if (ImGui::IsItemDeactivatedAfterEdit())
                ToastPush(ToastKind::Info, "Custom arguments updated locally — press Save Changes.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader("Read-only: effective flags (port, paths, parsed custom args)")) {
                UiLaunchOverviewTable("editor_profile", cfg);
            }
        } else {
            ImGui::TextDisabled("Select a configuration from the sidebar to begin.");
        }


        ImGui::EndChild(); // RightScroll
        
        ImGui::EndChild(); // End Content

        // ============================================================
        //  RIGHT SIDEBAR: ACTIVE MODELS (separate from config editor)
        // ============================================================
        if (has_active) {
            ImGui::SameLine();

            // Vertical splitter handle (draggable to resize right panel)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.35f, 0.40f, 1.0f));
            ImGui::Button("##vsplit", ImVec2(8, -30));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                g_active_panel_width -= ImGui::GetMouseDragDelta(0).x;
                if (g_active_panel_width < 380.f) g_active_panel_width = 380.f;
                if (g_active_panel_width > 900.f) g_active_panel_width = 900.f;
                ImGui::ResetMouseDragDelta(0);
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();

            // Active models panel (no outer scrollbar — inner terminal has its own)
            ImGui::BeginChild("ActivePanel", ImVec2(0, -30), true, ImGuiWindowFlags_NoScrollbar);

            // Header with live count
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "ACTIVE MODELS");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::TextDisabled("  (%zu running)", manager.running_servers.size());
            ImGui::PopStyleColor();
            ImGui::Spacing();

            // Tab bar — one tab per running server
            if (ImGui::BeginTabBar("ActiveTabBar", ImGuiTabBarFlags_FittingPolicyScroll)) {
                for (int i = 0; i < (int)manager.running_servers.size(); ++i) {
                    ServerInstance* srv = manager.running_servers[i];
                    std::string display = srv->config.name.empty()
                        ? "Port " + std::to_string(srv->config.port)
                        : srv->config.name;
                    std::string tab_id = display + "##act" + std::to_string(i);

                    bool is_stopping = srv->stop_requested.load();
                    ImVec4 tab_color = is_stopping
                        ? ImVec4(0.55f, 0.25f, 0.15f, 1.0f)
                        : ImVec4(0.15f, 0.55f, 0.25f, 1.0f);

                    ImGui::PushStyleColor(ImGuiCol_TabActive, tab_color);
                    ImGui::PushStyleColor(ImGuiCol_TabHovered,
                        ImVec4(tab_color.x + 0.1f, tab_color.y + 0.1f, tab_color.z + 0.1f, 1.0f));

                    if (ImGui::BeginTabItem(tab_id.c_str())) {
                        ImGui::PopStyleColor(2);
                        inference_tab_selected = i;
                        ImGui::PushID(srv);

                        ServerMetrics sm = srv->snapshot();

                        // --- Status bar + stop button ---
                        if (is_stopping) {
                            ImGui::BeginDisabled();
                            ImGui::Button("Stopping...", ImVec2(120, 24));
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.82f, 0.52f, 1.0f));
                            ImGui::TextWrapped("%s", srv->shutdown_status_text());
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.15f, 0.15f, 1.0f));
                            if (ImGui::Button("Stop", ImVec2(70, 24))) {
                                ImGui::PopStyleColor(3);
                                manager.request_stop_server(srv);
                                ToastPush(ToastKind::Info, "Shutdown queued — visible until process exits.");
                            } else {
                                ImGui::PopStyleColor(3);
                            }
                            ImGui::SameLine();

                            char state_str[64] = "Running";
                            if (!sm.model_loaded)
                                snprintf(state_str, sizeof(state_str), "Loading");
                            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "\xe2\x97\x8f %s", state_str);
                            ImGui::SameLine();
                            ImGui::TextDisabled("| %s",
                                format_duration(sm.uptime_seconds).c_str());
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // --- At-a-glance health, real request test, Issues ---
                        UiDrawHealthPanel(srv, sm, is_stopping);

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // --- Compact metrics: 2x2 grid (fits narrow panel) ---
                        float avail_w = ImGui::GetContentRegionAvail().x;
                        float half_w = (avail_w - 6) / 2.0f;

                        // Dynamic card heights based on actual content
                        const float line_h  = ImGui::GetTextLineHeightWithSpacing();
                        const float pad_y   = ImGui::GetStyle().WindowPadding.y;
                        const float space_y = ImGui::GetStyle().ItemSpacing.y;
                        auto calc_h = [&](int n_lines) {
                            return 3.0f + space_y + line_h * n_lines + pad_y * 2.0f;
                        };

                        float row1_h = calc_h(2); // throughput + tokens: always 2 lines
                        float rq_h   = calc_h(sm.n_slots > 0 ? 3 : 2);
                        float sys_h  = calc_h(2);
                        float row2_h = rq_h > sys_h ? rq_h : sys_h; // align row 2

                        // Row 1
                        ImGui::BeginChild("##tp_card", ImVec2(half_w, row1_h), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            ImVec2 p0 = ImGui::GetWindowPos();
                            dl->AddRectFilled(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y + 3), ImColor(0.3f, 0.7f, 1.0f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("THROUGHPUT");
                            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "P:%.1f  G:%.1f t/s",
                                sm.prompt_eval_tps, sm.eval_tps);
                        }
                        ImGui::EndChild();
                        ImGui::SameLine();
                        ImGui::BeginChild("##tk_card", ImVec2(half_w, row1_h), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            ImVec2 p0 = ImGui::GetWindowPos();
                            dl->AddRectFilled(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y + 3), ImColor(1.0f, 0.7f, 0.2f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("TOKENS");
                            ImGui::Text("P:%s  G:%s", format_number(sm.prompt_tokens).c_str(),
                                format_number(sm.generated_tokens).c_str());
                        }
                        ImGui::EndChild();

                        // Row 2
                        ImGui::BeginChild("##rq_card", ImVec2(half_w, row2_h), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            ImVec2 p0 = ImGui::GetWindowPos();
                            dl->AddRectFilled(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y + 3), ImColor(0.8f, 0.3f, 0.8f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("REQUESTS");
                            ImGui::Text("Done:%s", format_number(sm.completed_requests).c_str());
                            if (sm.n_slots > 0)
                                ImGui::Text("Slots:%d/%d", sm.active_slots, sm.n_slots);
                        }
                        ImGui::EndChild();
                        ImGui::SameLine();
                        ImGui::BeginChild("##sys_card", ImVec2(half_w, row2_h), true, ImGuiWindowFlags_NoScrollbar);
                        {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            ImVec2 p0 = ImGui::GetWindowPos();
                            dl->AddRectFilled(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y + 3), ImColor(0.2f, 0.8f, 0.5f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("SYSTEM");
                            if (sm.model_memory_bytes > 0)
                                ImGui::Text("VRAM:%s", format_bytes(sm.model_memory_bytes).c_str());
                            else
                                ImGui::Text("Port:%d", srv->config.port);
                        }
                        ImGui::EndChild();

                        ImGui::Spacing();

                        // --- Model loader / context progress (compact) ---
                        if (!is_stopping) {
                            if (!sm.model_loaded) {
                                char buf[64];
                                snprintf(buf, sizeof(buf), "Loading: %d%%", (int)(sm.current_progress * 100));
                                ColoredProgressBar(sm.current_progress, ImVec2(-1, 18), buf,
                                    ImVec4(0.85f, 0.38f, 0.15f, 1.0f), ImVec4(0.15f, 0.82f, 0.45f, 1.0f));
                            }
                            if (sm.context_processing) {
                                char buf[64];
                                snprintf(buf, sizeof(buf), "Context: %d%%", (int)(sm.context_progress * 100));
                                ColoredProgressBar(sm.context_progress, ImVec2(-1, 18), buf,
                                    ImVec4(0.22f, 0.42f, 0.92f, 1.0f), ImVec4(0.35f, 0.95f, 0.40f, 1.0f));
                            }
                            if (sm.kv_cache_usage > 0.0f) {
                                char buf[48];
                                snprintf(buf, sizeof(buf), "KV: %d%%", (int)(sm.kv_cache_usage * 100));
                                ColoredProgressBar(sm.kv_cache_usage, ImVec2(-1, 14), buf,
                                    ImVec4(0.2f, 0.7f, 0.3f, 1.0f), ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                            }
                            ImGui::Spacing();
                        }

                        // --- Resizable terminal splitter ---
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Terminal label
                        ImGui::TextColored(ImVec4(0.72f, 0.92f, 1.0f, 1.0f), "TERMINAL");
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", srv->config.name.c_str());
                        ImGui::SameLine();
                        if (ImGui::Button("Maximize")) {
                            g_maximize_logs = true;
                            g_maximized_srv = srv;
                        }

                        // Calculate terminal height: remaining space in panel minus little margin
                        float remaining_y = ImGui::GetContentRegionAvail().y;
                        static float term_ratio = 0.55f;
                        float term_h = remaining_y * term_ratio;
                        if (term_h < 70.f)  term_h = 70.f;
                        if (term_h > remaining_y - 10.f) term_h = remaining_y - 10.f;

                        // Terminal log viewport (has its own internal scroll)
                        ImGui::BeginChild("TerminalLog", ImVec2(-1, term_h), true,
                            ImGuiWindowFlags_NoScrollbar);
                        UiDrawServerLogViewport(srv);
                        ImGui::EndChild();

                        // Horizontal splitter handle (drag to resize terminal height)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.30f, 0.35f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.35f, 0.40f, 1.0f));
                        ImGui::Button("##hsplit", ImVec2(-1, 5));
                        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                            term_ratio += ImGui::GetMouseDragDelta(0).y / remaining_y;
                            if (term_ratio < 0.15f) term_ratio = 0.15f;
                            if (term_ratio > 0.85f) term_ratio = 0.85f;
                            ImGui::ResetMouseDragDelta(0);
                        }
                        ImGui::PopStyleColor(3);

                        ImGui::PopID();
                        ImGui::EndTabItem();
                    } else {
                        ImGui::PopStyleColor(2);
                    }
                }
                ImGui::EndTabBar();
            }

            ImGui::EndChild(); // ActivePanel
        }

        ToastDecayAndOverlay();

        if (g_maximize_logs && g_maximized_srv) {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), 
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.8f, vp->Size.y * 0.8f));
            
            if (ImGui::Begin("Log Viewer", &g_maximize_logs, ImGuiWindowFlags_NoCollapse)) {
                if (ImGui::Button("Close")) {
                    g_maximize_logs = false;
                    g_maximized_srv = nullptr;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Log output from maximized server");
                ImGui::Separator();
                ImGui::Spacing();
                
                UiDrawServerLogViewport(g_maximized_srv);
                
                ImGui::End();
            }
            if (!g_maximize_logs) g_maximized_srv = nullptr;
        }


        // ============================================================
        //  BOTTOM STATUS BAR
        // ============================================================
        ImGui::BeginChild("StatusBar", ImVec2(0, 0), false);
        {
            // Left: server count
            ImGui::TextDisabled("Nodes: ");
            ImGui::SameLine();
            if (manager.running_servers.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "0 active");
            } else {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%zu active", manager.running_servers.size());
            }
            
            // Aggregate metrics across all servers
            uint64_t total_tokens = 0;
            uint64_t total_requests = 0;
            double total_uptime = 0;
            for (auto* srv : manager.running_servers) {
                ServerMetrics sm = srv->snapshot();
                total_tokens += sm.total_tokens_processed;
                total_requests += sm.completed_requests;
                total_uptime += sm.uptime_seconds;
            }
            
            ImGui::SameLine(200);
            ImGui::TextDisabled("Tokens: ");
            ImGui::SameLine();
            ImGui::Text("%s", format_number(total_tokens).c_str());
            
            ImGui::SameLine(340);
            ImGui::TextDisabled("Requests: ");
            ImGui::SameLine();
            ImGui::Text("%s", format_number(total_requests).c_str());
            
            ImGui::SameLine(480);
            ImGui::TextDisabled("Total Uptime: ");
            ImGui::SameLine();
            ImGui::Text("%s", format_duration(total_uptime).c_str());

            // Right side: config count
            ImGui::SameLine(ImGui::GetWindowWidth() - 180);
            ImGui::TextDisabled("Configs: %zu", manager.saved_configs.size());
        }
        ImGui::EndChild();

        ImGui::End(); // End Main Window

        // --- Render Frame ---
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (logo_tex)
        glDeleteTextures(1, &logo_tex);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
