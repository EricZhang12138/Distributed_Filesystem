#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_set>
#include <ctime>
#include <unistd.h>
#include <dirent.h>
#include <cstring>

extern "C" {
    #include "primality_test.h"
}

#include "afs_client.h"

#define CHUNK_SIZE 100

struct Task {
    int task_id;
    std::string filename;
    long start_line;
    long num_lines;
    std::string status;  // pending/processing/completed
};

// count newlines
long count_lines(const std::string& content) {
    long count = 0;
    for (char c : content) {
        if (c == '\n') count++;
    }
    return count;
}

// scan directory for .txt files
std::vector<std::string> scan_local_input_files(const std::string& input_dir) {
    std::vector<std::string> files;
    DIR* dir = opendir(input_dir.c_str());
    
    if (!dir) {
        std::cerr << "Error: Could not open directory '" << input_dir << "'" << std::endl;
        return files;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        if (entry->d_type == DT_REG) {
            std::string filename = entry->d_name;
            if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".txt") {
                files.push_back(filename);
            }
        }
    }
    
    closedir(dir);
    return files;
}

// split files into tasks and save to AFS
int create_task_queue(AFSClient& client, const std::string& input_dir) {
    std::cout << "\n=== Creating Task Queue ===" << std::endl;
    
    std::vector<std::string> input_files = scan_local_input_files(input_dir);
    
    if (input_files.empty()) {
        std::cerr << "Error: No input files found" << std::endl;
        return -1;
    }
    
    std::cout << "Found " << input_files.size() << " files" << std::endl;
    
    // split into tasks
    std::vector<Task> tasks;
    int task_id = 0;
    
    for (const auto& filename : input_files) {
        std::string content;
        if (!client.read_file(filename, content, 0)) {
            std::cerr << "Failed to read: " << filename << std::endl;
            continue;
        }
        
        long total_lines = count_lines(content);
        
        // chunk into CHUNK_SIZE pieces
        for (long pos = 0; pos < total_lines; pos += CHUNK_SIZE) {
            Task task;
            task.task_id = task_id++;
            task.filename = filename;
            task.start_line = pos;
            task.num_lines = std::min((long)CHUNK_SIZE, total_lines - pos);
            task.status = "pending";
            tasks.push_back(task);
        }
    }
    
    std::cout << "Created " << tasks.size() << " tasks" << std::endl;
    
    // write tasks to AFS
    for (const auto& task : tasks) {
        std::ostringstream task_content;
        task_content << task.task_id << "\n"
                     << task.filename << "\n"
                     << task.start_line << "\n"
                     << task.num_lines << "\n"
                     << task.status << "\n";
        
        std::string task_filename = "task_" + std::to_string(task.task_id) + ".txt";
        if (!client.write_file(task_filename, task_content.str(), 1)) {
            std::cerr << "Failed to write task: " << task_filename << std::endl;
        }
    }
    
    // save metadata
    std::ostringstream metadata;
    metadata << "total_tasks=" << tasks.size() << "\n";
    metadata << "completed_tasks=0\n";
    metadata << "created_at=" << time(nullptr) << "\n";
    
    client.write_file("task_metadata.txt", metadata.str(), 1);
    
    return tasks.size();
}

// check progress periodically
void monitor_progress(AFSClient& client, int total_tasks) {
    std::cout << "\n=== Monitoring Progress ===" << std::endl;
    std::cout << "Total: " << total_tasks << " tasks" << std::endl;
    
    int last_completed = 0;
    while (true) {
        sleep(5); 
        
        // count how many are done
        int completed = 0;
        for (int i = 0; i < total_tasks; i++) {
            std::string task_filename = "task_" + std::to_string(i) + ".txt";
            std::string content;
            if (client.read_file(task_filename, content, 1)) {
                if (content.find("completed") != std::string::npos) {
                    completed++;
                }
            }
        }
        
        if (completed != last_completed) {
            std::cout << "Progress: " << completed << "/" << total_tasks 
                      << " (" << (completed * 100 / total_tasks) << "%)" << std::endl;
            last_completed = completed;
        }
        
        if (completed >= total_tasks) {
            break;
        }
    }
}

// collect results and check duplicates
void aggregate_results(AFSClient& client, int total_tasks, const std::string& output_file) 
{
    std::cout << "\n=== Aggregating Results ===" << std::endl;
    
    std::unordered_set<uint64_t> unique_primes;
    int files_read = 0;
    int files_missing = 0;
    int invalid_lines = 0;
    
    for (int task_id = 0; task_id < total_tasks; task_id++) {
        std::string result_filename = "result_task_" + std::to_string(task_id) + ".txt";
        std::string content;
        
        if (client.read_file(result_filename, content, 1)) {
            files_read++;
            
            // parse primes
            std::istringstream iss(content);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) {
                    try {
                        uint64_t prime = std::stoull(line);
                        unique_primes.insert(prime);
                    } catch (const std::exception& e) {
                        invalid_lines++;
                    }
                }
            }
        } else {
            files_missing++;
        }
        
        // show progress every 100 files - Open to Modify
        if ((task_id + 1) % 100 == 0 || task_id == total_tasks - 1) {
            std::cout << "Processed: " << (task_id + 1) << "/" << total_tasks << std::endl;
        }
    }
    
    std::cout << "Read: " << files_read << ", Missing: " << files_missing << std::endl;
    if (invalid_lines > 0) {
        std::cout << "Skipped " << invalid_lines << " invalid lines" << std::endl;
    }
    std::cout << "Unique primes: " << unique_primes.size() << std::endl;
    
    // write final output
    std::ostringstream final_output;
    for (uint64_t prime : unique_primes) {
        final_output << prime << "\n";
    }
    
    if (!client.write_file(output_file, final_output.str(), 1)) {
        std::cerr << "Failed to write output" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <server_address> <input_dir>" << std::endl;
        return 1;
    }
    
    std::string server_address = argv[1];
    std::string input_dir = argv[2];
    
    std::cout << "=== Distributed Prime Finder ===" << std::endl;
    
    clock_t start_time = clock();
    
    AFSClient client(server_address);
    
    int total_tasks = create_task_queue(client, input_dir);
    if (total_tasks <= 0) {
        return 1;
    }
    
    monitor_progress(client, total_tasks);
    
    aggregate_results(client, total_tasks, "primes_output.txt");
    
    clock_t end_time = clock();
    double elapsed_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    
    std::cout << "\nDone in " << elapsed_time << " seconds" << std::endl;
    
    return 0;
}