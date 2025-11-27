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


class FileSystem final : public afs_operation::operators::Service{

public: 
    std::string root_dir = "/Users/ericzhang/Documents/Filesystems/Filesystem_server";
    void RunServer();

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










