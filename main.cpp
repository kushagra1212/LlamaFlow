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

#include <cstdio>

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

/** Colored scrolling view of llama-server logs (shown in MODEL TERMINAL pane). */
static void UiDrawServerLogViewport(ServerInstance* srv) {
    if (!srv)
        return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    ImGui::BeginChild("LogViewport", ImVec2(-1, -1), false, ImGuiWindowFlags_HorizontalScrollbar);

    std::deque<std::string> log_snapshot = srv->get_logs();
    for (const auto& line : log_snapshot) {
        if (line.find("error") != std::string::npos || line.find("fail") != std::string::npos ||
            line.find("FAILED") != std::string::npos) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", line.c_str());
        } else if (line.find("print_timings") != std::string::npos) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "%s", line.c_str());
        } else if (line.find("slot") != std::string::npos) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "%s", line.c_str());
        } else {
            ImGui::TextUnformatted(line.c_str());
        }
    }

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================
//  Toast notifications (floating overlay)
// ============================================================
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

static void ToastDecayAndOverlay() {
    const double now = ImGui::GetTime();
    while (!g_toasts.empty() && g_toasts.front().expires_at < now)
        g_toasts.pop_front();
    if (g_toasts.empty())
        return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pane_w = 440.f;

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 18.f, vp->Pos.y + 78.f),
        ImGuiCond_Always,
        ImVec2(1.0f, 0.0f));
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
static void ColoredProgressBar(float fraction, const ImVec2& size, const char* overlay, 
                                const ImVec4& color_low, const ImVec4& color_high) {
    // Interpolate color based on fraction
    ImVec4 color;
    color.x = color_low.x + (color_high.x - color_low.x) * fraction;
    color.y = color_low.y + (color_high.y - color_low.y) * fraction;
    color.z = color_low.z + (color_high.z - color_low.z) * fraction;
    color.w = 1.0f;
    
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(fraction, size, overlay);
    ImGui::PopStyleColor();
}

