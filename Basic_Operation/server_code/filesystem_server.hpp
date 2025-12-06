#pragma once

#include <string>
#include <vector>
#include <filesystem> 

// gRPC and Protobuf includes
#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"



struct FID {
    int Volume_number;       // I only want the root, different user folders within usr and the projects folder to have a seperate volume number
                                // which are (root_dir), (root_dir/usr/Eric   root_dir/usr/Brian ...), root_dir/projects
    int Vnode_number;
    int uniquifier;

    // operator== ---> operator overloading (required for unordered_map)
    bool operator==(const FID& other) const {
        return Volume_number == other.Volume_number &&
               Vnode_number == other.Vnode_number &&
               uniquifier == other.uniquifier;
    }
}; 

// template specialisation: Specializing the std::hash template for FID (required for unordered_map)
namespace std {
    template <>
    struct hash<FID> {
        size_t operator()(const FID& fid) const {
            // Combine hashes of all three fields
            size_t h1 = hash<int>()(fid.Volume_number);
            size_t h2 = hash<int>()(fid.Vnode_number);
            size_t h3 = hash<int>()(fid.uniquifier);
            
            // Combine using XOR and bit shifting
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };
}

class FileSystem final : public afs_operation::operators::Service{

public: 
    std::string root_dir;           // = "/Users/ericzhang/Documents/Filesystems/Filesystem_server";
    int starting_length;
    std::unordered_map<FID, std::string> file_map;
    void RunServer();
    FileSystem(std::string root_dir);
private:

    grpc::Status request_dir(grpc::ServerContext* context, const afs_operation::InitialiseRequest* request, afs_operation::InitialiseResponse* response) override;

    grpc::Status open(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter<afs_operation::FileResponse>* writer) override;

    grpc::Status close(grpc::ServerContext* context, grpc::ServerReader<afs_operation::FileRequest>* reader, afs_operation::FileResponse* response) override;

    grpc::Status compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter< ::afs_operation::FileResponse>* writer) override;

    grpc::Status ls(grpc::ServerContext* context, const afs_operation::ListDirectoryRequest* request, afs_operation::ListDirectoryResponse* response) override;

    grpc::Status getattr(grpc::ServerContext* context, const afs_operation::GetAttrRequest* request, afs_operation::GetAttrResponse* response) override;

    grpc::Status rename(grpc::ServerContext* context, const afs_operation::RenameRequest* request, afs_operation::RenameResponse* response) override;

    grpc::Status mkdir(grpc::ServerContext* context, const afs_operation::MakeDir_request* request, afs_operation::MakeDir_response* response) override;

    grpc::Status unlink(grpc::ServerContext* context, const afs_operation::Delete_request* request, afs_operation::Delete_response* response) override;
};










