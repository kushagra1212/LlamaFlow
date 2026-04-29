#include "LlamaManager.hpp"
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <thread>
#include <vector>
#include <cctype>

namespace {
// Shutdown phases (ServerInstance::shutdown_phase) — visible on UI thread via release/acquire
constexpr int PHASE_NONE    = 0;
constexpr int PHASE_LOOKUP  = 1;  // find PID on port (external / attached)
constexpr int PHASE_SIGTERM = 2;
constexpr int PHASE_WAITING = 3;
constexpr int PHASE_SIGKILL = 4;
constexpr int PHASE_JOIN    = 5;  // join log/health threads

/** Signal the process tree leader: kill(-leader_pid) sends to whole process group (POSIX). */
static void signal_process_tree(pid_t leader_pid, int sig) {
    if (leader_pid <= 0)
        return;
    if (::kill(-leader_pid, sig) == 0)
        return;
    if (errno == EINVAL || errno == EPERM || errno == ESRCH)
        (void)::kill(leader_pid, sig);
}

static bool process_exists(pid_t p) {
    if (p <= 0)
        return false;
    return ::kill(p, 0) == 0;
}

static bool any_target_alive(const std::vector<pid_t>& targets) {
    for (pid_t p : targets) {
        if (process_exists(p))
            return true;
    }
    return false;
}

} // namespace

const char* ServerInstance::shutdown_status_text() const {
    switch (shutdown_phase.load(std::memory_order_acquire)) {
        case PHASE_NONE:
            return "";
        case PHASE_LOOKUP:
            return "Locating the process listening on this port (this can take a moment)...";
        case PHASE_SIGTERM:
            return "Sent SIGTERM — asking the server process group to exit cleanly...";
        case PHASE_WAITING:
            return "Waiting for the process to shut down (typically a few seconds)...";
        case PHASE_SIGKILL:
            return "Server did not exit in time — forcing SIGKILL on the process tree...";
        case PHASE_JOIN:
            return "Stopping background monitors and closing log readers...";
        default:
            return "";
    }
}

float ServerInstance::stop_elapsed_seconds() const {
    if (!stop_requested.load(std::memory_order_relaxed))
        return 0.f;
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - stop_requested_at).count();
}

ServerInstance::ServerInstance(const LlamaConfig& cfg) : config(cfg) {
    log_file_path = "/tmp/llama_log_" + std::to_string(config.port) + ".txt";
}

ServerInstance::~ServerInstance() {
    stop();
}

// -------------------------------------------------------
//  Attached mode: monitor an externally-started server
// -------------------------------------------------------
void ServerInstance::start_attached() {
    if (is_running) return;
    attached = true;
    is_running = true;
    model_loaded = true;        // assume loaded since port is active
    current_progress = 1.0f;
    start_time = std::chrono::steady_clock::now();

    // Tail log file if it exists (written by a previous LlamaFlow launch)
    log_thread = std::thread(&ServerInstance::tail_logs, this);

    // Start health polling thread
    health_thread = std::thread(&ServerInstance::poll_health, this);
}

ServerMetrics ServerInstance::snapshot() const {
    ServerMetrics m;
    m.current_progress        = current_progress.load();
    m.context_progress        = context_progress.load();
    m.context_processing      = context_processing.load();
    m.context_total           = context_total.load();
    m.prompt_eval_tps         = prompt_eval_tps.load();
    m.eval_tps                = eval_tps.load();
    m.total_tokens_processed  = total_tokens_processed.load();
    m.prompt_tokens           = prompt_tokens.load();
    m.generated_tokens        = generated_tokens.load();
    m.total_requests          = total_requests.load();
    m.completed_requests      = completed_requests.load();
    m.active_slots            = active_slots.load();
    m.n_slots                 = n_slots.load();
    m.kv_cache_usage          = kv_cache_usage.load();
    m.model_load_seconds      = model_load_seconds.load();
    m.model_memory_bytes      = model_memory_bytes.load();
    m.model_loaded            = model_loaded.load();

    if (is_running.load()) {
        auto now = std::chrono::steady_clock::now();
        m.uptime_seconds = std::chrono::duration<double>(now - start_time).count();
    }
    return m;
}

// --- Helper: extract a number after a keyword in a string ---
static float extract_float_after(const std::string& line, const std::string& keyword) {
    size_t pos = line.find(keyword);
    if (pos == std::string::npos) return -1.0f;
    pos += keyword.size();
    // Skip whitespace and punctuation
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == ':' || line[pos] == '=' || line[pos] == '\t')) pos++;
    size_t end = pos;
    while (end < line.size() && (isdigit(line[end]) || line[end] == '.' || line[end] == '-' || line[end] == '+')) end++;
    if (end > pos) {
        try { return std::stof(line.substr(pos, end - pos)); } catch(...) {}
    }
    return -1.0f;
}

