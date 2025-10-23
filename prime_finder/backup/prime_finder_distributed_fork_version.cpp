#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <unistd.h>
#include <sys/wait.h>
#include <ctime>
#include <dirent.h>
#include <cstring>

extern "C" {
    #include "primality_test.h"
}

#include "afs_client.h"

#define CHUNK_SIZE 100

struct Task {
    std::string filename;
    long start_line;
    long num_lines;
    int worker_id;
};

// Count lines in a string content
long count_lines(const std::string& content) {
    long count = 0;
    for (char c : content) {
        if (c == '\n') count++;
    }
    return count;
}

// Scan local test_data directory for input files
std::vector<std::string> scan_local_input_files(const std::string& input_dir) {
    std::vector<std::string> files;
    DIR* dir = opendir(input_dir.c_str());
    
    if (!dir) {
        std::cerr << "Warning: Could not open directory '" << input_dir 
                  << "'. Will try to use files from AFS directly." << std::endl;
        return files;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and .. directories
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Only process regular files (not directories)
        if (entry->d_type == DT_REG) {
            std::string filename = entry->d_name;
            // Only process .txt files
            if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".txt") {
                files.push_back(filename);
                std::cout << "Found input file: " << filename << std::endl;
            }
        }
    }
    
    closedir(dir);
    return files;
}

// Worker function: process a chunk of lines from input file
void do_work(const Task& task, const std::string& server_address, int worker_slot) {
    // Each worker creates its own AFS client
    AFSClient client(server_address);
    
    std::cout << "[Worker " << worker_slot << " PID:" << getpid() << "] Processing " 
              << task.filename << " from line " << task.start_line << std::endl;
    
    // Read the input file from AFS
    std::string content;
    if (!client.read_file(task.filename, content, 0)) {
        std::cerr << "[Worker " << worker_slot << "] Failed to read input file" << std::endl;
        return;
    }
    
    // Parse content line by line
    std::istringstream iss(content);
    std::string line;
    long current_line = 0;
    std::ostringstream results;
    
    // Skip to start_line
    while (current_line < task.start_line && std::getline(iss, line)) {
        current_line++;
    }
    
    // Process num_lines
    int processed = 0;
    while (processed < task.num_lines && std::getline(iss, line)) {
        if (line.empty()) continue;
        
        uint64_t num = std::stoull(line);
        if (is_prime(num)) {
            results << num << "\n";
        }
        processed++;
        current_line++;
    }
    
    // Write results to AFS (use worker_slot as file identifier)
    if (results.str().length() > 0) {
        std::string temp_filename = "temp_worker_" + std::to_string(worker_slot) + ".txt";
        if (!client.write_file(temp_filename, results.str(), 1)) {
            std::cerr << "[Worker " << worker_slot << "] Failed to write results" << std::endl;
        }
    }
    
    std::cout << "[Worker " << worker_slot << "] Finished processing " 
              << processed << " lines" << std::endl;
}

// Create tasks from input files on AFS
std::vector<Task> create_tasks(AFSClient& client, const std::vector<std::string>& filenames) {
    std::vector<Task> tasks;
    
    for (const auto& filename : filenames) {
        std::string content;
        if (!client.read_file(filename, content, 0)) {
            std::cerr << "Failed to read file: " << filename << std::endl;
            continue;
        }
        
        long total_lines = count_lines(content);
        std::cout << "File '" << filename << "' has " << total_lines << " lines" << std::endl;
        
        // Create tasks with CHUNK_SIZE lines each
        for (long pos = 0; pos < total_lines; pos += CHUNK_SIZE) {
            Task task;
            task.filename = filename;
            task.start_line = pos;
            task.num_lines = std::min((long)CHUNK_SIZE, total_lines - pos);
            task.worker_id = tasks.size();
            tasks.push_back(task);
        }
    }
    
    return tasks;
}

