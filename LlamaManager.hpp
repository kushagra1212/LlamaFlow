#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
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

class ServerInstance {
public:
    LlamaConfig config;
    std::atomic<bool> is_running{false};
    pid_t process_id = -1;
    std::deque<std::string> logs;
    std::mutex log_mutex;
    std::thread log_thread;
    std::string log_file_path;

    ServerInstance(const LlamaConfig& cfg);
    ~ServerInstance();

    void start();
    void stop();
    void tail_logs();
};

class LlamaManager {
public:
    std::vector<LlamaConfig> saved_configs;
    std::vector<ServerInstance*> running_servers;

    void load_configs(const std::string& filename = "configs.json");
    void save_configs(const std::string& filename = "configs.json");
    void launch_server(const LlamaConfig& config);
    void stop_server(int index);
};