/** Parse "... ms / 11391 tokens" as printed by print_timings in current llama.cpp */
static bool extract_tokens_after_ms_slash(const std::string& line, uint64_t* out_tokens) {
    const char* key = "ms / ";
    size_t p = line.find(key);
    if (p == std::string::npos)
        return false;
    p += strlen(key);
    while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p])))
        ++p;
    size_t start = p;
    while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p])))
        ++p;
    if (p == start)
        return false;
    if (line.compare(p, 7, " tokens") != 0)
        return false;
    try {
        *out_tokens = static_cast<uint64_t>(std::stoull(line.substr(start, p - start)));
        return true;
    } catch (...) {
        return false;
    }
}

static bool extract_http_body_if_ok(const std::string& response, std::string* body_out) {
    size_t hdr = response.find("\r\n\r\n");
    if (hdr == std::string::npos)
        return false;
    size_t line_end = response.find("\r\n");
    if (line_end == std::string::npos)
        return false;
    const std::string status_line = response.substr(0, line_end);
    if (status_line.find("200") == std::string::npos)
        return false;
    *body_out = response.substr(hdr + 4);
    return true;
}

static bool http_get_localhost_body(int port, const char* path, std::string* out_whole_response) {
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "Connection: close\r\n\r\n";
    std::string request = req.str();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = {0, 750000}; // 750 ms
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    bool ok = connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0;
    if (ok) {
        ssize_t nw = ::send(sock, request.data(), request.size(), 0);
        ok = nw == static_cast<ssize_t>(request.size());
        if (ok) {
            char buf[8192];
            out_whole_response->clear();
            ssize_t nr;
            while ((nr = recv(sock, buf, sizeof(buf)-1, 0)) > 0) {
                buf[nr] = '\0';
                *out_whole_response += buf;
                if (out_whole_response->size() > 4 * 1024 * 1024)
                    break;
            }
        }
    }
    close(sock);
    return ok && !out_whole_response->empty();
}

/** Parse llamacpp: lines from Prometheus text (see ggml-org llama.cpp server metrics route) */
static void apply_prometheus_plaintext(ServerInstance* inst, const std::string& body) {
    uint64_t prompt_tot = 0, gen_tot = 0;
    bool has_prompt_cnt = false, has_gen_cnt = false;
    float p_tps = -1.f, g_tps = -1.f, kv = -1.f;

    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.rfind("llamacpp:", 0) != 0)
            continue;
        size_t last_sp = line.find_last_of(" \t");
        if (last_sp == std::string::npos || last_sp < 10)
            continue;
        char* endp = nullptr;
        double v = std::strtod(line.c_str() + last_sp + 1, &endp);
        if (endp == line.c_str() + last_sp + 1)
            continue;
        std::string name = line.substr(0, last_sp);
        if (name == "llamacpp:prompt_tokens_total") {
            prompt_tot = static_cast<uint64_t>(v);
            has_prompt_cnt = true;
        } else if (name == "llamacpp:tokens_predicted_total") {
            gen_tot = static_cast<uint64_t>(v);
            has_gen_cnt = true;
        } else if (name == "llamacpp:prompt_tokens_seconds") {
            p_tps = static_cast<float>(v);
        } else if (name == "llamacpp:predicted_tokens_seconds") {
            g_tps = static_cast<float>(v);
        } else if (name == "llamacpp:kv_cache_usage_ratio") {
            kv = static_cast<float>(v);
        }
    }

    if (has_prompt_cnt && has_gen_cnt) {
        inst->metrics_from_prometheus.store(true, std::memory_order_relaxed);
        inst->prompt_tokens.store(prompt_tot, std::memory_order_relaxed);
        inst->generated_tokens.store(gen_tot, std::memory_order_relaxed);
        inst->total_tokens_processed.store(prompt_tot + gen_tot, std::memory_order_relaxed);
    } else {
        inst->metrics_from_prometheus.store(false, std::memory_order_relaxed);
    }
    if (p_tps >= 0.f)
        inst->prompt_eval_tps.store(p_tps, std::memory_order_relaxed);
    if (g_tps >= 0.f)
        inst->eval_tps.store(g_tps, std::memory_order_relaxed);
    if (kv >= 0.f)
        inst->kv_cache_usage.store(kv, std::memory_order_relaxed);
}

static uint64_t extract_uint_after(const std::string& line, const std::string& keyword) {
    size_t pos = line.find(keyword);
    if (pos == std::string::npos) return 0;
    pos += keyword.size();
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == ':' || line[pos] == '=' || line[pos] == '\t')) pos++;
    size_t end = pos;
    while (end < line.size() && isdigit(line[end])) end++;
    if (end > pos) {
        try { return static_cast<uint64_t>(std::stoull(line.substr(pos, end - pos))); } catch(...) {}
    }
    return 0;
}

