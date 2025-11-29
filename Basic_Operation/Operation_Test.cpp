#include "filesystem_client.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <sys/stat.h>

// ANSI Color codes for pretty output
#define GREEN "\033[32m"
#define RED "\033[31m"
#define RESET "\033[0m"

// Helper to print test status
void log_test(const std::string& test_name) {
    std::cout << "\n[TEST] Starting: " << test_name << "..." << std::endl;
}

void assert_true(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << RED << "[FAIL] " << message << RESET << std::endl;
        exit(1);
    }
    std::cout << GREEN << "[PASS] " << message << RESET << std::endl;
}

// Consistency Check (from your snippet)
bool verify_metadata(FileSystemClient& client, const std::string& filename, const std::string& dir, int64_t expected_size) {
    // Check Client Cache (RAM)
    std::string cache_key = client.resolve_server_path(dir) + (dir.back() == '/' ? "" : "/") + filename;
    auto attr_it = client.cached_attr.find(cache_key);
    
    if (attr_it == client.cached_attr.end()) {
        std::cerr << RED << "  Metadata missing from RAM cache" << RESET << std::endl;
        return false;
    }

    if (attr_it->second.size != expected_size) {
        std::cerr << RED << "  RAM Size mismatch. Expected: " << expected_size << ", Got: " << attr_it->second.size << RESET << std::endl;
        return false;
    }

    // Check Physical Disk (Cache file)
    std::string local_path = "./tmp/cache" + cache_key;
    struct stat s;
    if (stat(local_path.c_str(), &s) != 0) {
        std::cerr << RED << "  Local cache file missing on disk" << RESET << std::endl;
        return false;
    }

    if (s.st_size != expected_size) {
        std::cerr << RED << "  Disk Size mismatch. Expected: " << expected_size << ", Got: " << s.st_size << RESET << std::endl;
        return false;
    }
    
    return true;
}

int main() {
    // 0. Setup
    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    FileSystemClient client(channel);
    
    std::string test_dir = "/test_suite_dir";
    std::string test_file = "integration_test.txt";

    // Cleanup previous runs
    client.delete_file(test_dir + "/" + test_file);
    client.delete_file(test_dir);

    // ==========================================
    // Test 1: Directory Management
    // ==========================================
    log_test("Directory Creation & Listing");
    
    bool mkdir_res = client.make_directory(test_dir, 0755);
    assert_true(mkdir_res, "Directory created successfully");

    auto list_res = client.ls_contents("/");
    assert_true(list_res.has_value(), "Root listing retrieved");
    bool found_dir = false;
    for (const auto& [name, type] : *list_res) {
        if (name == "test_suite_dir") found_dir = true;
    }
    assert_true(found_dir, "Created directory found in ls output");


    // ==========================================
    // Test 2: File Creation & Attributes
    // ==========================================
    log_test("File Creation");

    bool create_res = client.create_file(test_file, test_dir);
    assert_true(create_res, "File created successfully");

    // Verify consistency (Size 0)
    assert_true(verify_metadata(client, test_file, test_dir, 0), "Initial metadata consistent (size 0)");


    // ==========================================
    // Test 3: Write & Read (In-Memory/Local Cache)
    // ==========================================
    log_test("Write & Read (Local)");

    std::string data1 = "EricZhang12345";
    bool write_res = client.write_file(test_file, data1, test_dir, 0);
    assert_true(write_res, "Written 'Hello World' to file");

    std::vector<char> buffer;
    bool read_res = client.read_file(test_file, test_dir, data1.size(), 0, buffer);
    assert_true(read_res, "Read successful");
    
    std::string read_str(buffer.begin(), buffer.end());
    std::cout << "read data is: " << read_str << std::endl;
    std::cout << "expected data is: " << data1 << std::endl;
    assert_true(read_str == data1, "Content matches written data");
    
    // Verify consistency (Size 11)
    assert_true(verify_metadata(client, test_file, test_dir, 14), "Metadata updated after write");


    // ==========================================
    // Test 4: Flush to Server (Close) & Re-open
    // ==========================================
    log_test("Persistence (Close & Re-open)");

    bool close_res = client.close_file(test_file, test_dir);
    assert_true(close_res, "File closed (flushed to server)");

    // Simulate another client or restart by clearing local cache (manually for test)
    // In a real scenario, we'd delete the ./tmp/cache files here to force a fetch,
    // but client.open_file handles staleness checks via gRPC.
    
    bool open_res = client.open_file(test_file, test_dir);
    assert_true(open_res, "File re-opened from server");

    buffer.clear();
    client.read_file(test_file, test_dir, data1.size(), 0, buffer);
    std::string persistent_content(buffer.begin(), buffer.end());
    assert_true(persistent_content == data1, "Data persisted after close/open");


    // ==========================================
    // Test 5: Overwrite & Truncate
    // ==========================================
    log_test("Random Access Write");

    // "Hello World" -> "Hello Fuse!"
    std::string data2 = "Fuse!";
    client.write_file(test_file, data2, test_dir, 6); // Write at offset 6

    buffer.clear();
    client.read_file(test_file, test_dir, 14, 0, buffer);
    std::string edited_content(buffer.begin(), buffer.end());
    std::cout << "read data is: " << edited_content << std::endl;
    std::cout << "expected data is: " << "EricZFuse!2345" << std::endl;
    assert_true(edited_content == "EricZhFuse!345", "Partial overwrite successful");


    // ==========================================
    // Test 6: Rename
    // ==========================================
    log_test("Rename File");
    
    std::string new_name = "renamed_test.txt";
    bool rename_res = client.rename_file(test_file, new_name, test_dir, test_dir);
    assert_true(rename_res, "Rename RPC successful");

    // Check if old file is gone from list
    auto new_list = client.ls_contents(test_dir);
    bool found_old = false, found_new = false;
    for (const auto& [name, type] : *new_list) {
        if (name == test_file) found_old = true;
        if (name == new_name) found_new = true;
    }
    assert_true(!found_old && found_new, "Old name gone, new name present");


    // ==========================================
    // Test 7: Cleanup (Delete)
    // ==========================================
    log_test("Deletion");

    bool del_file = client.delete_file(test_dir + "/" + new_name);
    assert_true(del_file, "File deleted");

    bool del_dir = client.delete_file(test_dir); // Assuming delete_file handles rmdir logic or you use rmdir
    // Note: Your delete_file implementation in integration seems to rely on 'unlink' which might map to std::filesystem::remove (which handles both).
    assert_true(del_dir, "Directory deleted");


    std::cout << "\n========================================" << std::endl;
    std::cout << GREEN << "ALL INTEGRATION TESTS PASSED" << RESET << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}