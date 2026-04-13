#include "metadata_server.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>

MetadataServer::MetadataServer(int port) : port_(port) {
    // Initialize root node
    root_node_ = std::make_shared<MetaNode>();
    root_node_->name = "";
    root_node_->is_directory = true;
    root_node_->attrs.mode = S_IFDIR | 0755;
    root_node_->attrs.nlink = 2;
    root_node_->attrs.uid = 0;
    root_node_->attrs.gid = 0;
    root_node_->attrs.size = 4096;
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    root_node_->attrs.atime = now_ns;
    root_node_->attrs.mtime = now_ns;
    root_node_->attrs.ctime = now_ns;
}

// === Helpers ===

std::vector<std::string> MetadataServer::split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::istringstream ss(path);
    std::string token;
    while (std::getline(ss, token, '/')) {
        if (!token.empty()) {
            parts.push_back(token);
        }
    }
    return parts;
}

std::shared_ptr<MetaNode> MetadataServer::resolve_node(const std::string& path) {
    // tree_mutex_ must be held by caller
    auto parts = split_path(path);
    auto node = root_node_;
    for (const auto& part : parts) {
        if (!node->is_directory) return nullptr;
        auto it = node->children.find(part);
        if (it == node->children.end()) return nullptr;
        node = it->second;
    }
    return node;
}

std::pair<std::shared_ptr<MetaNode>, std::string> MetadataServer::resolve_parent(const std::string& path) {
    // tree_mutex_ must be held by caller
    auto parts = split_path(path);
    if (parts.empty()) return {nullptr, ""};
    std::string child_name = parts.back();
    parts.pop_back();

    auto node = root_node_;
    for (const auto& part : parts) {
        if (!node->is_directory) return {nullptr, ""};
        auto it = node->children.find(part);
        if (it == node->children.end()) return {nullptr, ""};
        node = it->second;
    }
    return {node, child_name};
}

std::string MetadataServer::to_ds_path(const std::string& ds_root, const std::string& virtual_path) {
    // virtual_path is like "/subdir/file.txt"
    // ds_root is like "/Users/ericzhang/server_a"
    // result: "/Users/ericzhang/server_a/subdir/file.txt"
    if (virtual_path.empty() || virtual_path == "/") {
        return ds_root;
    }
    if (ds_root.back() == '/' && virtual_path.front() == '/') {
        return ds_root + virtual_path.substr(1);
    }
    if (ds_root.back() != '/' && virtual_path.front() != '/') {
        return ds_root + "/" + virtual_path;
    }
    return ds_root + virtual_path;
}

afs_operation::operators::Stub* MetadataServer::get_stub_for(const std::string& address) {
    // ds_mutex_ must be held by caller
    for (auto& entry : data_servers_) {
        if (entry.address == address && entry.alive) {
            return entry.stub.get();
        }
    }
    return nullptr;
}

DataServerEntry* MetadataServer::pick_next_server() {
    // ds_mutex_ must be held by caller
    if (data_servers_.empty()) return nullptr;
    // Round-robin, skip dead servers
    for (size_t i = 0; i < data_servers_.size(); i++) {
        size_t idx = (next_server_idx_ + i) % data_servers_.size();
        if (data_servers_[idx].alive) {
            next_server_idx_ = (idx + 1) % data_servers_.size();
            return &data_servers_[idx];
        }
    }
    return nullptr;
}

void MetadataServer::push_notification(const std::string& path,
                                       const std::string& exclude_client_id,
                                       const afs_operation::Notification& notif) {
    // subscriber_mutex_ must be held by caller
    auto it = client_file_map_.find(path);
    if (it == client_file_map_.end()) return;
    for (const auto& client_id : it->second) {
        if (client_id == exclude_client_id) continue;
        auto q_it = subscribers_.find(client_id);
        if (q_it != subscribers_.end()) {
            q_it->second->push(notif);
        }
    }
}

void MetadataServer::cleanup_client(const std::string& client_id) {
    std::cout << "[META] Cleaning up client: " << client_id << std::endl;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        connected_clients_.erase(client_id);
    }
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        // Remove from client_file_map_
        for (auto it = client_file_map_.begin(); it != client_file_map_.end(); ) {
            it->second.erase(client_id);
            if (it->second.empty()) {
                it = client_file_map_.erase(it);
            } else {
                ++it;
            }
        }
        // Remove subscriber queue
        auto q_it = subscribers_.find(client_id);
        if (q_it != subscribers_.end()) {
            q_it->second->cancel();
            subscribers_.erase(q_it);
        }
    }
}

// === Tree population ===