// -------------------------------------------------------
//  HTTP polling (/health, /slots, /metrics) — spawned and attached servers
// -------------------------------------------------------
void ServerInstance::poll_health() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    while (!stop_threads && is_running.load(std::memory_order_relaxed)) {
        const int port = config.port;

        std::string raw;
        if (http_get_localhost_body(port, "/health", &raw)) {
            std::string body;
            if (extract_http_body_if_ok(raw, &body)) {
                try {
                    json j = json::parse(body);

                    if (j.contains("slots_idle")) {
                        const int idle = j["slots_idle"].get<int>();
                        const int processing =
                            j.contains("slots_processing") ? j["slots_processing"].get<int>() : 0;
                        n_slots = idle + processing;
                        active_slots = processing;
                    }

                    if (j.contains("requests_processed")) {
                        completed_requests.store(
                            static_cast<uint64_t>(j["requests_processed"].get<uint64_t>()),
                            std::memory_order_relaxed);
                    }
                    if (j.contains("tokens_processed")) {
                        total_tokens_processed.store(
                            static_cast<uint64_t>(j["tokens_processed"].get<uint64_t>()),
                            std::memory_order_relaxed);
                    }
                } catch (...) {
                    // minimal /health body (e.g. {"status":"ok"}) — expected
                }
            }
        }

        if (http_get_localhost_body(port, "/slots", &raw)) {
            std::string body;
            if (extract_http_body_if_ok(raw, &body)) {
                try {
                    json arr = json::parse(body);
                    if (arr.is_array()) {
                        int active = 0;
                        for (const auto& el : arr) {
                            if (el.value("is_processing", false))
                                active++;
                        }
                        n_slots = static_cast<int>(arr.size());
                        active_slots = active;
                    }
                } catch (...) {}
            }
        }

        if (http_get_localhost_body(port, "/metrics", &raw)) {
            std::string body;
            if (extract_http_body_if_ok(raw, &body))
                apply_prometheus_plaintext(this, body);
            else
                metrics_from_prometheus.store(false, std::memory_order_relaxed);
        } else {
            metrics_from_prometheus.store(false, std::memory_order_relaxed);
        }

        for (int i = 0; i < 40 && !stop_threads && is_running.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// -------------------------------------------------------
//  Helper: split a string by spaces into argv, respecting shell quoting
// -------------------------------------------------------
static std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote) { quote = 0; continue; }
            cur += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == ' ') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// -------------------------------------------------------
//  Pretty labels for CUSTOM ARGUMENTS (display only)
// -------------------------------------------------------
std::vector<std::pair<std::string, std::string>> LlamaManager::summarize_launch_args(const std::string& custom_args_only) {
    std::vector<std::pair<std::string, std::string>> rows;
    std::string safe = custom_args_only;
    std::replace(safe.begin(), safe.end(), '\n', ' ');
    auto tok = split_args(safe);

    for (size_t i = 0; i < tok.size(); ++i) {
        const std::string& w = tok[i];

        auto pair_val = [&](const char* label) {
            if (i + 1 < tok.size()) {
                rows.emplace_back(label, tok[i + 1]);
                ++i;
            } else {
                rows.emplace_back(label, "—");
            }
        };

        if (w == "-c") {
            pair_val("Context length (-c)");
        } else if (w == "--ctx-size") {
            pair_val("Context (--ctx-size)");
        } else if (w == "-n" || w == "--threads") {
            pair_val("CPU threads (-n)");
        } else if (w == "-ngl") {
            pair_val("GPU layers (-ngl)");
        } else if (w == "-b") {
            pair_val("Batch (-b)");
        } else if (w == "-ub") {
            pair_val("Micro-batch (-ub)");
        } else if (w == "-ctk") {
            pair_val("K cache type (-ctk)");
        } else if (w == "-ctv") {
            pair_val("V cache type (-ctv)");
        } else if (w == "-m") {
            pair_val("Model path (-m)");
        } else if (w == "--port") {
            pair_val("Port (--port)");
        } else if (w == "--temp") {
            pair_val("Temperature (--temp)");
        } else if (w == "--top-p") {
            pair_val("Top-p (--top-p)");
        } else if (w == "--top-k") {
            pair_val("Top-k (--top-k)");
        } else if (w == "--min-p") {
            pair_val("Min-p (--min-p)");
        } else if (w == "--repeat-penalty") {
            pair_val("Repeat penalty (--repeat-penalty)");
        } else if (w == "--ctx-checkpoints") {
            pair_val("Context checkpoints (--ctx-checkpoints)");
        } else if (w == "--checkpoint-every-n-tokens") {
            pair_val("Checkpoint every n tokens");
        } else if (w == "--chat-template-kwargs") {
            pair_val("--chat-template-kwargs");
        } else if (w == "-fa" || w == "--flash-attn") {
            std::string val = "on";
            if (i + 1 < tok.size() && !tok[i + 1].empty() && tok[i + 1][0] != '-') {
                val = tok[i + 1];
                ++i;
            }
            rows.emplace_back(w == "--flash-attn" ? "Flash attention (--flash-attn)" : "Flash attention (-fa)", val);
        }
    }

    return rows;
}

