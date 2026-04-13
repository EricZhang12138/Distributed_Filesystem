#pragma once

#include <string>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <memory>
#include <vector>
#include <queue>
#include <condition_variable>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include "metadata.grpc.pb.h"
#include "metadata.pb.h"
#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"
#include "file_attributes.hpp"

// === Namespace tree node ===
struct MetaNode {
    std::string name;               // e.g., "report.txt"
    bool is_directory;
    FileAttributes attrs;
    std::string data_server_address; // which server holds this file (empty for dirs)
    std::string symlink_target;
    std::unordered_map<std::string, std::shared_ptr<MetaNode>> children; // dirs only

    MetaNode() : is_directory(false), attrs{} {}
};

// === Data server registry entry ===
struct DataServerEntry {
    std::string address;
    std::string root_dir;
    std::shared_ptr<afs_operation::operators::Stub> stub;
    std::chrono::steady_clock::time_point last_heartbeat;
    bool alive = true;
};

// === Notification queue (copied from filesystem_server.hpp) ===
struct MetaNotificationQueue {
    std::queue<afs_operation::Notification> queue;
    std::mutex mu;
    std::condition_variable cv;
    bool shutdown = false;

    void push(afs_operation::Notification notif) {
        std::lock_guard<std::mutex> lock(mu);
        queue.push(notif);
        cv.notify_one();
    }

    bool pop(afs_operation::Notification& notif) {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [this]() { return !queue.empty() || shutdown; });
        if (shutdown && queue.empty()) return false;
        notif = queue.front();
        queue.pop();
        return true;
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mu);
        shutdown = true;
        cv.notify_all();
    }
};

// === Metadata server ===
class MetadataServer final : public metadata::MetadataService::Service {
public:
    MetadataServer(int port);
    void RunServer();

private:
    int port_;

    // --- Namespace tree ---
    std::shared_ptr<MetaNode> root_node_;        // Root of the virtual namespace tree
    std::mutex tree_mutex_;                       // Protects the entire tree

    // Helper: resolve a path like "/subdir/file.txt" to its MetaNode (nullptr if not found)
    std::shared_ptr<MetaNode> resolve_node(const std::string& path);
    // Helper: resolve the parent of a path and return (parent_node, child_name)
    std::pair<std::shared_ptr<MetaNode>, std::string> resolve_parent(const std::string& path);
    // Helper: split path into components
    std::vector<std::string> split_path(const std::string& path);
    // Helper: translate a virtual path to a data-server-local path
    std::string to_ds_path(const std::string& ds_root, const std::string& virtual_path);

    // --- Multi-server registry ---
    std::mutex ds_mutex_;
    std::vector<DataServerEntry> data_servers_;
    size_t next_server_idx_ = 0;

    // Helper: get stub for a specific data server address
    afs_operation::operators::Stub* get_stub_for(const std::string& address);
    // Helper: pick the next data server (round-robin)
    DataServerEntry* pick_next_server();

    // --- Connected clients ---
    std::mutex clients_mutex_;
    std::unordered_set<std::string> connected_clients_;

    // --- Subscriber system ---
    std::mutex subscriber_mutex_;
    std::unordered_map<std::string, std::shared_ptr<MetaNotificationQueue>> subscribers_;
    // path -> set of client_ids who have the file cached
    std::unordered_map<std::string, std::unordered_set<std::string>> client_file_map_;

    void push_notification(const std::string& path,
                           const std::string& exclude_client_id,
                           const afs_operation::Notification& notif);
    void cleanup_client(const std::string& client_id);

    // --- Tree population ---
    void populate_tree_from_server(DataServerEntry& entry);
    void populate_recursive(const std::string& ds_path,
                            const std::string& ds_root,
                            std::shared_ptr<MetaNode> parent,
                            afs_operation::operators::Stub* stub,
                            const std::string& ds_address);

    // === Metadata-specific RPCs ===
    grpc::Status Init(grpc::ServerContext* context,
                      const metadata::MetadataInitRequest* request,
                      metadata::MetadataInitResponse* response) override;

    grpc::Status Lookup(grpc::ServerContext* context,
                        const metadata::LookupRequest* request,
                        metadata::LookupResponse* response) override;

    grpc::Status CreateFile(grpc::ServerContext* context,
                            const metadata::CreateFileRequest* request,
                            metadata::CreateFileResponse* response) override;

    grpc::Status RegisterDataServer(grpc::ServerContext* context,
                                    const metadata::RegisterRequest* request,
                                    metadata::RegisterResponse* response) override;

    grpc::Status Heartbeat(grpc::ServerContext* context,
                           const metadata::HeartbeatRequest* request,
                           metadata::HeartbeatResponse* response) override;

    grpc::Status Subscribe(grpc::ServerContext* context,
                           const afs_operation::SubscribeRequest* request,
                           grpc::ServerWriter<afs_operation::Notification>* writer) override;

    grpc::Status NotifyFileChange(grpc::ServerContext* context,
                                  const metadata::NotifyFileChangeRequest* request,
                                  metadata::NotifyFileChangeResponse* response) override;

    grpc::Status RegisterFileInterest(grpc::ServerContext* context,
                                      const metadata::RegisterFileInterestRequest* request,
                                      metadata::RegisterFileInterestResponse* response) override;

    // === Structural RPCs — handled locally via namespace tree ===
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