void MetadataServer::populate_tree_from_server(DataServerEntry& entry) {
    std::cout << "[META] Populating tree from data server: " << entry.address
              << " root=" << entry.root_dir << std::endl;
    // tree_mutex_ must be held by caller
    populate_recursive(entry.root_dir, entry.root_dir, root_node_, entry.stub.get(), entry.address);
    std::cout << "[META] Tree population complete for " << entry.address << std::endl;
}

void MetadataServer::populate_recursive(const std::string& ds_path,
                                        const std::string& ds_root,
                                        std::shared_ptr<MetaNode> parent,
                                        afs_operation::operators::Stub* stub,
                                        const std::string& ds_address) {
    // List directory contents on the data server
    grpc::ClientContext ls_ctx;
    afs_operation::ListDirectoryRequest ls_req;
    afs_operation::ListDirectoryResponse ls_resp;
    ls_req.set_directory(ds_path);

    grpc::Status status = stub->ls(&ls_ctx, ls_req, &ls_resp);
    if (!status.ok()) {
        std::cerr << "[META] ls failed for " << ds_path << ": " << status.error_message() << std::endl;
        return;
    }

    for (const auto& [name, type] : ls_resp.entry_list()) {
        // Get attributes
        grpc::ClientContext attr_ctx;
        afs_operation::GetAttrRequest attr_req;
        afs_operation::GetAttrResponse attr_resp;
        attr_req.set_directory(ds_path);
        attr_req.set_filename(name);

        grpc::Status attr_status = stub->getattr(&attr_ctx, attr_req, &attr_resp);

        auto child = std::make_shared<MetaNode>();
        child->name = name;
        child->is_directory = (type == "Directory");

        if (attr_status.ok()) {
            child->attrs.size = attr_resp.size();
            child->attrs.mtime = attr_resp.mtime();
            child->attrs.atime = attr_resp.atime();
            child->attrs.ctime = attr_resp.ctime();
            child->attrs.mode = attr_resp.mode();
            child->attrs.nlink = attr_resp.nlink();
            child->attrs.uid = attr_resp.uid();
            child->attrs.gid = attr_resp.gid();
        } else {
            // Default attrs
            child->attrs.mode = child->is_directory ? (S_IFDIR | 0755) : (S_IFREG | 0644);
            child->attrs.nlink = child->is_directory ? 2 : 1;
        }

        if (!child->is_directory) {
            child->data_server_address = ds_address;
        }

        // Check for symlinks
        if (attr_status.ok() && S_ISLNK(attr_resp.mode())) {
            grpc::ClientContext rl_ctx;
            afs_operation::Readlink_request rl_req;
            afs_operation::Readlink_response rl_resp;
            rl_req.set_directory(ds_path);
            rl_req.set_filename(name);
            if (stub->readlink(&rl_ctx, rl_req, &rl_resp).ok() && rl_resp.success()) {
                child->symlink_target = rl_resp.target();
            }
        }

        parent->children[name] = child;

        // Recurse into directories
        if (child->is_directory) {
            std::string child_ds_path = ds_path + (ds_path.back() == '/' ? "" : "/") + name;
            populate_recursive(child_ds_path, ds_root, child, stub, ds_address);
        }
    }
}

void MetadataServer::RunServer() {
    std::string server_address = "0.0.0.0:" + std::to_string(port_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Metadata server listening on " << server_address << std::endl;

    server->Wait();
}


// === Metadata-specific RPCs ===

grpc::Status MetadataServer::Init(grpc::ServerContext* context,
                                  const metadata::MetadataInitRequest* request,
                                  metadata::MetadataInitResponse* response) {
    std::string client_id = request->client_id();
    std::cout << "[META] Init from client: " << client_id << std::endl;

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        connected_clients_.insert(client_id);
    }

    std::lock_guard<std::mutex> lock(ds_mutex_);
    if (data_servers_.empty()) {
        std::cerr << "[META] No data server registered yet!" << std::endl;
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server registered");
    }

    // Phase 2: return "/" as the virtual root — clients discover data servers per-file
    response->set_root_path("/");
    return grpc::Status::OK;
}

grpc::Status MetadataServer::Lookup(grpc::ServerContext* context,
                                    const metadata::LookupRequest* request,
                                    metadata::LookupResponse* response) {
    std::string path = request->path();
    std::lock_guard<std::mutex> lock(tree_mutex_);
    auto node = resolve_node(path);
    if (!node) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found in namespace tree");
    }
    if (node->data_server_address.empty()) {
        // It's a directory — not hosted on a specific server
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Path is a directory, not a file");
    }
    response->set_data_server_address(node->data_server_address);

    // Return the data server's root_dir so the client can build full paths
    {
        std::lock_guard<std::mutex> ds_lock(ds_mutex_);
        for (const auto& entry : data_servers_) {
            if (entry.address == node->data_server_address) {
                response->set_data_server_root(entry.root_dir);
                break;
            }
        }
    }
    return grpc::Status::OK;
}