void ServerInstance::start() {
    if (is_running) return;

    // Record start time for uptime
    start_time = std::chrono::steady_clock::now();

    // 1. Sanitize custom arguments (flatten newlines and remove backslashes)
    std::string safe_args = config.custom_args;
    std::replace(safe_args.begin(), safe_args.end(), '\n', ' ');
    safe_args.erase(std::remove(safe_args.begin(), safe_args.end(), '\\'), safe_args.end());

    // 2. Expand ~ in paths using wordexp / getenv
    std::string model_path = config.model_path;
    std::string exec_path  = config.exec_path;
    if (!model_path.empty() && model_path[0] == '~') {
        const char* home = getenv("HOME");
        if (home) model_path = std::string(home) + model_path.substr(1);
    }
    if (!exec_path.empty() && exec_path[0] == '~') {
        const char* home = getenv("HOME");
        if (home) exec_path = std::string(home) + exec_path.substr(1);
    }

    // 3. Build argv for execvp
    std::vector<std::string> args_str;
    args_str.push_back(exec_path);
    args_str.push_back("-m");
    args_str.push_back(model_path);
    args_str.push_back("--port");
    args_str.push_back(std::to_string(config.port));
    // Append custom args (already space-separated, no newlines)
    auto custom = split_args(safe_args);
    args_str.insert(args_str.end(), custom.begin(), custom.end());

    // Build the raw C-style argv
    std::vector<const char*> argv;
    for (const auto& a : args_str) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // 4. Open log file for stdout/stderr redirection
    int log_fd = open(log_file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        std::lock_guard<std::mutex> lock(log_mutex);
        logs.push_back("FAILED to open log file: " + log_file_path);
        return;
    }

    // 5. Fork + exec
    pid_t pid = fork();
    if (pid == 0) {
        // --- Child ---
        // Isolate session/process group so PGID == pid; kill(-pid) then targets this server tree only (EINVAL -> fallback kill(pid)).
        if (setsid() == (pid_t)-1 && setpgid(0, 0) != 0) {
            // Last resort — still exec; teardown uses kill(pid, ...) only when process-group kill fails
        }
        // Redirect stdout/stderr to log file
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);

        execvp(exec_path.c_str(), (char* const*)argv.data());

        // execvp failed — write error to log and exit
        int err = errno;
        dprintf(STDERR_FILENO, "execvp(%s) failed: %s\n", exec_path.c_str(), strerror(err));
        _exit(127);
    } else if (pid > 0) {
        // --- Parent ---
        close(log_fd);
        process_id = pid;
        is_running = true;
        log_thread = std::thread(&ServerInstance::tail_logs, this);
        health_thread = std::thread(&ServerInstance::poll_health, this);

        // Log success
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            logs.push_back("Launched PID " + std::to_string(pid) + " -> " + log_file_path);
        }
    } else {
        // --- Fork failed ---
        close(log_fd);
        std::lock_guard<std::mutex> lock(log_mutex);
        logs.push_back("FAILED to fork(): " + std::string(strerror(errno)));
    }
}

