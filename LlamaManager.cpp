#include "LlamaManager.hpp"
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

ServerInstance::ServerInstance(const LlamaConfig& cfg) : config(cfg) {
    log_file_path = "/tmp/llama_log_" + std::to_string(config.port) + ".txt";
}

ServerInstance::~ServerInstance() {
    stop();
}

void ServerInstance::start() {
    if (is_running) return;

    // Build the command, e.g., passing all custom args seamlessly
    std::string command = config.exec_path + " -m " + config.model_path + 
                          " --port " + std::to_string(config.port) + " " + 
                          config.custom_args + " > " + log_file_path + " 2>&1 & echo $!";

    // Launch and capture PID
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return;
    
    char buffer[128];
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        process_id = std::stoi(buffer);
    }
    pclose(pipe);

    is_running = true;
    log_thread = std::thread(&ServerInstance::tail_logs, this);
}

void ServerInstance::stop() {
    if (!is_running) return;
    is_running = false;
    
    if (process_id > 0) {
        kill(process_id, SIGTERM);
    }
    
    if (log_thread.joinable()) {
        log_thread.join();
    }
}

void ServerInstance::tail_logs() {
    std::ifstream file;
    std::string line;
    
    while (is_running) {
        if (!file.is_open()) {
            file.open(log_file_path);
        }
        
        if (file.is_open()) {
            while (std::getline(file, line)) {
                std::lock_guard<std::mutex> lock(log_mutex);
                logs.push_back(line);
                if (logs.size() > 500) logs.pop_front(); // Keep last 500 lines
            }
            file.clear(); // Clear EOF flag to wait for more data
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
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

void LlamaManager::launch_server(const LlamaConfig& config) {
    ServerInstance* instance = new ServerInstance(config);
    instance->start();
    running_servers.push_back(instance);
}

void LlamaManager::stop_server(int index) {
    if (index >= 0 && index < running_servers.size()) {
        delete running_servers[index];
        running_servers.erase(running_servers.begin() + index);
    }
}