grpc::Status MetadataServer::CreateFile(grpc::ServerContext* context,
                                        const metadata::CreateFileRequest* request,
                                        metadata::CreateFileResponse* response) {
    std::string path = request->path();
    uint32_t mode = request->mode();
    std::string client_id = request->client_id();

    std::cout << "[META] CreateFile: " << path << " from client " << client_id << std::endl;

    // Pick a data server
    DataServerEntry* ds;
    {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        ds = pick_next_server();
        if (!ds) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server available");
        }
    }

    // Add to the namespace tree
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto [parent, child_name] = resolve_parent(path);
        if (!parent || !parent->is_directory) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Parent directory not found");
        }
        if (parent->children.count(child_name)) {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "File already exists");
        }

        auto node = std::make_shared<MetaNode>();
        node->name = child_name;
        node->is_directory = false;
        node->data_server_address = ds->address;
        node->attrs.mode = S_IFREG | (mode & 0777);
        node->attrs.nlink = 1;
        node->attrs.size = 0;
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        node->attrs.atime = now_ns;
        node->attrs.mtime = now_ns;
        node->attrs.ctime = now_ns;

        parent->children[child_name] = node;
    }

    response->set_data_server_address(ds->address);
    response->set_data_server_root(ds->root_dir);
    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::RegisterDataServer(grpc::ServerContext* context,
                                                const metadata::RegisterRequest* request,
                                                metadata::RegisterResponse* response) {
    std::string address = request->address();
    std::string root_dir = request->root_dir();

    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto stub = afs_operation::operators::NewStub(channel);

    DataServerEntry entry;
    entry.address = address;
    entry.root_dir = root_dir;
    entry.stub = std::move(stub);
    entry.last_heartbeat = std::chrono::steady_clock::now();
    entry.alive = true;

    // Populate tree from this server
    {
        std::lock_guard<std::mutex> tree_lock(tree_mutex_);
        populate_tree_from_server(entry);
    }

    {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        // Check if this server is already registered (re-registration)
        bool found = false;
        for (auto& existing : data_servers_) {
            if (existing.address == address) {
                existing.root_dir = root_dir;
                existing.stub = entry.stub;
                existing.last_heartbeat = entry.last_heartbeat;
                existing.alive = true;
                found = true;
                break;
            }
        }
        if (!found) {
            data_servers_.push_back(std::move(entry));
        }
    }

    std::cout << "[META] Data server registered: " << address
              << " root=" << root_dir << std::endl;

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::Heartbeat(grpc::ServerContext* context,
                                       const metadata::HeartbeatRequest* request,
                                       metadata::HeartbeatResponse* response) {
    std::lock_guard<std::mutex> lock(ds_mutex_);
    for (auto& entry : data_servers_) {
        if (entry.address == request->address()) {
            entry.last_heartbeat = std::chrono::steady_clock::now();
            entry.alive = true;
            response->set_success(true);
            return grpc::Status::OK;
        }
    }
    response->set_success(false);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::Subscribe(grpc::ServerContext* context,
                                       const afs_operation::SubscribeRequest* request,
                                       grpc::ServerWriter<afs_operation::Notification>* writer) {
    std::string client_id = request->client_id();
    std::cout << "[META] Client subscribed: " << client_id << std::endl;

    auto queue = std::make_shared<MetaNotificationQueue>();
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        subscribers_[client_id] = queue;
    }

    // Monitor thread — cancel queue when client disconnects
    std::thread monitor([context, queue, client_id]() {
        while (!context->IsCancelled()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        std::cout << "[META] Client " << client_id << " context cancelled" << std::endl;
        queue->cancel();
    });

    // Block, popping notifications and writing to stream
    afs_operation::Notification note;
    while (queue->pop(note)) {
        if (!writer->Write(note)) {
            std::cout << "[META] Client disconnected: " << client_id << std::endl;
            break;
        }
    }

    cleanup_client(client_id);
    monitor.join();
    return grpc::Status::OK;
}

grpc::Status MetadataServer::NotifyFileChange(grpc::ServerContext* context,
                                              const metadata::NotifyFileChangeRequest* request,
                                              metadata::NotifyFileChangeResponse* response) {
    std::string path = request->path();
    std::string message = request->message();
    int64_t timestamp = request->timestamp();
    std::string client_id = request->client_id();
    int64_t new_size = request->new_size();
    std::string new_path = request->new_path();

    std::cout << "[META] NotifyFileChange: " << message << " " << path << std::endl;

    // Update the tree
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        if (message == "DELETE") {
            auto [parent, child_name] = resolve_parent(path);
            if (parent) {
                parent->children.erase(child_name);
            }
        } else if (message == "RENAME" || message == "Rename") {
            // Move node from old path to new path
            auto [old_parent, old_name] = resolve_parent(path);
            auto [new_parent, new_name] = resolve_parent(new_path);
            if (old_parent && new_parent) {
                auto it = old_parent->children.find(old_name);
                if (it != old_parent->children.end()) {
                    auto node = it->second;
                    node->name = new_name;
                    new_parent->children[new_name] = node;
                    old_parent->children.erase(it);
                }
            }
        } else {
            // UPDATE — refresh attrs
            auto node = resolve_node(path);
            if (node) {
                if (timestamp > 0) {
                    node->attrs.mtime = timestamp;
                    node->attrs.ctime = timestamp;
                }
                if (new_size >= 0) {
                    node->attrs.size = new_size;
                }
            }
        }
    }

    // Build notification and push to interested clients
    afs_operation::Notification notif;
    notif.set_directory(path);
    notif.set_message(message);
    notif.set_timestamp(timestamp);
    if (!new_path.empty()) {
        notif.set_new_directory(new_path);
    }

    {
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        push_notification(path, client_id, notif);
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::RegisterFileInterest(grpc::ServerContext* context,
                                                  const metadata::RegisterFileInterestRequest* request,
                                                  metadata::RegisterFileInterestResponse* response) {
    std::string path = request->path();
    std::string client_id = request->client_id();

    {
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        client_file_map_[path].insert(client_id);
    }

    response->set_success(true);
    return grpc::Status::OK;
}


// === Structural RPCs — handled locally via namespace tree ===

grpc::Status MetadataServer::ls(grpc::ServerContext* server_ctx,
                                const afs_operation::ListDirectoryRequest* request,
                                afs_operation::ListDirectoryResponse* response) {
    std::string directory = request->directory();
    std::lock_guard<std::mutex> lock(tree_mutex_);
    auto node = resolve_node(directory);
    if (!node) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Directory not found");
    }
    if (!node->is_directory) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Path is not a directory");
    }

    auto* entry_map = response->mutable_entry_list();
    for (const auto& [name, child] : node->children) {
        (*entry_map)[name] = child->is_directory ? "Directory" : "Regular_File";
    }
    return grpc::Status::OK;
}

grpc::Status MetadataServer::getattr(grpc::ServerContext* server_ctx,
                                     const afs_operation::GetAttrRequest* request,
                                     afs_operation::GetAttrResponse* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();

    // Build the path to look up
    std::string path;
    if (filename.empty() || filename == ".") {
        path = directory;
    } else {
        path = directory + (directory.back() == '/' ? "" : "/") + filename;
    }

    std::lock_guard<std::mutex> lock(tree_mutex_);
    auto node = resolve_node(path);
    if (!node) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
    }

    response->set_size(node->attrs.size);
    response->set_mtime(node->attrs.mtime);
    response->set_atime(node->attrs.atime);
    response->set_ctime(node->attrs.ctime);
    response->set_mode(node->attrs.mode);
    response->set_nlink(node->attrs.nlink);
    response->set_uid(node->attrs.uid);
    response->set_gid(node->attrs.gid);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::mkdir(grpc::ServerContext* server_ctx,
                                   const afs_operation::MakeDir_request* request,
                                   afs_operation::MakeDir_response* response) {
    std::string directory = request->directory();
    uint32_t mode = request->mode();

    // Add to tree
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto [parent, child_name] = resolve_parent(directory);
        if (!parent || !parent->is_directory) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Parent directory not found");
        }
        // If already exists, just return OK (matching Phase 1 behavior)
        if (parent->children.count(child_name)) {
            auto existing = parent->children[child_name];
            if (existing->is_directory) {
                response->set_success(true);
                // Still forward to all data servers to ensure physical dirs exist
            } else {
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Path exists but is not a directory");
            }
        } else {
            auto node = std::make_shared<MetaNode>();
            node->name = child_name;
            node->is_directory = true;
            node->attrs.mode = S_IFDIR | (mode & 07777);
            node->attrs.nlink = 2;
            node->attrs.size = 4096;
            auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            node->attrs.atime = now_ns;
            node->attrs.mtime = now_ns;
            node->attrs.ctime = now_ns;
            parent->children[child_name] = node;
        }
    }

    // Forward to ALL data servers (so physical dirs exist for file I/O)
    {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        for (auto& entry : data_servers_) {
            if (!entry.alive) continue;
            grpc::ClientContext ctx;
            afs_operation::MakeDir_request ds_req;
            afs_operation::MakeDir_response ds_resp;
            ds_req.set_directory(to_ds_path(entry.root_dir, directory));
            ds_req.set_mode(mode);
            auto status = entry.stub->mkdir(&ctx, ds_req, &ds_resp);
            if (!status.ok()) {
                std::cerr << "[META] mkdir forward to " << entry.address << " failed: "
                          << status.error_message() << std::endl;
            }
        }
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::unlink(grpc::ServerContext* server_ctx,
                                    const afs_operation::Delete_request* request,
                                    afs_operation::Delete_response* response) {
    std::string directory = request->directory();
    std::string client_id = request->client_id();

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(directory);
        if (!node) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        }
        ds_address = node->data_server_address;

        // Remove from tree
        auto [parent, child_name] = resolve_parent(directory);
        if (parent) {
            parent->children.erase(child_name);
        }
    }

    // Forward to the owning data server
    if (!ds_address.empty()) {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        auto* stub = get_stub_for(ds_address);
        if (stub) {
            // Find the root_dir for this data server
            std::string ds_root;
            for (const auto& entry : data_servers_) {
                if (entry.address == ds_address) {
                    ds_root = entry.root_dir;
                    break;
                }
            }
            grpc::ClientContext ctx;
            afs_operation::Delete_request ds_req;
            afs_operation::Delete_response ds_resp;
            ds_req.set_directory(to_ds_path(ds_root, directory));
            ds_req.set_client_id(client_id);
            stub->unlink(&ctx, ds_req, &ds_resp);
        }
    }

    // Push notification
    {
        afs_operation::Notification notif;
        notif.set_directory(directory);
        notif.set_message("DELETE");
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        push_notification(directory, client_id, notif);
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::rmdir(grpc::ServerContext* server_ctx,
                                   const afs_operation::Rmdir_request* request,
                                   afs_operation::Rmdir_response* response) {
    std::string directory = request->directory();
    std::string client_id = request->client_id();

    // Check and remove from tree
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(directory);
        if (!node) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Directory not found");
        }
        if (!node->is_directory) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Path is not a directory");
        }
        if (!node->children.empty()) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Directory is not empty");
        }

        auto [parent, child_name] = resolve_parent(directory);
        if (parent) {
            parent->children.erase(child_name);
        }
    }

    // Forward to ALL data servers
    {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        for (auto& entry : data_servers_) {
            if (!entry.alive) continue;
            grpc::ClientContext ctx;
            afs_operation::Rmdir_request ds_req;
            afs_operation::Rmdir_response ds_resp;
            ds_req.set_directory(to_ds_path(entry.root_dir, directory));
            ds_req.set_client_id(client_id);
            entry.stub->rmdir(&ctx, ds_req, &ds_resp);
        }
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::rename(grpc::ServerContext* server_ctx,
                                    const afs_operation::RenameRequest* request,
                                    afs_operation::RenameResponse* response) {
    std::string directory = request->directory();
    std::string directory_new = request->new_directory();
    std::string filename = request->filename();
    std::string new_filename = request->new_filename();
    std::string client_id = request->client_id();

    std::string old_path = directory + (directory.back() == '/' ? "" : "/") + filename;
    std::string new_path = directory_new + (directory_new.back() == '/' ? "" : "/") + new_filename;

    std::string old_ds_address;
    std::string new_ds_address;

    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(old_path);
        if (!node) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Source file not found");
        }
        old_ds_address = node->data_server_address;

        // Remove from old parent
        auto [old_parent, old_name] = resolve_parent(old_path);
        auto [new_parent, new_name] = resolve_parent(new_path);
        if (!new_parent || !new_parent->is_directory) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Destination directory not found");
        }

        // If a file exists at destination, check if it's a file we can overwrite
        if (new_parent->children.count(new_name)) {
            auto dest = new_parent->children[new_name];
            if (dest->is_directory && !dest->children.empty()) {
                return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Target is non-empty directory");
            }
            new_ds_address = dest->data_server_address;
        }

        // Move node
        node->name = new_name;
        new_parent->children[new_name] = node;
        if (old_parent) {
            old_parent->children.erase(old_name);
        }

        new_ds_address = old_ds_address; // File stays on same server for now
    }

    // Forward rename to the owning data server
    if (!old_ds_address.empty()) {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        auto* stub = get_stub_for(old_ds_address);
        if (stub) {
            std::string ds_root;
            for (const auto& entry : data_servers_) {
                if (entry.address == old_ds_address) {
                    ds_root = entry.root_dir;
                    break;
                }
            }
            grpc::ClientContext ctx;
            afs_operation::RenameRequest ds_req;
            afs_operation::RenameResponse ds_resp;
            ds_req.set_filename(filename);
            ds_req.set_new_filename(new_filename);
            ds_req.set_directory(to_ds_path(ds_root, directory));
            ds_req.set_new_directory(to_ds_path(ds_root, directory_new));
            ds_req.set_client_id(client_id);
            stub->rename(&ctx, ds_req, &ds_resp);
        }
    } else {
        // Directory rename — forward to all data servers
        std::lock_guard<std::mutex> lock(ds_mutex_);
        for (auto& entry : data_servers_) {
            if (!entry.alive) continue;
            grpc::ClientContext ctx;
            afs_operation::RenameRequest ds_req;
            afs_operation::RenameResponse ds_resp;
            ds_req.set_filename(filename);
            ds_req.set_new_filename(new_filename);
            ds_req.set_directory(to_ds_path(entry.root_dir, directory));
            ds_req.set_new_directory(to_ds_path(entry.root_dir, directory_new));
            ds_req.set_client_id(client_id);
            entry.stub->rename(&ctx, ds_req, &ds_resp);
        }
    }

    // Notification
    {
        afs_operation::Notification notif;
        notif.set_directory(old_path);
        notif.set_new_directory(new_path);
        notif.set_message("Rename");
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        push_notification(old_path, client_id, notif);
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::chmod(grpc::ServerContext* server_ctx,
                                   const afs_operation::Chmod_request* request,
                                   afs_operation::Chmod_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;
    std::string client_id = request->client_id();
    uint32_t mode = request->mode();

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(path);
        if (!node) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        }
        // Update tree attrs — preserve file type bits
        node->attrs.mode = (node->attrs.mode & ~07777) | (mode & 07777);
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        node->attrs.ctime = now_ns;
        ds_address = node->data_server_address;
    }

    // Forward to owning data server
    int64_t ts = 0;
    if (!ds_address.empty()) {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        auto* stub = get_stub_for(ds_address);
        if (stub) {
            std::string ds_root;
            for (const auto& entry : data_servers_) {
                if (entry.address == ds_address) { ds_root = entry.root_dir; break; }
            }
            grpc::ClientContext ctx;
            afs_operation::Chmod_request ds_req;
            afs_operation::Chmod_response ds_resp;
            ds_req.set_directory(to_ds_path(ds_root, directory));
            ds_req.set_filename(filename);
            ds_req.set_mode(mode);
            ds_req.set_client_id(client_id);
            auto status = stub->chmod(&ctx, ds_req, &ds_resp);
            if (status.ok()) ts = ds_resp.timestamp();
        }
    }

    response->set_success(true);
    response->set_timestamp(ts);

    // Notification
    {
        afs_operation::Notification notif;
        notif.set_directory(path);
        notif.set_message("UPDATE");
        notif.set_timestamp(ts);
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        push_notification(path, client_id, notif);
    }

    return grpc::Status::OK;
}