void ServerInstance::stop() {
    if (!is_running.load())
        return;

    // --- Attached: we did not fork this process — find listener PID and kill the whole group -----------
    if (attached) {
        shutdown_phase.store(PHASE_LOOKUP, std::memory_order_release);

        std::vector<pid_t> targets = LlamaManager::pids_listen_on_tcp_port(config.port);
        if (targets.empty() && external_listen_pid > 0)
            targets.push_back(external_listen_pid);
        else if (!targets.empty())
            external_listen_pid = targets[0];

        if (!targets.empty()) {
            {
                std::lock_guard<std::mutex> lg(log_mutex);
                std::string msg = "Stop: terminating external server — PIDs";
                for (pid_t t : targets)
                    msg += " " + std::to_string((int)t);
                msg += " (port " + std::to_string(config.port) + ")";
                logs.push_back(msg);
            }

            shutdown_phase.store(PHASE_SIGTERM, std::memory_order_release);
            for (pid_t t : targets)
                signal_process_tree(t, SIGTERM);

            shutdown_phase.store(PHASE_WAITING, std::memory_order_release);
            for (int i = 0; i < 75; ++i) {
                if (!any_target_alive(targets))
                    break;
                if (!LlamaManager::is_port_in_use(config.port))
                    break;
                usleep(100000);
            }

            if (any_target_alive(targets)) {
                shutdown_phase.store(PHASE_SIGKILL, std::memory_order_release);
                for (pid_t t : targets) {
                    if (process_exists(t))
                        signal_process_tree(t, SIGKILL);
                }
                for (int i = 0; i < 40; ++i) {
                    if (!any_target_alive(targets))
                        break;
                    usleep(100000);
                }
            }
        } else {
            std::lock_guard<std::mutex> lg(log_mutex);
            logs.push_back(
                "Stop: could not find listener PID (install `lsof` in PATH, or quit llama-server manually).");
        }

        shutdown_phase.store(PHASE_JOIN, std::memory_order_release);
        is_running = false;
        stop_threads = true;
        if (log_thread.joinable())
            log_thread.join();
        if (health_thread.joinable())
            health_thread.join();
        shutdown_phase.store(PHASE_NONE, std::memory_order_release);
        return;
    }

    is_running = false;
    stop_threads = true;

    if (process_id > 0) {
        shutdown_phase.store(PHASE_SIGTERM, std::memory_order_release);
        signal_process_tree(process_id, SIGTERM);

        shutdown_phase.store(PHASE_WAITING, std::memory_order_release);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t ret = waitpid(process_id, &status, WNOHANG);
            if (ret == process_id)
                goto reaped_child;
            if (ret == -1 && errno == ECHILD)
                goto reaped_child;
            usleep(100000); // 100ms
        }

        shutdown_phase.store(PHASE_SIGKILL, std::memory_order_release);
        signal_process_tree(process_id, SIGKILL);
        (void)waitpid(process_id, nullptr, 0);

    reaped_child:
        process_id = -1;
    }

    shutdown_phase.store(PHASE_JOIN, std::memory_order_release);
    if (log_thread.joinable())
        log_thread.join();
    if (health_thread.joinable())
        health_thread.join();

    shutdown_phase.store(PHASE_NONE, std::memory_order_release);
}

