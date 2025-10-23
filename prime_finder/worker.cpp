#include <iostream>
#include <string>
#include <sstream>
#include <unordered_set>
#include <unistd.h>
#include <ctime>

extern "C" {
    #include "primality_test.h"
}

#include "afs_client.h"

struct Task {
    int task_id;
    std::string filename;
    long start_line;
    long num_lines;
    std::string status;
};

// parse task from content
bool parse_task(const std::string& content, Task& task) {
    std::istringstream iss(content);
    std::string line;
    
    if (!std::getline(iss, line)) return false;
    task.task_id = std::stoi(line);
    
    if (!std::getline(iss, task.filename)) return false;
    
    if (!std::getline(iss, line)) return false;
    task.start_line = std::stol(line);
    
    if (!std::getline(iss, line)) return false;
    task.num_lines = std::stol(line);
    
    if (!std::getline(iss, task.status)) return false;
    
    return true;
}

// try to claim a pending task
bool claim_task(AFSClient& client, int task_id, Task& task) {
    std::string task_filename = "task_" + std::to_string(task_id) + ".txt";
    std::string content;
    
    if (!client.read_file(task_filename, content, 1)) {
        return false;
    }
    
    if (!parse_task(content, task)) {
        return false;
    }
    
    // only claim if pending
    if (task.status != "pending") {
        return false;
    }
    
    // update status to processing
    std::ostringstream new_content;
    new_content << task.task_id << "\n"
                << task.filename << "\n"
                << task.start_line << "\n"
                << task.num_lines << "\n"
                << "processing\n"
                << "worker_pid=" << getpid() << "\n"
                << "started_at=" << time(nullptr) << "\n";
    
    // simplified version - proper locking would be better
    if (!client.write_file(task_filename, new_content.str(), 1)) {
        return false;
    }
    task.status = "processing";
    return true;
}

// process single task
bool process_task(AFSClient& client, const Task& task) {
    std::cout << "Processing task " << task.task_id 
              << ": " << task.filename << " [" << task.start_line 
              << "-" << (task.start_line + task.num_lines) << "]" << std::endl;
    
    // read input file
    std::string content;
    if (!client.read_file(task.filename, content, 0)) {
        std::cerr << "Failed to read: " << task.filename << std::endl;
        return false;
    }
    
    // parse lines
    std::istringstream iss(content);
    std::string line;
    long current_line = 0;
    std::ostringstream results;
    
    // skip to start_line
    while (current_line < task.start_line && std::getline(iss, line)){
        current_line++;
    }
    
    // process chunk
    int processed = 0;
    while (processed < task.num_lines && std::getline(iss, line)){
        if (line.empty()) continue;
        
        uint64_t num = std::stoull(line);
        if (is_prime(num)) {
            results << num << "\n";
        }
        processed++;
    }
    
    // save results
    std::string result_filename = "result_task_" + std::to_string(task.task_id) + ".txt";
    if (!client.write_file(result_filename, results.str(), 1)) {
        std::cerr << "Failed to write results for task " << task.task_id << std::endl;
        return false;
    }
    
    // mark completed
    std::ostringstream task_update;
    task_update << task.task_id << "\n"
                << task.filename << "\n"
                << task.start_line << "\n"
                << task.num_lines << "\n"
                << "completed\n"
                << "worker_pid=" << getpid() << "\n"
                << "completed_at=" << time(nullptr) << "\n";
    
    std::string task_filename = "task_" + std::to_string(task.task_id) + ".txt";
    client.write_file(task_filename, task_update.str(), 1);
    
    return true;
}

// main work loop
int work_loop(AFSClient& client, int max_tasks) {
    int tasks_completed = 0;
    int consecutive_failures = 0;
    const int MAX_FAILURES = 10;
    
    std::cout << "Worker " << getpid() << " started" << std::endl;
    
    while (consecutive_failures < MAX_FAILURES){
        bool found_task = false;
        
        // look for available task
        for (int task_id = 0; task_id < max_tasks; task_id++){
            Task task;
            if (claim_task(client, task_id, task)){
                found_task = true;
                consecutive_failures = 0;
                
                if (process_task(client, task)){
                    tasks_completed++;
                }
                
                break;
            }
        }
        
        if (!found_task){
            consecutive_failures++;
            if (consecutive_failures % 3 == 0){  // print every 3 failures - Open to Modify 
                std::cout << "No tasks available, retrying... (" 
                          << consecutive_failures << "/" << MAX_FAILURES << ")" << std::endl;
            }
            sleep(2);
        }
    }
    
    std::cout << "Worker finished: " << tasks_completed << " tasks completed" << std::endl;
    
    return tasks_completed;
}

int main(int argc, char* argv[]) 
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <server_address>" << std::endl;
        return 1;
    }
    
    std::string server_address = argv[1];
    
    AFSClient client(server_address);
    
    // get total tasks from metadata
    std::string metadata_content;
    int max_tasks = 1000;  // default
    
    if (client.read_file("task_metadata.txt", metadata_content, 1)) {
        size_t pos = metadata_content.find("total_tasks=");
        if (pos != std::string::npos) {
            std::string value = metadata_content.substr(pos + 12);
            max_tasks = std::stoi(value.substr(0, value.find('\n')));
        }
    }
    
    work_loop(client, max_tasks);
    
    return 0;
}