grpc::Status MetadataServer::chown(grpc::ServerContext* server_ctx,
                                   const afs_operation::Chown_request* request,
                                   afs_operation::Chown_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;
    std::string client_id = request->client_id();

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(path);
        if (!node) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        }
        node->attrs.uid = request->uid();
        node->attrs.gid = request->gid();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        node->attrs.ctime = now_ns;
        ds_address = node->data_server_address;
    }

    int64_t ts = 0;
    if (!ds_address.empty()) {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        auto* stub = get_stub_for(ds_address);
        if (stub) {
            std::string ds_root;
            for (const auto& entry : data_servers_) {
                if (entry.address == ds_address) { ds_root = entry.root_dir; break; }
            }
            grpc::ClientContext ctx;
            afs_operation::Chown_request ds_req;
            afs_operation::Chown_response ds_resp;
            ds_req.set_directory(to_ds_path(ds_root, directory));
            ds_req.set_filename(filename);
            ds_req.set_uid(request->uid());
            ds_req.set_gid(request->gid());
            ds_req.set_client_id(client_id);
            auto status = stub->chown(&ctx, ds_req, &ds_resp);
            if (status.ok()) ts = ds_resp.timestamp();
        }
    }

    response->set_success(true);
    response->set_timestamp(ts);

    {
        afs_operation::Notification notif;
        notif.set_directory(path);
        notif.set_message("UPDATE");
        notif.set_timestamp(ts);
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        push_notification(path, client_id, notif);
    }

    return grpc::Status::OK;
}