void ServerInstance::tail_logs() {
    FILE* file = nullptr;
    std::string current_line = "";

    while (is_running) {
        if (!file) {
            file = fopen(log_file_path.c_str(), "r");
        }

        if (file) {
            char buffer[2048];
            // Safely read whatever bytes are currently available
            size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);

            if (bytes_read > 0) {
                std::lock_guard<std::mutex> lock(log_mutex);
                for (size_t i = 0; i < bytes_read; ++i) {
                    char c = buffer[i];
                    
                    // Llama.cpp uses \r for progress bars and \n for new lines
                   if (c == '\n' || c == '\r') {
                        if (!current_line.empty()) {
                            
                            // ===== ENHANCED METRICS PARSING =====

                            // --- Determine if this is prompt processing progress ---
                            bool is_prompt_progress = (current_line.find("prompt processing progress") != std::string::npos);
                            bool is_prompt_done      = (current_line.find("prompt processing done") != std::string::npos);

                            // 1. Loading / Context progress
                            float val = extract_float_after(current_line, "progress =");
                            if (val >= 0.0f && val <= 1.0f) {
                                if (is_prompt_progress) {
                                    // This is context/prompt ingestion progress
                                    context_progress  = val;
                                    context_processing = true;
                                    // Extract total n_tokens from the line if present
                                    // e.g. "n_tokens = 11391, batch.n_tokens = 4, progress = 0.999649"
                                    uint64_t ctx_total = extract_uint_after(current_line, "n_tokens =");
                                    if (ctx_total > 0) context_total = ctx_total;
                                } else {
                                    current_progress = val;
                                }
                            }
                            // "progress: 85%" — percentage format (model loading)
                            else if (!is_prompt_progress) {
                                size_t pct_pos = current_line.find("progress:");
                                if (pct_pos != std::string::npos || current_line.find("%") != std::string::npos) {
                                    val = extract_float_after(current_line, "progress:");
                                    if (val < 0 && pct_pos != std::string::npos) {
                                        // Try number before %
                                        size_t pct = current_line.find("%", pct_pos);
                                        size_t num_end = pct;
                                        size_t num_start = num_end;
                                        while (num_start > pct_pos && (isdigit(current_line[num_start-1]) || current_line[num_start-1] == '.')) num_start--;
                                        if (num_end > num_start) {
                                            try { val = std::stof(current_line.substr(num_start, num_end - num_start)) / 100.0f; } catch(...) {}
                                        }
                                    }
                                    if (val >= 0.0f && val <= 1.0f) current_progress = val;
                                }
                            }

                            // 2. Prompt processing done — clear context flag
                            if (is_prompt_done) {
                                context_processing = false;
                            }

                            // 3. Track model loaded state from key log lines
                            if (!model_loaded) {
                                if (current_line.find("llama_new_context_with_model") != std::string::npos ||
                                    current_line.find("loaded model in") != std::string::npos ||
                                    current_line.find("starting the server") != std::string::npos ||
                                    current_line.find("build info") != std::string::npos ||
                                    current_line.find("server is listening") != std::string::npos ||
                                    current_line.find("starting assistant loop") != std::string::npos ||
                                    current_line.find("model loaded") != std::string::npos) {
                                    model_loaded = true;
                                    current_progress = 1.0f;
                                }
                                // File loading progress: "loaded 42/42 files" or "| 42/42 |"
                                else if (current_line.find("loaded") != std::string::npos &&
                                         current_line.find("/") != std::string::npos &&
                                         current_line.find("file") != std::string::npos) {
                                    // Parse fraction like "42/42"
                                    size_t slash = current_line.find("/");
                                    size_t num_start = slash;
                                    while (num_start > 0 && isdigit(current_line[num_start-1])) num_start--;
                                    size_t num_end = slash + 1;
                                    while (num_end < current_line.size() && isdigit(current_line[num_end])) num_end++;
                                    if (num_end > slash + 1 && slash > num_start) {
                                        try {
                                            float num = std::stof(current_line.substr(num_start, slash - num_start));
                                            float den = std::stof(current_line.substr(slash + 1, num_end - slash - 1));
                                            if (den > 0) current_progress = std::min(num / den, 1.0f);
                                        } catch(...) {}
                                    }
                                }
                            }

                            const bool from_prom =
                                metrics_from_prometheus.load(std::memory_order_relaxed);
                            const bool is_prompt_eval_line =
                                current_line.find("prompt eval time") != std::string::npos;
                            const bool is_gen_eval_line =
                                current_line.find("eval time") != std::string::npos && !is_prompt_eval_line;

                            if (is_prompt_eval_line) {
                                if (current_line.find("tokens per second") != std::string::npos) {
                                    size_t p1 = current_line.find_last_of(',');
                                    size_t p2 = current_line.find("tokens per second");
                                    if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1) {
                                        try {
                                            prompt_eval_tps = std::stof(current_line.substr(p1 + 1, p2 - p1 - 1));
                                            current_progress = 1.0f;
                                        } catch (...) {}
                                    }
                                } else {
                                    current_progress = 1.0f;
                                }
                                if (!from_prom) {
                                    uint64_t ntok = 0;
                                    if (extract_tokens_after_ms_slash(current_line, &ntok)) {
                                        prompt_tokens.fetch_add(ntok);
                                        total_tokens_processed.store(prompt_tokens.load() +
                                                                     generated_tokens.load());
                                    }
                                }
                            }
                            if (is_gen_eval_line) {
                                if (current_line.find("tokens per second") != std::string::npos) {
                                    size_t p1 = current_line.find_last_of(',');
                                    size_t p2 = current_line.find("tokens per second");
                                    if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1) {
                                        try {
                                            eval_tps = std::stof(current_line.substr(p1 + 1, p2 - p1 - 1));
                                        } catch (...) {}
                                    }
                                }
                                if (!from_prom) {
                                    uint64_t ntok = 0;
                                    if (extract_tokens_after_ms_slash(current_line, &ntok)) {
                                        generated_tokens.fetch_add(ntok);
                                        total_tokens_processed.store(prompt_tokens.load() +
                                                                     generated_tokens.load());
                                    }
                                }
                            }

                            if (current_line.find("done request") != std::string::npos ||
                                current_line.find("request completed") != std::string::npos) {
                                completed_requests.fetch_add(1, std::memory_order_relaxed);
                                total_requests.fetch_add(1, std::memory_order_relaxed);
                            }

                            if (!from_prom && current_line.find("prompt tokens") != std::string::npos) {
                                uint64_t v = extract_uint_after(current_line, "prompt tokens");
                                if (v > 0) {
                                    prompt_tokens.store(v);
                                    total_tokens_processed.store(prompt_tokens.load() + generated_tokens.load());
                                }
                            } else if (!from_prom &&
                                       current_line.find("generated tokens") != std::string::npos) {
                                uint64_t v = extract_uint_after(current_line, "generated tokens");
                                if (v > 0) {
                                    generated_tokens.store(v);
                                    total_tokens_processed.store(prompt_tokens.load() + generated_tokens.load());
                                }
                            } else if (current_line.find("kv cache") != std::string::npos) {
                                val = extract_float_after(current_line, "kv cache");
                                if (val < 0) val = extract_float_after(current_line, "kv_cache");
                                if (val >= 0.0f) kv_cache_usage = val;
                            } else if (current_line.find("n_slots") != std::string::npos) {
                                uint64_t v = extract_uint_after(current_line, "n_slots");
                                if (v > 0) n_slots = static_cast<int>(v);
                            } else if ((current_line.find("request") != std::string::npos) &&
                                       (current_line.find("all slots") == std::string::npos) &&
                                       (current_line.find("completed") == std::string::npos) &&
                                       (current_line.find("done request") == std::string::npos) &&
                                       (current_line.find("new") != std::string::npos ||
                                        current_line.find("start") != std::string::npos ||
                                        current_line.find("incoming") != std::string::npos)) {
                                total_requests.fetch_add(1, std::memory_order_relaxed);
                            } else if (current_line.find("loaded model in") != std::string::npos) {
                                val = extract_float_after(current_line, "loaded model in");
                                if (val >= 0.0f) model_load_seconds = val / 1000.0f;
                            } else if (current_line.find("total VRAM") != std::string::npos) {
                                size_t vr = current_line.find("total VRAM");
                                size_t num_start = vr;
                                while (num_start < current_line.size() &&
                                       !isdigit(static_cast<unsigned char>(current_line[num_start])))
                                    num_start++;
                                size_t num_end = num_start;
                                while (num_end < current_line.size() &&
                                       (isdigit(static_cast<unsigned char>(current_line[num_end])) ||
                                        current_line[num_end] == '.'))
                                    num_end++;
                                if (num_end > num_start) {
                                    try {
                                        float mem_val =
                                            std::stof(current_line.substr(num_start, num_end - num_start));
                                        std::string after = current_line.substr(num_end);
                                        if (after.find("GB") != std::string::npos ||
                                            after.find("GiB") != std::string::npos)
                                            mem_val *= 1024.0f;
                                        model_memory_bytes =
                                            static_cast<uint64_t>(mem_val * 1024.0f * 1024.0f);
                                    } catch (...) {}
                                }
                            } else if (current_line.find("slot") != std::string::npos &&
                                       current_line.find("in use") != std::string::npos) {
                                active_slots++;
                            } else if (current_line.find("slot") != std::string::npos &&
                                       current_line.find("free") != std::string::npos &&
                                       current_line.find("cache") == std::string::npos) {
                                if (active_slots > 0) active_slots--;
                            }
                            // ====================================

                            // If it's a progress update, replace the last line to avoid spam
                            if (c == '\r' && !logs.empty() && (logs.back().find("...") != std::string::npos || logs.back().find("progress") != std::string::npos)) {
                                logs.back() = current_line; 
                            } else {
                                logs.push_back(current_line);
                            }
                            
                            if (logs.size() > 500) logs.pop_front();
                            current_line.clear();
                        }
                    } else {
                        current_line += c; // Safely build the line char-by-char
                    }
                }
            } else {
                // We hit the end of the file. Clear the EOF flag to wait for more data.
                clearerr(file);
            }
        }
        
        // Sleep briefly to prevent pinning the CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    if (file) fclose(file);
}

