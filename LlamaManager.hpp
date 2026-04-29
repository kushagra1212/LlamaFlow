#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <sys/types.h>
#include "json.hpp" // nlohmann/json

using json = nlohmann::json;

struct LlamaConfig {
    std::string name = "New Model";
    std::string exec_path = "./llama-server";
    std::string model_path = "";
    int port = 8080;
    std::string custom_args = "-c 4096 -ngl 99"; 

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LlamaConfig, name, exec_path, model_path, port, custom_args)
};

// Metrics snapshot for display
struct ServerMetrics {
    // Model loading progress (0..1, 1 = done)
    float current_progress = 0.0f;

    // Context / prompt processing progress (0..1, for the current request)
    float context_progress = 0.0f;
    bool   context_processing = false;  // true while prompt is being ingested
    uint64_t context_total = 0;         // total tokens in the current prompt

    // Throughput (tokens/sec)
    float prompt_eval_tps = 0.0f;
    float eval_tps = 0.0f;

    // Token counters
    uint64_t total_tokens_processed = 0;
    uint64_t prompt_tokens = 0;
    uint64_t generated_tokens = 0;

    // Request tracking
    uint64_t total_requests = 0;
    uint64_t completed_requests = 0;
    int active_slots = 0;
    int n_slots = 0;             // total available slots

    // Timing
    double uptime_seconds = 0.0;

    // KV cache (if reported)
    float kv_cache_usage = 0.0f; // 0..1

    // Memory (approximate from log parsing)
    float model_load_seconds = 0.0f;
    uint64_t model_memory_bytes = 0;
    
    // Loading state
    bool model_loaded = false;
};

class ServerInstance {
public:
    LlamaConfig config;
    std::atomic<bool> is_running{false};
    pid_t process_id = -1;
    bool attached = false;               // true = attached to externally-started server (not fork/exec by us)
    /** Set when attaching (or refreshed at stop): PID owning TCP LISTEN on config.port — used to stop external llama-server. */
    pid_t external_listen_pid = -1;
    std::deque<std::string> logs;
    mutable std::mutex log_mutex;        // mutable so get_logs() (const) can lock it
    std::thread log_thread;
    std::thread health_thread;           // HTTP polls /health,/slots,/metrics (spawn + attach modes)
    std::atomic<bool> stop_threads{false};
    std::string log_file_path;

    // Live metrics (atomic for lock-free reads from UI thread)
    std::atomic<float> current_progress{0.0f};       // model loading progress (0..1)
    std::atomic<float> context_progress{0.0f};        // prompt processing progress (0..1)
    std::atomic<bool>  context_processing{false};     // true while ingesting prompt
    std::atomic<uint64_t> context_total{0};           // total tokens in current prompt
    std::atomic<float> prompt_eval_tps{0.0f};
    std::atomic<float> eval_tps{0.0f};
    std::atomic<uint64_t> total_tokens_processed{0};
    std::atomic<uint64_t> prompt_tokens{0};
    std::atomic<uint64_t> generated_tokens{0};
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> completed_requests{0};
    std::atomic<int> active_slots{0};
    std::atomic<int> n_slots{0};
    /** Set when GET /metrics returns llamacpp: counters — log tail skips additive token parses to avoid double-counting */
    std::atomic<bool> metrics_from_prometheus{false};
    std::atomic<float> kv_cache_usage{0.0f};
    std::atomic<float> model_load_seconds{0.0f};
    std::atomic<uint64_t> model_memory_bytes{0};

    // Start time for uptime
    std::chrono::steady_clock::time_point start_time;

    ServerInstance(const LlamaConfig& cfg);
    ~ServerInstance();

    void start();                        // launch new process
    void start_attached();               // attach to existing server on port (no process spawn)
    void stop();
    void tail_logs();
    void poll_health();                  // localhost HTTP (/health,/slots,/metrics — optional --metrics)


    // Snapshot all metrics atomically (lock-free)
    // Model loading state
    std::atomic<bool> model_loaded{false};  // true when model is done loading

    ServerMetrics snapshot() const;

    // Async shutdown: set when user clicks Stop — UI shows status until drain removes the node
    std::atomic<bool> stop_requested{false};
    /** Set on UI thread right before async stop worker starts — used only for elapsed time display */
    mutable std::chrono::steady_clock::time_point stop_requested_at{};
    std::atomic<int> shutdown_phase{0};  // ShutdownUiPhase; see LlamaManager.cpp for values

    const char* shutdown_status_text() const;
    float stop_elapsed_seconds() const;

    // Thread-safe log access: returns a copy of the log lines under mutex
    std::deque<std::string> get_logs() const {
        std::lock_guard<std::mutex> lock(log_mutex);
        return logs;
    }
};

class LlamaManager {
public:
    std::vector<LlamaConfig> saved_configs;
    std::vector<ServerInstance*> running_servers;
    std::string last_launch_error;   // stores the last error message from launch_server()

    void load_configs(const std::string& filename = "configs.json");
    void save_configs(const std::string& filename = "configs.json");
    bool launch_server(const LlamaConfig& config);   // returns true on success

    // Stops subprocess tree in a worker thread; call drain_async_stop_completions() once per frame from the UI thread
    void request_stop_server(ServerInstance* instance);
    void drain_async_stop_completions();

    // Auto-detect externally-running servers on configured ports
    void attach_all();
    bool attach_to_port(int port);   // returns true if attached successfully
    
    // Port conflict check
    static bool is_port_in_use(int port);

    /** PIDs with a TCP LISTEN socket on port (typically one). Uses `lsof` — required to stop externally launched servers */
    static std::vector<pid_t> pids_listen_on_tcp_port(int port);

    /** Human-readable rows parsed from CUSTOM ARGUMENTS (for dashboard / editor summaries) */
    static std::vector<std::pair<std::string, std::string>> summarize_launch_args(const std::string& custom_args_only);

private:
    std::mutex async_stop_completion_mutex_;
    std::vector<ServerInstance*> async_stop_completed_;
};