grpc::Status MetadataServer::utimens(grpc::ServerContext* server_ctx,
                                     const afs_operation::Utimens_request* request,
                                     afs_operation::Utimens_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;
    std::string client_id = request->client_id();

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(path);
        if (!node) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        }
        node->attrs.atime = request->atime();
        node->attrs.mtime = request->mtime();
        ds_address = node->data_server_address;
    }

    if (!ds_address.empty()) {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        auto* stub = get_stub_for(ds_address);
        if (stub) {
            std::string ds_root;
            for (const auto& entry : data_servers_) {
                if (entry.address == ds_address) { ds_root = entry.root_dir; break; }
            }
            grpc::ClientContext ctx;
            afs_operation::Utimens_request ds_req;
            afs_operation::Utimens_response ds_resp;
            ds_req.set_directory(to_ds_path(ds_root, directory));
            ds_req.set_filename(filename);
            ds_req.set_atime(request->atime());
            ds_req.set_mtime(request->mtime());
            ds_req.set_client_id(client_id);
            stub->utimens(&ctx, ds_req, &ds_resp);
        }
    }

    response->set_success(true);

    {
        afs_operation::Notification notif;
        notif.set_directory(path);
        notif.set_message("UPDATE");
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        push_notification(path, client_id, notif);
    }

    return grpc::Status::OK;
}

