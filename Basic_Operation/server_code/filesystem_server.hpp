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

// Unused in the class, but part of your original file
struct FileInfo{
    std::string filename;
    std::string server_path;
    int64_t timestamp;
};


class FileSystem final : public afs_operation::operators::Service{

public: 

    void RunServer();

private:

    grpc::Status request_dir(grpc::ServerContext* context, const afs_operation::InitialiseRequest* request, afs_operation::InitialiseResponse* response) override;

    grpc::Status open(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter<afs_operation::FileResponse>* writer) override;

    grpc::Status close(grpc::ServerContext* context, grpc::ServerReader<afs_operation::FileRequest>* reader, afs_operation::FileResponse* response) override;

    grpc::Status compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter< ::afs_operation::FileResponse>* writer) override;

    grpc::Status ls(grpc::ServerContext* context, const afs_operation::ListDirectoryRequest* request, afs_operation::ListDirectoryResponse* response) override;
};










