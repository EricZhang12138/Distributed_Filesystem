#ifndef AFS_CLIENT_H
#define AFS_CLIENT_H

#include <string>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"

// AFS Client for prime_finder - basic file operations via RPC
class AFSClient {
private:
    std::unique_ptr<afs_operation::operators::Stub> stub_;
    std::string input_path_;
    std::string output_path_;
    
public:
    AFSClient(const std::string& server_address);
    
    // Read file from server
    // path_select: 0=input, 1=output
    bool read_file(const std::string& filename, std::string& content, int path_select = 0);
    
    // Write to file (overwrites if exists)
    bool write_file(const std::string& filename, const std::string& content, int path_select = 1);
    
    // Append data to existing file
    bool append_file(const std::string& filename, const std::string& content, int path_select = 1);
    
    const std::string& get_input_path() const { return input_path_; }
    const std::string& get_output_path() const { return output_path_; }
};

#endif