grpc::Status MetadataServer::symlink(grpc::ServerContext* server_ctx,
                                     const afs_operation::Symlink_request* request,
                                     afs_operation::Symlink_response* response) {
    std::string target = request->target();
    std::string linkpath_dir = request->linkpath_dir();
    std::string linkpath_name = request->linkpath_name();
    std::string linkpath = linkpath_dir + (linkpath_dir.back() == '/' ? "" : "/") + linkpath_name;

    // Add symlink to tree
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto [parent, child_name] = resolve_parent(linkpath);
        if (!parent || !parent->is_directory) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Parent directory not found");
        }
        if (parent->children.count(child_name)) {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "Path already exists");
        }

        auto node = std::make_shared<MetaNode>();
        node->name = child_name;
        node->is_directory = false;
        node->symlink_target = target;
        node->attrs.mode = S_IFLNK | 0777;
        node->attrs.nlink = 1;
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        node->attrs.atime = now_ns;
        node->attrs.mtime = now_ns;
        node->attrs.ctime = now_ns;
        parent->children[child_name] = node;
    }

    // Forward to all data servers
    {
        std::lock_guard<std::mutex> lock(ds_mutex_);
        for (auto& entry : data_servers_) {
            if (!entry.alive) continue;
            grpc::ClientContext ctx;
            afs_operation::Symlink_request ds_req;
            afs_operation::Symlink_response ds_resp;
            ds_req.set_target(target);
            ds_req.set_linkpath_dir(to_ds_path(entry.root_dir, linkpath_dir));
            ds_req.set_linkpath_name(linkpath_name);
            ds_req.set_client_id(request->client_id());
            entry.stub->symlink(&ctx, ds_req, &ds_resp);
        }
    }

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::readlink(grpc::ServerContext* server_ctx,
                                      const afs_operation::Readlink_request* request,
                                      afs_operation::Readlink_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;

    std::lock_guard<std::mutex> lock(tree_mutex_);
    auto node = resolve_node(path);
    if (!node) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
    }
    if (node->symlink_target.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Not a symlink");
    }

    response->set_target(node->symlink_target);
    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::statfs(grpc::ServerContext* server_ctx,
                                    const afs_operation::Statfs_request* request,
                                    afs_operation::Statfs_response* response) {
    // Aggregate across all data servers
    uint64_t total_blocks = 0, total_bfree = 0, total_bavail = 0;
    uint64_t total_files = 0, total_ffree = 0;
    uint64_t bsize = 0, frsize = 0;

    std::lock_guard<std::mutex> lock(ds_mutex_);
    for (auto& entry : data_servers_) {
        if (!entry.alive) continue;
        grpc::ClientContext ctx;
        afs_operation::Statfs_request ds_req;
        afs_operation::Statfs_response ds_resp;
        auto status = entry.stub->statfs(&ctx, ds_req, &ds_resp);
        if (status.ok()) {
            if (bsize == 0) {
                bsize = ds_resp.bsize();
                frsize = ds_resp.frsize();
            }
            total_blocks += ds_resp.blocks();
            total_bfree += ds_resp.bfree();
            total_bavail += ds_resp.bavail();
            total_files += ds_resp.files();
            total_ffree += ds_resp.ffree();
        }
    }

    response->set_bsize(bsize);
    response->set_frsize(frsize);
    response->set_blocks(total_blocks);
    response->set_bfree(total_bfree);
    response->set_bavail(total_bavail);
    response->set_files(total_files);
    response->set_ffree(total_ffree);
    return grpc::Status::OK;
}

