#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include "LlamaManager.hpp"
#include <vector>

// --- Helper for std::string with ImGui InputText ---
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

// --- The Minimalist Theme Engine ---
void ApplyMinimalistTheme() {
    ImGuiStyle* style = &ImGui::GetStyle();
    ImVec4* colors = style->Colors;

    // Structural Geometry
    style->WindowRounding = 8.0f;
    style->FrameRounding = 6.0f;
    style->GrabRounding = 6.0f;
    style->PopupRounding = 6.0f;
    style->ChildRounding = 6.0f;
    style->TabRounding = 6.0f;
    
    style->WindowBorderSize = 0.0f;
    style->FrameBorderSize = 0.0f;
    style->ChildBorderSize = 1.0f; // Subtle border for logical grouping
    
    style->ItemSpacing = ImVec2(8, 12); // Give elements room to breathe

    // Matte Dark Palette
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_Border]                 = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
}

int main() {
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1380, 850, "LlamaFlow Orchestrator", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImFont* font = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Supplemental/Arial.ttf", 20.0f);
    if (!font) {
        // Fallback for Windows (for your dual-boot setup)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 20.0f);
    }
    if (!font) {
        // Absolute fallback: scale up the default pixel font
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1.5f; 
    }
    
    // Apply our custom sleek theme
    ApplyMinimalistTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    LlamaManager manager;
    manager.load_configs();
    int selected_config_idx = -1;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ==========================================
        // LEFT SIDEBAR: CONFIGURATION LIST
        // ==========================================
        ImGui::BeginChild("Sidebar", ImVec2(350, 0), true);
        
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // Optional: scale up later
        ImGui::TextDisabled("SAVED MODELS");
        ImGui::PopFont();
        ImGui::Spacing();

        if (ImGui::Button("+ Add New Model", ImVec2(-1, 40))) {
            manager.saved_configs.push_back(LlamaConfig());
            selected_config_idx = manager.saved_configs.size() - 1;
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        
        for (int i = 0; i < manager.saved_configs.size(); ++i) {
            bool is_selected = (selected_config_idx == i);
            if (ImGui::Selectable(manager.saved_configs[i].name.c_str(), is_selected, 0, ImVec2(0, 30))) {
                selected_config_idx = i;
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ==========================================
        // RIGHT PANEL: EDITOR & LOGS
        // ==========================================
        ImGui::BeginChild("Content", ImVec2(0, 0), false);
        
        if (selected_config_idx >= 0 && selected_config_idx < manager.saved_configs.size()) {
            LlamaConfig& cfg = manager.saved_configs[selected_config_idx];
            
            // --- TOP ROW: Actions ---
            ImGui::BeginChild("TopActions", ImVec2(0, 50), false);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "CONFIG: %s", cfg.name.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 320);
            
            if (ImGui::Button("Save Changes", ImVec2(150, 35))) {
                manager.save_configs();
            }
            ImGui::SameLine();
            
            // Semantic Green Button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.60f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.70f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.50f, 0.25f, 1.0f));
            if (ImGui::Button("START SERVER", ImVec2(150, 35))) {
                manager.launch_server(cfg);
            }
            ImGui::PopStyleColor(3);
            ImGui::EndChild();

            ImGui::Separator(); ImGui::Spacing();

            // --- EDITOR FORM ---
            ImGui::BeginChild("EditorForm", ImVec2(0, 290), true);
            ImGui::TextDisabled("LAUNCH PARAMETERS");
            ImGui::Spacing();

            ImGui::Text("Model Name Alias");
            ImGui::PushItemWidth(-1);
            ImGui_InputText("##Name", &cfg.name);
            ImGui::PopItemWidth();
            ImGui::Spacing();

            ImGui::Text("Executable Path (llama-server)");
            ImGui::PushItemWidth(-1);
            ImGui_InputText("##ExecPath", &cfg.exec_path);
            ImGui::PopItemWidth();
            ImGui::Spacing();

            ImGui::Text("Model File (.gguf path)");
            ImGui::PushItemWidth(-1);
            ImGui_InputText("##ModelPath", &cfg.model_path);
            ImGui::PopItemWidth();
            ImGui::Spacing();

            ImGui::Text("Network Port");
            ImGui::PushItemWidth(150);
            ImGui::InputInt("##Port", &cfg.port);
            ImGui::PopItemWidth();

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextDisabled("CUSTOM ARGUMENTS");
            ImGui_InputTextMultiline("##Args", &cfg.custom_args, ImVec2(-1, 60));
            ImGui::EndChild();
        } else {
            ImGui::TextDisabled("Select a configuration from the sidebar to begin.");
        }

        ImGui::Spacing(); ImGui::Spacing();

        // ==========================================
        // BOTTOM SECTION: RUNNING INSTANCES
        // ==========================================
        if (!manager.running_servers.empty()) {
            ImGui::TextDisabled("ACTIVE INFERENCE NODES");
            
            if (ImGui::BeginTabBar("ServersTab")) {
                for (int i = 0; i < manager.running_servers.size(); ++i) {
                    ServerInstance* srv = manager.running_servers[i];
                    std::string tab_name = srv->config.name + " (Port " + std::to_string(srv->config.port) + ")";
                    
                    if (ImGui::BeginTabItem(tab_name.c_str())) {
                        
                        ImGui::Spacing();
                        // Semantic Red Button
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.15f, 0.15f, 1.0f));
                        if (ImGui::Button("STOP SERVER", ImVec2(150, 30))) {
                            manager.stop_server(i);
                            ImGui::PopStyleColor(3);
                            ImGui::EndTabItem();
                            break; 
                        }
                        ImGui::PopStyleColor(3);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "  ● Status: Running (PID: %d)", srv->process_id);
                        ImGui::Spacing();

                        // Terminal Output Window (Darker to look like a console)
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
                        ImGui::BeginChild("LogViewer", ImVec2(-1, -1), true, ImGuiWindowFlags_HorizontalScrollbar);
                        
                        std::lock_guard<std::mutex> lock(srv->log_mutex);
                        for (const auto& line : srv->logs) {
                            ImGui::TextUnformatted(line.c_str());
                        }
                        
                        // Auto-scroll logic
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                            ImGui::SetScrollHereY(1.0f);
                        }
                            
                        ImGui::EndChild();
                        ImGui::PopStyleColor();

                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }
        
        ImGui::EndChild();
        ImGui::End(); // End Main Window

        // --- Render Frame ---
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f); // Match the matte background
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