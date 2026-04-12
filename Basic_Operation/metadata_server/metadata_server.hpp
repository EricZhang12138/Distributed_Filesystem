#pragma once

#include <string>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include "metadata.grpc.pb.h"
#include "metadata.pb.h"
#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"

struct DataServerInfo {
    std::string address;
    std::string root_dir;
    std::chrono::steady_clock::time_point last_heartbeat;
};

class MetadataServer final : public metadata::MetadataService::Service {
public:
    MetadataServer(int port);
    void RunServer();

private:
    int port_;

    // Data server info (single data server in Phase 1)
    std::mutex ds_mutex_;
    DataServerInfo data_server_;
    bool data_server_registered_ = false;

    // Connected clients
    std::mutex clients_mutex_;
    std::unordered_set<std::string> connected_clients_;

    // Stub to the single data server (shared_ptr so in-flight RPCs survive re-registration)
    std::shared_ptr<afs_operation::operators::Stub> data_stub_;

    // Helper: returns a shared_ptr copy that stays alive for the duration of the RPC
    std::shared_ptr<afs_operation::operators::Stub> get_data_stub();

    // === Metadata-specific RPCs ===
    grpc::Status Init(grpc::ServerContext* context,
                      const metadata::MetadataInitRequest* request,
                      metadata::MetadataInitResponse* response) override;

    grpc::Status Lookup(grpc::ServerContext* context,
                        const metadata::LookupRequest* request,
                        metadata::LookupResponse* response) override;

    grpc::Status RegisterDataServer(grpc::ServerContext* context,
                                    const metadata::RegisterRequest* request,
                                    metadata::RegisterResponse* response) override;

    grpc::Status Heartbeat(grpc::ServerContext* context,
                           const metadata::HeartbeatRequest* request,
                           metadata::HeartbeatResponse* response) override;

    // === Forwarded structural RPCs ===
    grpc::Status ls(grpc::ServerContext*, const afs_operation::ListDirectoryRequest*, afs_operation::ListDirectoryResponse*) override;
    grpc::Status getattr(grpc::ServerContext*, const afs_operation::GetAttrRequest*, afs_operation::GetAttrResponse*) override;
    grpc::Status mkdir(grpc::ServerContext*, const afs_operation::MakeDir_request*, afs_operation::MakeDir_response*) override;
    grpc::Status unlink(grpc::ServerContext*, const afs_operation::Delete_request*, afs_operation::Delete_response*) override;
    grpc::Status rmdir(grpc::ServerContext*, const afs_operation::Rmdir_request*, afs_operation::Rmdir_response*) override;
    grpc::Status rename(grpc::ServerContext*, const afs_operation::RenameRequest*, afs_operation::RenameResponse*) override;
    grpc::Status chmod(grpc::ServerContext*, const afs_operation::Chmod_request*, afs_operation::Chmod_response*) override;
    grpc::Status chown(grpc::ServerContext*, const afs_operation::Chown_request*, afs_operation::Chown_response*) override;
    grpc::Status utimens(grpc::ServerContext*, const afs_operation::Utimens_request*, afs_operation::Utimens_response*) override;
    grpc::Status symlink(grpc::ServerContext*, const afs_operation::Symlink_request*, afs_operation::Symlink_response*) override;
    grpc::Status readlink(grpc::ServerContext*, const afs_operation::Readlink_request*, afs_operation::Readlink_response*) override;
    grpc::Status statfs(grpc::ServerContext*, const afs_operation::Statfs_request*, afs_operation::Statfs_response*) override;
    grpc::Status setxattr(grpc::ServerContext*, const afs_operation::SetXattr_request*, afs_operation::SetXattr_response*) override;
    grpc::Status getxattr(grpc::ServerContext*, const afs_operation::GetXattr_request*, afs_operation::GetXattr_response*) override;
    grpc::Status listxattr(grpc::ServerContext*, const afs_operation::ListXattr_request*, afs_operation::ListXattr_response*) override;
    grpc::Status removexattr(grpc::ServerContext*, const afs_operation::RemoveXattr_request*, afs_operation::RemoveXattr_response*) override;
};
