#include "filesystem_client.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

// ANSI Color codes for pretty output
#define GREEN "\033[32m"
#define BLUE "\033[34m"
#define YELLOW "\033[33m"
#define RED "\033[31m"
#define RESET "\033[0m"

void log_action(const std::string& message) {
    std::cout << BLUE << "[CLIENT 1] " << RESET << message << std::endl;
}

void log_success(const std::string& message) {
    std::cout << GREEN << "[CLIENT 1 SUCCESS] " << message << RESET << std::endl;
}

void log_error(const std::string& message) {
    std::cerr << RED << "[CLIENT 1 ERROR] " << message << RESET << std::endl;
}

int main() {
    try {
        // Get server address from environment variable, default to localhost
        const char* server_env = std::getenv("SERVER_ADDRESS");
        std::string server_address = server_env ? server_env : "localhost:50051";

        log_action("Connecting to server at " + server_address);
        std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
            server_address,
            grpc::InsecureChannelCredentials()
        );

        // Create client with unique cache directory
        FileSystemClient client(channel, "./tmp/client1_cache");

        std::string directory = "/test";
        std::string filename = "file1.txt";

        log_action("Starting concurrent test - 5 write operations every 5 seconds");

        // Perform 5 iterations of writes every 5 seconds
        for (int i = 1; i <= 5; i++) {
            log_action("========== ITERATION " + std::to_string(i) + " ==========");

            // Write to file1.txt
            std::string write_content = "Client1_Iteration_" + std::to_string(i) + "_Data";
            log_action("Opening file: " + filename);

            if (!client.open_file(filename, directory)) {
                log_error("Failed to open file");
                return 1;
            }

            log_action("Writing: " + write_content);
            if (!client.write_file(filename, write_content, directory, 0)) {
                log_error("Failed to write to file");
                return 1;
            }

            log_action("Closing file after write");
            if (!client.close_file(filename, directory)) {
                log_error("Failed to close file");
                return 1;
            }

            log_success("Write completed");

            // Wait 5 seconds before next write
            if (i < 5) {
                log_action("Waiting 5 seconds before next write...");
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        log_success("All 5 iterations completed successfully!");
        return 0;

    } catch (const std::exception& e) {
        log_error(std::string("Exception: ") + e.what());
        return 1;
    }
}