// ============================================================
//  Port conflict check — tries to bind to the port (reliable on macOS)
// ============================================================
bool LlamaManager::is_port_in_use(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    // Use a short timeout via non-blocking + select
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    
    if (rc == 0) {
        // Immediately connected = port is in use
        close(sock);
        return true;
    }
    
    if (rc < 0 && errno == EINPROGRESS) {
        // Connection in progress — wait briefly
        struct timeval tv = {0, 100000}; // 100ms timeout
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        rc = select(sock + 1, NULL, &fdset, NULL, &tv);
        if (rc > 0) {
            // Socket is writable — check if connection actually succeeded
            // (select returns >0 even on connection refusal on macOS)
            int error = 0;
            socklen_t errlen = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errlen) == 0) {
                close(sock);
                return (error == 0);  // only true if no error (connected successfully)
            }
            close(sock);
            return false;
        }
        // select timed out — no listener, port is free
        close(sock);
        return false;
    }
    
    close(sock);
    return false;
}

// -------------------------------------------------------
//  Resolve PIDs that own TCP LISTEN on `port` (external llama-server, etc.)
// -------------------------------------------------------
std::vector<pid_t> LlamaManager::pids_listen_on_tcp_port(int port) {
    std::vector<pid_t> out;

    static const char* const templates[] = {
        "/usr/sbin/lsof -nP -iTCP:%d -sTCP:LISTEN -t 2>/dev/null",
        "lsof -nP -iTCP:%d -sTCP:LISTEN -t 2>/dev/null",
        "/usr/sbin/lsof -nP -i :%d -sTCP:LISTEN -t 2>/dev/null",
        "lsof -nP -i :%d -sTCP:LISTEN -t 2>/dev/null",
    };

    for (const char* tmpl : templates) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), tmpl, port);
        FILE* fp = ::popen(cmd, "r");
        if (!fp)
            continue;
        char line[128];
        while (std::fgets(line, sizeof(line), fp)) {
            char* end_ptr = nullptr;
            long v = std::strtol(line, &end_ptr, 10);
            if (v > 0 && v < 100000000L)
                out.push_back(static_cast<pid_t>(v));
        }
        (void)::pclose(fp);
        if (!out.empty())
            break;
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void LlamaManager::load_configs(const std::string& filename) {
    std::ifstream file(filename);
    if (file.is_open()) {
        json j;
        file >> j;
        saved_configs = j.get<std::vector<LlamaConfig>>();
    }
}

