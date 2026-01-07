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


int main(){
    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    FileSystemClient* client_1 = new FileSystemClient(channel, "./tmp1/cache");
    FileSystemClient* client_2 = new FileSystemClient(channel, "./tmp2/cache");

    std::string content = "This is my test";
    std::string directory = "/test";
    std::string filename = "Eric.txt";
    std::string new_filename = "Cotton.txt";

    client_1 -> create_file(filename, directory);
    client_1 -> open_file(filename, directory);
    client_1 -> write_file(filename, content, directory, 0);
    client_1 -> close_file(filename, directory);

    client_2 -> open_file(filename, directory);
    std::vector<char> buffer;
    client_2 -> write_file(filename, "Hi", directory, 15);
    bool res = client_2 -> read_file(filename, directory, 17, 0, buffer);
    assert_true(res, "client_2 successfully read the file");
    std::string buffer_str(buffer.begin(), buffer.end());
    bool comp =  std::string("This is my testHi") == buffer_str;
    std::cout << "The read value by client 2 is: " << buffer_str << std::endl;
    assert_true(comp, "client 2 read the value and it matches the correct content");
    client_2 ->close_file(filename, directory);
    
   std::this_thread::sleep_for(std::chrono::seconds(2));

    std::vector<char> buffer_1;
    client_1 -> open_file(filename, directory);
    client_1 -> read_file(filename, directory, 17, 0, buffer_1);
    std::string buffer_1_str(buffer_1.begin(), buffer_1.end());
    bool comp_1 = std::string("This is my testHi") == buffer_1_str;
    std::cout << "The read value by client_1 is: " << buffer_1_str << std::endl;
    assert_true(comp_1, "The register callback worked and client is now reading the value that client 2 wrote to the server");
}