// === xattr — forward to owning data server ===

grpc::Status MetadataServer::setxattr(grpc::ServerContext* server_ctx,
                                      const afs_operation::SetXattr_request* request,
                                      afs_operation::SetXattr_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(path);
        if (!node) return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        ds_address = node->data_server_address;
    }

    if (ds_address.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Cannot set xattr on directory");
    }

    std::lock_guard<std::mutex> lock(ds_mutex_);
    auto* stub = get_stub_for(ds_address);
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Data server unavailable");

    std::string ds_root;
    for (const auto& entry : data_servers_) {
        if (entry.address == ds_address) { ds_root = entry.root_dir; break; }
    }

    grpc::ClientContext ctx;
    afs_operation::SetXattr_request ds_req;
    ds_req.set_directory(to_ds_path(ds_root, directory));
    ds_req.set_filename(filename);
    ds_req.set_name(request->name());
    ds_req.set_value(request->value());
    ds_req.set_flags(request->flags());
    ds_req.set_client_id(request->client_id());
    return stub->setxattr(&ctx, ds_req, response);
}

grpc::Status MetadataServer::getxattr(grpc::ServerContext* server_ctx,
                                      const afs_operation::GetXattr_request* request,
                                      afs_operation::GetXattr_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(path);
        if (!node) return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        ds_address = node->data_server_address;
    }

    if (ds_address.empty()) {
        response->set_found(false);
        return grpc::Status::OK;
    }

    std::lock_guard<std::mutex> lock(ds_mutex_);
    auto* stub = get_stub_for(ds_address);
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Data server unavailable");

    std::string ds_root;
    for (const auto& entry : data_servers_) {
        if (entry.address == ds_address) { ds_root = entry.root_dir; break; }
    }

    grpc::ClientContext ctx;
    afs_operation::GetXattr_request ds_req;
    ds_req.set_directory(to_ds_path(ds_root, directory));
    ds_req.set_filename(filename);
    ds_req.set_name(request->name());
    return stub->getxattr(&ctx, ds_req, response);
}