// ============================================================
//  THEME
// ============================================================
void ApplyMinimalistTheme() {
    ImGuiStyle* style = &ImGui::GetStyle();
    ImVec4* colors = style->Colors;

    // Structural Geometry
    style->WindowRounding = 8.0f;
    style->FrameRounding = 6.0f;
    style->GrabRounding = 6.0f;
    style->PopupRounding = 6.0f;
    style->ChildRounding = 8.0f;
    style->TabRounding = 6.0f;
    
    style->WindowBorderSize = 0.0f;
    style->FrameBorderSize = 0.0f;
    style->ChildBorderSize = 1.0f;
    style->TabBorderSize = 0.0f;
    
    style->ItemSpacing = ImVec2(8, 10);
    style->ItemInnerSpacing = ImVec2(6, 6);
    style->ScrollbarSize = 8.0f;
    style->ScrollbarRounding = 4.0f;
    
    style->Colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    style->Colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    style->Colors[ImGuiCol_WindowBg]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    style->Colors[ImGuiCol_ChildBg]                = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    style->Colors[ImGuiCol_PopupBg]                = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    style->Colors[ImGuiCol_Border]                 = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    style->Colors[ImGuiCol_FrameBg]                = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    style->Colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    style->Colors[ImGuiCol_FrameBgActive]          = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
    style->Colors[ImGuiCol_TitleBg]                = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    style->Colors[ImGuiCol_TitleBgActive]          = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
    style->Colors[ImGuiCol_MenuBarBg]              = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.30f, 0.30f, 0.32f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.40f, 0.40f, 0.42f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);
    style->Colors[ImGuiCol_CheckMark]              = ImVec4(0.40f, 0.80f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_SliderGrab]             = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.50f, 0.80f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_Button]                 = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    style->Colors[ImGuiCol_ButtonHovered]          = ImVec4(0.33f, 0.33f, 0.35f, 1.00f);
    style->Colors[ImGuiCol_ButtonActive]           = ImVec4(0.43f, 0.43f, 0.45f, 1.00f);
    style->Colors[ImGuiCol_Header]                 = ImVec4(0.25f, 0.28f, 0.32f, 1.00f);
    style->Colors[ImGuiCol_HeaderHovered]          = ImVec4(0.30f, 0.33f, 0.38f, 1.00f);
    style->Colors[ImGuiCol_HeaderActive]           = ImVec4(0.35f, 0.38f, 0.43f, 1.00f);
    style->Colors[ImGuiCol_Separator]              = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    style->Colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.30f, 0.30f, 0.32f, 1.00f);
    style->Colors[ImGuiCol_SeparatorActive]        = ImVec4(0.40f, 0.40f, 0.42f, 1.00f);
    style->Colors[ImGuiCol_ResizeGrip]             = ImVec4(0.30f, 0.30f, 0.32f, 1.00f);
    style->Colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.40f, 0.40f, 0.42f, 1.00f);
    style->Colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);
    style->Colors[ImGuiCol_Tab]                    = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style->Colors[ImGuiCol_TabHovered]             = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    style->Colors[ImGuiCol_TabActive]              = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    style->Colors[ImGuiCol_TabUnfocused]           = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style->Colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    style->Colors[ImGuiCol_PlotLines]              = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_PlotLinesHovered]       = ImVec4(0.60f, 0.80f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_PlotHistogram]          = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(0.60f, 0.80f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.25f, 0.50f, 0.80f, 1.00f);
    style->Colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.05f, 0.05f, 0.05f, 0.60f);
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
    
    ApplyMinimalistTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

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
        //  RIGHT PANEL: sticky actions + single scroll (config + inference)
        // ============================================================
        constexpr float kModelTerminalStripPx = 312.f;

        ImGui::BeginChild("Content", ImVec2(0, -30), false);

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

        const bool has_inference_nodes = !manager.running_servers.empty();
        const float reserve_model_terminal =
            has_inference_nodes ? (kModelTerminalStripPx + 10.f) : 0.f;

        // One vertical scroll for editor + inference; when no terminal dock, stretch to fill Content.
        const float right_scroll_height = reserve_model_terminal > 0.f ? -reserve_model_terminal : -1.f;
        ImGui::BeginChild("RightScroll", ImVec2(0, right_scroll_height), true);

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

        // ============================================================
        //  ACTIVE INFERENCE NODES SECTION
        // ============================================================
        if (!manager.running_servers.empty()) {
            ImGui::Spacing();
            
            // Section header with count badge
            ImGui::TextDisabled("ACTIVE INFERENCE NODES");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::Text("  (%zu running)", manager.running_servers.size());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            
            if (ImGui::BeginTabBar("ServersTab", ImGuiTabBarFlags_FittingPolicyScroll)) {
                for (int i = 0; i < manager.running_servers.size(); ++i) {
                    ServerInstance* srv = manager.running_servers[i];
                    std::string tab_name = srv->config.name + " (Port " + std::to_string(srv->config.port) + ")";
                    
                    // Green dot indicator in tab
                    std::string tab_id = tab_name + "##srv" + std::to_string(i);
                    if (ImGui::BeginTabItem(tab_id.c_str())) {
                        inference_tab_selected = i;

                        ImGui::PushID(srv);

                        // --- Stop / shutdown ---
                        if (srv->stop_requested.load()) {
                            ImGui::BeginDisabled();
                            ImGui::Button("Stopping server…", ImVec2(200, 30));
                            ImGui::EndDisabled();
                            char elapsed_line[128];
                            snprintf(elapsed_line, sizeof(elapsed_line),
                                "Elapsed %.1f s  —  unloading the model or closing listeners can take several seconds…",
                                srv->stop_elapsed_seconds());
                            ImGui::TextDisabled("%s", elapsed_line);
                            const char* st = srv->shutdown_status_text();
                            if (st && st[0]) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.82f, 0.52f, 1.0f));
                                ImGui::TextWrapped("%s", st);
                                ImGui::PopStyleColor();
                            } else {
                                ImGui::TextDisabled("Preparing shutdown...");
                            }
                            float pulse = static_cast<float>(std::fmod(ImGui::GetTime() * 0.9, 2.0));
                            float frac = (pulse < 1.0f) ? pulse : (2.0f - pulse);
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.55f, 0.38f, 0.18f, 0.9f));
                            ImGui::ProgressBar(frac, ImVec2(-1, 7), " ");
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.15f, 0.15f, 1.0f));
                            if (ImGui::Button("Stop server", ImVec2(200, 30))) {
                                ImGui::PopStyleColor(3);
                                manager.request_stop_server(srv);
                                ToastPush(ToastKind::Info, "Shutdown queued — this tab stays visible until the process exits.");
                            } else {
                                ImGui::PopStyleColor(3);
                            }
                            ImGui::SameLine();
                            ImGui::TextDisabled("Stops llama-server (including processes started outside LlamaFlow).");
                        }

                        ServerMetrics sm = srv->snapshot();

                        char pid_frag[144] = "?";
                        if (srv->process_id > 0)
                            snprintf(pid_frag, sizeof(pid_frag), "child PID %d", srv->process_id);
                        else if (srv->attached && srv->external_listen_pid > 0)
                            snprintf(pid_frag, sizeof(pid_frag), "external PID %d", (int)srv->external_listen_pid);
                        else if (srv->attached)
                            snprintf(pid_frag, sizeof(pid_frag), "external (PID from lsof on stop)");

                        char state_frag[96] = "Running";
                        if (srv->stop_requested.load())
                            snprintf(state_frag, sizeof(state_frag), "Stopping");
                        else if (!sm.model_loaded)
                            snprintf(state_frag, sizeof(state_frag), "Loading model");
                        else
                            snprintf(state_frag, sizeof(state_frag), "Running");

                        char status_banner[896];
                        snprintf(status_banner, sizeof(status_banner),
                            "Status: %s  ·  %s  ·  Uptime %s%s%s",
                            state_frag,
                            pid_frag,
                            format_duration(sm.uptime_seconds).c_str(),
                            sm.context_processing ? "  ·  Prompt/context ingest active" : "",
                            sm.eval_tps > 1.0f ? "  ·  Generating" : "");

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.9f, 0.93f, 1.0f));
                        ImGui::TextWrapped("%s", status_banner);
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0.f, ImGui::GetStyle().FramePadding.y + 2.f));

                        ImGui::Spacing();
                        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                        if (ImGui::CollapsingHeader("Launch settings summary (parsed from paths + custom args)", ImGuiTreeNodeFlags_None)) {
                            UiLaunchOverviewTable("active_node", srv->config);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // --- MODEL LOADER (only for processes spawned by LlamaFlow) ---
                        if (srv->stop_requested.load()) {
                            ImGui::TextDisabled("Loader hidden during shutdown.");
                        } else if (srv->attached) {
                            ImGui::TextDisabled(
                                "ATTACHED NODE: LlamaFlow did not spawn this llama-server, so loader output may be missing unless "
                                "this app started it earlier — check Activity Monitor / terminal for load progress.");
                        } else if (!sm.model_loaded) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.25f, 1.0f));
                            ImGui::TextUnformatted("MODEL LOADER");
                            ImGui::PopStyleColor();
                            ImGui::TextWrapped("Loading tensors and building KV / context — large models can spend minutes here.");

                            char prog_overlay[96];
                            if (sm.current_progress > 0.01f) {
                                snprintf(prog_overlay, sizeof(prog_overlay), "Weights & context initialization — %d%%",
                                    (int)(sm.current_progress * 100));
                            } else {
                                snprintf(prog_overlay, sizeof(prog_overlay),
                                    "Waiting for first progress lines in server log...");
                            }
                            ColoredProgressBar(sm.current_progress, ImVec2(-1.0f, 30), prog_overlay,
                                ImVec4(0.85f, 0.38f, 0.15f, 1.0f),
                                ImVec4(0.15f, 0.82f, 0.45f, 1.0f));
                            ImGui::Spacing();
                        } else {
                            ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1.0f), "Model ready.");
                            ImGui::Spacing();
                        }

                        // --- PROMPT / CONTEXT ingestion (streaming batches into KV) ---
                        if (!srv->stop_requested.load() && sm.context_processing) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.76f, 1.0f, 1.0f));
                            ImGui::TextUnformatted("PROMPT / CONTEXT PIPELINE");
                            ImGui::PopStyleColor();
                            ImGui::TextWrapped(
                                "Matches llama-server log lines like «prompt processing progress» — embeddings + KV fills.");
                            char ctx_overlay[96];
                            int ctx_pct = (int)(sm.context_progress * 100);
                            if (sm.context_total > 0) {
                                snprintf(ctx_overlay, sizeof(ctx_overlay), "%d%% ingested (~%s prompt tokens)",
                                    ctx_pct, format_number(sm.context_total).c_str());
                            } else {
                                snprintf(ctx_overlay, sizeof(ctx_overlay), "%d%% ingested", ctx_pct);
                            }
                            ColoredProgressBar(sm.context_progress, ImVec2(-1.0f, 28), ctx_overlay,
                                ImVec4(0.22f, 0.42f, 0.92f, 1.0f),
                                ImVec4(0.35f, 0.95f, 0.40f, 1.0f));
                            ImGui::Spacing();
                        }

                        ImGui::Separator();
                        ImGui::Spacing();

                        // ============================================================
                        //  METRICS DASHBOARD
                        // ============================================================

                        // --- Row 2: 4 metric cards side by side ---
                        float card_w = (ImGui::GetContentRegionAvail().x - 24) / 4.0f;  // 3 gaps
                        const float metric_card_height = (std::max)(158.f, ImGui::GetFontSize() * 9.5f);

                        // Card 1: Throughput
                        ImGui::BeginChild("##card_throughput", ImVec2(card_w, metric_card_height), true,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                        {
                            ImDrawList* draw = ImGui::GetWindowDrawList();
                            ImVec2 p_min = ImGui::GetWindowPos();
                            ImVec2 p_max = ImVec2(p_min.x + ImGui::GetWindowWidth(), p_min.y + 3);
                            draw->AddRectFilled(p_min, p_max, ImColor(0.3f, 0.7f, 1.0f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("THROUGHPUT");
                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Prompt:  %.1f t/s", sm.prompt_eval_tps);
                            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Gen:    %.1f t/s", sm.eval_tps);
                        }
                        ImGui::EndChild();
                        ImGui::SameLine();

                        // Card 2: Token Counters
                        ImGui::BeginChild("##card_tokens", ImVec2(card_w, metric_card_height), true,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                        {
                            ImDrawList* draw = ImGui::GetWindowDrawList();
                            ImVec2 p_min = ImGui::GetWindowPos();
                            ImVec2 p_max = ImVec2(p_min.x + ImGui::GetWindowWidth(), p_min.y + 3);
                            draw->AddRectFilled(p_min, p_max, ImColor(1.0f, 0.7f, 0.2f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("TOKENS");
                            ImGui::Spacing();
                            ImGui::Text("Prompt:     %s", format_number(sm.prompt_tokens).c_str());
                            ImGui::Text("Generated:  %s", format_number(sm.generated_tokens).c_str());
                            ImGui::Text("Total:      %s", format_number(sm.total_tokens_processed).c_str());
                        }
                        ImGui::EndChild();
                        ImGui::SameLine();

                        // Card 3: Requests & Slots
                        ImGui::BeginChild("##card_requests", ImVec2(card_w, metric_card_height), true,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                        {
                            ImDrawList* draw = ImGui::GetWindowDrawList();
                            ImVec2 p_min = ImGui::GetWindowPos();
                            ImVec2 p_max = ImVec2(p_min.x + ImGui::GetWindowWidth(), p_min.y + 3);
                            draw->AddRectFilled(p_min, p_max, ImColor(0.8f, 0.3f, 0.8f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("REQUESTS");
                            ImGui::Spacing();
                            ImGui::Text("Total:      %s", format_number(sm.total_requests).c_str());
                            ImGui::Text("Completed:  %s", format_number(sm.completed_requests).c_str());
                            // Active slots bar
                            float slot_frac = (sm.n_slots > 0) ? (float)sm.active_slots / (float)sm.n_slots : 0.0f;
                            char slot_buf[64];
                            snprintf(slot_buf, sizeof(slot_buf), "Slots: %d/%d", sm.active_slots, sm.n_slots);
                            ColoredProgressBar(slot_frac, ImVec2(-1, 16), slot_buf,
                                ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
                                ImVec4(1.0f, 0.3f, 0.2f, 1.0f));
                            ImGui::Dummy(ImVec2(0.f, 6.f));
                        }
                        ImGui::EndChild();
                        ImGui::SameLine();

                        // Card 4: System Info
                        ImGui::BeginChild("##card_system", ImVec2(card_w, metric_card_height), true,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                        {
                            ImDrawList* draw = ImGui::GetWindowDrawList();
                            ImVec2 p_min = ImGui::GetWindowPos();
                            ImVec2 p_max = ImVec2(p_min.x + ImGui::GetWindowWidth(), p_min.y + 3);
                            draw->AddRectFilled(p_min, p_max, ImColor(0.2f, 0.8f, 0.5f));
                            ImGui::Spacing();
                            ImGui::TextDisabled("SYSTEM");
                            ImGui::Spacing();
                            if (sm.model_load_seconds > 0)
                                ImGui::Text("Load:      %.1fs", sm.model_load_seconds);
                            if (sm.model_memory_bytes > 0)
                                ImGui::Text("VRAM:      %s", format_bytes(sm.model_memory_bytes).c_str());
                            ImGui::Text("Port:      %d", srv->config.port);
                        }
                        ImGui::EndChild();

                        ImGui::Spacing();

                        // --- Row 3: Extended metrics (KV cache etc.) ---
                        if (sm.kv_cache_usage > 0.0f) {
                            char cache_overlay[48];
                            snprintf(cache_overlay, sizeof(cache_overlay), "KV Cache: %d%%", (int)(sm.kv_cache_usage * 100));
                            ColoredProgressBar(sm.kv_cache_usage, ImVec2(-1, 18), cache_overlay,
                                ImVec4(0.2f, 0.7f, 0.3f, 1.0f),
                                ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                            ImGui::Spacing();
                        }

                        ImGui::PopID();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }

        ImGui::EndChild(); // RightScroll — single scrollbar for launch form + inference

        if (has_inference_nodes) {
            ImGui::Separator();
            ImGui::Spacing();

            ServerInstance* term_srv = manager.running_servers[inference_tab_selected];
            ImGui::PushID(term_srv);

            ImGui::BeginChild("ModelTerminalDock", ImVec2(-1, kModelTerminalStripPx), true,
                ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(ImVec4(0.72f, 0.92f, 1.0f, 1.0f), "MODEL TERMINAL");
            ImGui::SameLine();
            ImGui::TextDisabled("(selected inference tab)");
            ImGui::Text("%s  ·  port %d", term_srv->config.name.c_str(), term_srv->config.port);
            ImGui::TextDisabled("$ tail -f %s", term_srv->log_file_path.c_str());
            ImGui::Spacing();
            UiDrawServerLogViewport(term_srv);
            ImGui::EndChild();

            ImGui::PopID();
        }
        
        ImGui::EndChild(); // End Content

        ToastDecayAndOverlay();

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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