void LlamaManager::save_configs(const std::string& filename) {
    std::ofstream file(filename);
    if (file.is_open()) {
        json j = saved_configs;
        file << j.dump(4);
    }
}

bool LlamaManager::launch_server(const LlamaConfig& config) {
    last_launch_error.clear();
    
    // 1. Check if port is already in use on this machine
    if (is_port_in_use(config.port)) {
        last_launch_error = "Port " + std::to_string(config.port) + " is already in use by another process. Choose a different port.";
        return false;
    }
    
    // 2. Check if any existing server already uses this port
    for (auto* srv : running_servers) {
        if (srv->config.port == config.port) {
            last_launch_error = "Port " + std::to_string(config.port) + " is already assigned to running server \"" + srv->config.name + "\".";
            return false;
        }
    }
    
    // 3. Validate paths exist
    if (config.exec_path.empty()) {
        last_launch_error = "Executable path is empty.";
        return false;
    }
    if (config.model_path.empty()) {
        last_launch_error = "Model file path is empty.";
        return false;
    }
    
    ServerInstance* instance = new ServerInstance(config);
    instance->start();
    
    // Verify the instance actually started (popen may have failed)
    if (!instance->is_running.load()) {
        last_launch_error = "Failed to launch process. Check that the executable exists at: " + config.exec_path;
        // Copy any error logs from the instance before deleting
        {
            std::lock_guard<std::mutex> lock(instance->log_mutex);
            for (const auto& line : instance->logs) {
                last_launch_error += "\n  " + line;
            }
        }
        delete instance;
        return false;
    }
    
    running_servers.push_back(instance);
    return true;
}

void LlamaManager::request_stop_server(ServerInstance* instance) {
    if (!instance)
        return;
    bool expected = false;
    if (!instance->stop_requested.compare_exchange_strong(expected, true))
        return;

    instance->stop_requested_at = std::chrono::steady_clock::now();

    std::thread([this, instance]() {
        instance->stop();
        std::lock_guard<std::mutex> lk(async_stop_completion_mutex_);
        async_stop_completed_.push_back(instance);
    }).detach();
}

void LlamaManager::drain_async_stop_completions() {
    std::vector<ServerInstance*> done;
    {
        std::lock_guard<std::mutex> lk(async_stop_completion_mutex_);
        done.swap(async_stop_completed_);
    }
    for (ServerInstance* inst : done) {
        auto it = std::find(running_servers.begin(), running_servers.end(), inst);
        if (it != running_servers.end()) {
            delete *it;
            running_servers.erase(it);
        }
    }
}

// -------------------------------------------------------
//  Try to attach to an externally-started server on `port`
// -------------------------------------------------------
bool LlamaManager::attach_to_port(int port) {
    // Already tracking this port?
    for (auto* srv : running_servers) {
        if (srv->config.port == port) return true;   // already attached
    }

    // Check if port is actually in use
    if (!is_port_in_use(port)) return false;

    // Quick health check to confirm it's a responding HTTP server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv = {0, 300000}; // 300ms connect timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0) { close(sock); return false; }

    // Send a minimal health check request
    const char* req = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(sock, req, strlen(req), 0);

    char buf[256];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);

    if (n <= 0) return false;
    buf[n] = '\0';

    // Verify we got an HTTP response (status line)
    if (strstr(buf, "HTTP/") == nullptr) return false;

    // --- Create a tracked instance in attached mode ---
    LlamaConfig cfg;
    cfg.port = port;
    cfg.name = "External (port " + std::to_string(port) + ")";

    ServerInstance* instance = new ServerInstance(cfg);
    std::vector<pid_t> plist = LlamaManager::pids_listen_on_tcp_port(port);
    if (!plist.empty())
        instance->external_listen_pid = plist[0];

    instance->start_attached();
    running_servers.push_back(instance);

    return true;
}

// -------------------------------------------------------
//  Scan all saved configs and attach to any running servers
// -------------------------------------------------------
void LlamaManager::attach_all() {
    for (const auto& cfg : saved_configs) {
        attach_to_port(cfg.port);
    }
}