grpc::Status MetadataServer::listxattr(grpc::ServerContext* server_ctx,
                                       const afs_operation::ListXattr_request* request,
                                       afs_operation::ListXattr_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(path);
        if (!node) return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        ds_address = node->data_server_address;
    }

    if (ds_address.empty()) {
        return grpc::Status::OK; // empty list for directories
    }

    std::lock_guard<std::mutex> lock(ds_mutex_);
    auto* stub = get_stub_for(ds_address);
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Data server unavailable");

    std::string ds_root;
    for (const auto& entry : data_servers_) {
        if (entry.address == ds_address) { ds_root = entry.root_dir; break; }
    }

    grpc::ClientContext ctx;
    afs_operation::ListXattr_request ds_req;
    ds_req.set_directory(to_ds_path(ds_root, directory));
    ds_req.set_filename(filename);
    return stub->listxattr(&ctx, ds_req, response);
}

grpc::Status MetadataServer::removexattr(grpc::ServerContext* server_ctx,
                                         const afs_operation::RemoveXattr_request* request,
                                         afs_operation::RemoveXattr_response* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path = directory + (directory.back() == '/' ? "" : "/") + filename;

    std::string ds_address;
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        auto node = resolve_node(path);
        if (!node) return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
        ds_address = node->data_server_address;
    }

    if (ds_address.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Cannot remove xattr from directory");
    }

    std::lock_guard<std::mutex> lock(ds_mutex_);
    auto* stub = get_stub_for(ds_address);
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Data server unavailable");

    std::string ds_root;
    for (const auto& entry : data_servers_) {
        if (entry.address == ds_address) { ds_root = entry.root_dir; break; }
    }

    grpc::ClientContext ctx;
    afs_operation::RemoveXattr_request ds_req;
    ds_req.set_directory(to_ds_path(ds_root, directory));
    ds_req.set_filename(filename);
    ds_req.set_name(request->name());
    ds_req.set_client_id(request->client_id());
    return stub->removexattr(&ctx, ds_req, response);
}


// === main ===

int main(int argc, char** argv) {
    int port = 50050; // default

    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = std::atoi(argv[i + 1]);
            i++;
        }
    }

    MetadataServer server(port);
    std::cout << "Starting metadata server on port " << port << "..." << std::endl;
    server.RunServer();

    return 0;
}