// Collect results from all temp files and remove duplicates
void collect_results(AFSClient& client, const std::string& output_file, int num_workers) {
    std::unordered_set<uint64_t> unique_primes;
    
    std::cout << "Collecting results from " << num_workers << " workers..." << std::endl;
    
    // Read all temp_worker_*.txt files
    for (int i = 0; i < num_workers; i++) {
        std::string temp_filename = "temp_worker_" + std::to_string(i) + ".txt";
        std::string content;
        
        if (client.read_file(temp_filename, content, 1)) {
            std::istringstream iss(content);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) {
                    uint64_t prime = std::stoull(line);
                    unique_primes.insert(prime);
                }
            }
        }
    }
    
    std::cout << "Found " << unique_primes.size() << " unique primes" << std::endl;
    
    // Write final results to output file
    std::ostringstream final_output;
    for (uint64_t prime : unique_primes) {
        final_output << prime << "\n";
    }
    
    if (!client.write_file(output_file, final_output.str(), 1)) {
        std::cerr << "Failed to write final output file" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <num_workers> <server_address> <input_dir>" << std::endl;
        std::cerr << "Example: " << argv[0] << " 4 localhost:50051 test_data" << std::endl;
        std::cerr << "" << std::endl;
        std::cerr << "  num_workers   - Number of worker processes" << std::endl;
        std::cerr << "  server_address - AFS server address (e.g. localhost:50051)" << std::endl;
        std::cerr << "  input_dir     - Local directory containing input files" << std::endl;
        return 1;
    }
    
    int num_workers = std::atoi(argv[1]);
    std::string server_address = argv[2];
    std::string input_dir = argv[3];
    
    if (num_workers <= 0) {
        std::cerr << "Error: Number of workers must be positive" << std::endl;
        return 1;
    }
    
    std::cout << "=== Distributed Prime Finder ===" << std::endl;
    std::cout << "Workers: " << num_workers << std::endl;
    std::cout << "AFS Server: " << server_address << std::endl;
    std::cout << "Input Directory: " << input_dir << std::endl;
    
    clock_t start_time = clock();
    
    // Create AFS client for master process
    AFSClient master_client(server_address);
    
    // Scan local directory for input files
    std::cout << "\nScanning local directory for input files..." << std::endl;
    std::vector<std::string> input_files = scan_local_input_files(input_dir);
    
    if (input_files.empty()) {
        std::cerr << "Error: No input files found in '" << input_dir << "'" << std::endl;
        return 1;
    }
    
    std::cout << "Found " << input_files.size() << " input file(s)" << std::endl;
    
    // Create tasks from input files
    std::cout << "\nReading files from AFS and creating tasks..." << std::endl;
    std::vector<Task> tasks = create_tasks(master_client, input_files);
    
    if (tasks.empty()) {
        std::cerr << "No tasks created. Make sure input files exist on AFS server." << std::endl;
        return 1;
    }
    
    std::cout << "Created " << tasks.size() << " tasks" << std::endl;
    
    // Fork workers to process tasks
    std::cout << "\nSpawning " << num_workers << " workers..." << std::endl;
    
    int workers_spawned = 0;
    int max_parallel = std::min(num_workers, (int)tasks.size());
    
    for (int i = 0; i < max_parallel; i++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // Child process: process assigned tasks
            size_t my_idx = i;
            while (my_idx < tasks.size()) {
                Task task = tasks[my_idx];
                do_work(task, server_address, i);  // Pass worker slot number
                my_idx += max_parallel;
            }
            exit(0);
        } else if (pid < 0) {
            std::cerr << "Fork failed" << std::endl;
            exit(1);
        } else {
            workers_spawned++;
        }
    }
    
    // Wait for all workers to complete
    std::cout << "Waiting for workers to complete..." << std::endl;
    for (int i = 0; i < workers_spawned; i++) {
        wait(NULL);
    }
    
    std::cout << "\nAll workers finished. Aggregating results..." << std::endl;
    
    // Collect and deduplicate results
    std::string output_file = "primes_output.txt";
    collect_results(master_client, output_file, workers_spawned);
    
    clock_t end_time = clock();
    double elapsed = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    
    std::cout << "\n=== DONE ===" << std::endl;
    std::cout << "Results written to '" << output_file << "' on AFS" << std::endl;
    std::cout << "Total time: " << elapsed << " seconds" << std::endl;
    
    return 0;
}

