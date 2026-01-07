#include "filesystem_client.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

// ANSI Color codes for pretty output
#define GREEN "\033[32m"
#define CYAN "\033[36m"
#define YELLOW "\033[33m"
#define RED "\033[31m"
#define RESET "\033[0m"

void log_action(const std::string& message) {
    std::cout << CYAN << "[CLIENT 2] " << RESET << message << std::endl;
}

void log_success(const std::string& message) {
    std::cout << GREEN << "[CLIENT 2 SUCCESS] " << message << RESET << std::endl;
}

void log_error(const std::string& message) {
    std::cerr << RED << "[CLIENT 2 ERROR] " << message << RESET << std::endl;
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
        FileSystemClient client(channel, "./tmp/client2_cache");

        std::string directory = "/test";
        std::string filename = "file1.txt";

        // Wait 1 second initially to let client 1 write first (1s delay from client 1's write)
        log_action("Initial delay - waiting 1 second after client 1's first write...");
        std::this_thread::sleep_for(std::chrono::seconds(1));

        log_action("Starting concurrent test - 5 read operations every 5 seconds");

        // Perform 5 iterations of reads every 5 seconds (starting 1s after each client 1 write)
        for (int i = 1; i <= 5; i++) {
            log_action("========== ITERATION " + std::to_string(i) + " ==========");

            // Expected content from client 1 for this iteration
            std::string expected_content = "Client1_Iteration_" + std::to_string(i) + "_Data";
            log_action("Expected content: " + expected_content);

            // Read from file1.txt
            log_action("Opening file for read");
            if (!client.open_file(filename, directory)) {
                log_error("Failed to open file for read");
                return 1;
            }

            std::vector<char> buffer;
            // Read up to 1000 bytes
            if (!client.read_file(filename, directory, 1000, 0, buffer)) {
                log_error("Failed to read from file");
                return 1;
            }

            std::string content(buffer.begin(), buffer.end());
            log_action("Actual content read: " + content);

            log_action("Closing file after read");
            if (!client.close_file(filename, directory)) {
                log_error("Failed to close file after read");
                return 1;
            }

            // Verify content matches expected
            if (content != expected_content) {
                log_error("Content mismatch!");
                log_error("Expected: " + expected_content);
                log_error("Got: " + content);
                return 1;
            }

            log_success("Content verification passed: " + content);

            // Wait 5 seconds before next read
            if (i < 5) {
                log_action("Waiting 5 seconds before next read...");
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
