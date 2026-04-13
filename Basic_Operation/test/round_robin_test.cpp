#include "filesystem_client.hpp"
#include <iostream>
#include <filesystem>

#define GREEN "\033[32m"
#define RED   "\033[31m"
#define RESET "\033[0m"

void assert_true(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << RED << "[FAIL] " << message << RESET << std::endl;
        exit(1);
    }
    std::cout << GREEN << "[PASS] " << message << RESET << std::endl;
}

int main() {
    std::string ds1 = "/Users/ericzhang/Documents/Filesystems/Test_env/Filesystem_server";
    std::string ds2 = "/Users/ericzhang/Documents/Filesystems/Test_env/Filesystem_server_2";

    auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
    FileSystemClient client(channel, "./tmp/rr_cache");

    std::cout << "\n=== Round-Robin Placement Test ===" << std::endl;

    // Create file A
    assert_true(client.create_file("rr_file_a.txt", "/"), "Created rr_file_a.txt");
    client.close_file("rr_file_a.txt", "/");

    // Create file B
    assert_true(client.create_file("rr_file_b.txt", "/"), "Created rr_file_b.txt");
    client.close_file("rr_file_b.txt", "/");

    // Create file C
    assert_true(client.create_file("rr_file_c.txt", "/"), "Created rr_file_c.txt");
    client.close_file("rr_file_c.txt", "/");

    // Check which data server each file ended up on
    bool a_on_ds1 = std::filesystem::exists(ds1 + "/rr_file_a.txt");
    bool a_on_ds2 = std::filesystem::exists(ds2 + "/rr_file_a.txt");
    bool b_on_ds1 = std::filesystem::exists(ds1 + "/rr_file_b.txt");
    bool b_on_ds2 = std::filesystem::exists(ds2 + "/rr_file_b.txt");
    bool c_on_ds1 = std::filesystem::exists(ds1 + "/rr_file_c.txt");
    bool c_on_ds2 = std::filesystem::exists(ds2 + "/rr_file_c.txt");

    std::cout << "\nPlacement results:" << std::endl;
    std::cout << "  rr_file_a.txt: DS1=" << a_on_ds1 << " DS2=" << a_on_ds2 << std::endl;
    std::cout << "  rr_file_b.txt: DS1=" << b_on_ds1 << " DS2=" << b_on_ds2 << std::endl;
    std::cout << "  rr_file_c.txt: DS1=" << c_on_ds1 << " DS2=" << c_on_ds2 << std::endl;

    // Each file should exist on exactly one server
    assert_true((a_on_ds1 ^ a_on_ds2), "File A on exactly one server");
    assert_true((b_on_ds1 ^ b_on_ds2), "File B on exactly one server");
    assert_true((c_on_ds1 ^ c_on_ds2), "File C on exactly one server");

    // Files should be spread across both servers
    int ds1_count = a_on_ds1 + b_on_ds1 + c_on_ds1;
    int ds2_count = a_on_ds2 + b_on_ds2 + c_on_ds2;
    std::cout << "\n  DS1 has " << ds1_count << " files, DS2 has " << ds2_count << " files" << std::endl;
    assert_true(ds1_count > 0 && ds2_count > 0, "Files distributed across both servers");

    // Verify ls from metadata server shows all files
    auto contents = client.ls_contents("/");
    assert_true(contents.has_value(), "ls root succeeded");
    bool found_a = false, found_b = false, found_c = false;
    for (auto& [name, type] : *contents) {
        if (name == "rr_file_a.txt") found_a = true;
        if (name == "rr_file_b.txt") found_b = true;
        if (name == "rr_file_c.txt") found_c = true;
    }
    assert_true(found_a && found_b && found_c, "All 3 files visible in merged ls");

    // Cleanup
    client.delete_file("/rr_file_a.txt");
    client.delete_file("/rr_file_b.txt");
    client.delete_file("/rr_file_c.txt");

    std::cout << "\n========================================" << std::endl;
    std::cout << GREEN << "ROUND-ROBIN TEST PASSED" << RESET << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
