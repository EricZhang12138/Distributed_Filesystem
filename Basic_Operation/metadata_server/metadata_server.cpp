#include "metadata_server.hpp"
#include <iostream>
#include <string>
#include <cstring>

MetadataServer::MetadataServer(int port) : port_(port) {}

std::shared_ptr<afs_operation::operators::Stub> MetadataServer::get_data_stub() {
    std::lock_guard<std::mutex> lock(ds_mutex_);
    return data_stub_;  // returns a shared_ptr copy — keeps stub alive for the caller
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
    if (!data_server_registered_) {
        std::cerr << "[META] No data server registered yet!" << std::endl;
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server registered");
    }

    response->set_root_path(data_server_.root_dir);
    response->set_data_server_address(data_server_.address);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::Lookup(grpc::ServerContext* context,
                                    const metadata::LookupRequest* request,
                                    metadata::LookupResponse* response) {
    std::lock_guard<std::mutex> lock(ds_mutex_);
    if (!data_server_registered_) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server registered");
    }
    // Phase 1: all files are on the single data server
    response->set_data_server_address(data_server_.address);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::RegisterDataServer(grpc::ServerContext* context,
                                                const metadata::RegisterRequest* request,
                                                metadata::RegisterResponse* response) {
    std::lock_guard<std::mutex> lock(ds_mutex_);
    data_server_.address = request->address();
    data_server_.root_dir = request->root_dir();
    data_server_.last_heartbeat = std::chrono::steady_clock::now();
    data_server_registered_ = true;

    // Create the stub to the data server
    auto channel = grpc::CreateChannel(data_server_.address, grpc::InsecureChannelCredentials());
    data_stub_ = afs_operation::operators::NewStub(channel);

    std::cout << "[META] Data server registered: " << data_server_.address
              << " root=" << data_server_.root_dir << std::endl;

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status MetadataServer::Heartbeat(grpc::ServerContext* context,
                                       const metadata::HeartbeatRequest* request,
                                       metadata::HeartbeatResponse* response) {
    std::lock_guard<std::mutex> lock(ds_mutex_);
    if (data_server_registered_ && data_server_.address == request->address()) {
        data_server_.last_heartbeat = std::chrono::steady_clock::now();
        response->set_success(true);
    } else {
        response->set_success(false);
    }
    return grpc::Status::OK;
}


// === Forwarded structural RPCs ===
// Each one creates a new ClientContext and forwards the request to the data server.

grpc::Status MetadataServer::ls(grpc::ServerContext* server_ctx,
                                const afs_operation::ListDirectoryRequest* request,
                                afs_operation::ListDirectoryResponse* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->ls(&ctx, *request, response);
}

grpc::Status MetadataServer::getattr(grpc::ServerContext* server_ctx,
                                     const afs_operation::GetAttrRequest* request,
                                     afs_operation::GetAttrResponse* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->getattr(&ctx, *request, response);
}

grpc::Status MetadataServer::mkdir(grpc::ServerContext* server_ctx,
                                   const afs_operation::MakeDir_request* request,
                                   afs_operation::MakeDir_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->mkdir(&ctx, *request, response);
}

grpc::Status MetadataServer::unlink(grpc::ServerContext* server_ctx,
                                    const afs_operation::Delete_request* request,
                                    afs_operation::Delete_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->unlink(&ctx, *request, response);
}

grpc::Status MetadataServer::rmdir(grpc::ServerContext* server_ctx,
                                   const afs_operation::Rmdir_request* request,
                                   afs_operation::Rmdir_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->rmdir(&ctx, *request, response);
}

grpc::Status MetadataServer::rename(grpc::ServerContext* server_ctx,
                                    const afs_operation::RenameRequest* request,
                                    afs_operation::RenameResponse* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->rename(&ctx, *request, response);
}

grpc::Status MetadataServer::chmod(grpc::ServerContext* server_ctx,
                                   const afs_operation::Chmod_request* request,
                                   afs_operation::Chmod_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->chmod(&ctx, *request, response);
}

grpc::Status MetadataServer::chown(grpc::ServerContext* server_ctx,
                                   const afs_operation::Chown_request* request,
                                   afs_operation::Chown_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->chown(&ctx, *request, response);
}

grpc::Status MetadataServer::utimens(grpc::ServerContext* server_ctx,
                                     const afs_operation::Utimens_request* request,
                                     afs_operation::Utimens_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->utimens(&ctx, *request, response);
}

grpc::Status MetadataServer::symlink(grpc::ServerContext* server_ctx,
                                     const afs_operation::Symlink_request* request,
                                     afs_operation::Symlink_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->symlink(&ctx, *request, response);
}

grpc::Status MetadataServer::readlink(grpc::ServerContext* server_ctx,
                                      const afs_operation::Readlink_request* request,
                                      afs_operation::Readlink_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->readlink(&ctx, *request, response);
}

grpc::Status MetadataServer::statfs(grpc::ServerContext* server_ctx,
                                    const afs_operation::Statfs_request* request,
                                    afs_operation::Statfs_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->statfs(&ctx, *request, response);
}

grpc::Status MetadataServer::setxattr(grpc::ServerContext* server_ctx,
                                      const afs_operation::SetXattr_request* request,
                                      afs_operation::SetXattr_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->setxattr(&ctx, *request, response);
}

grpc::Status MetadataServer::getxattr(grpc::ServerContext* server_ctx,
                                      const afs_operation::GetXattr_request* request,
                                      afs_operation::GetXattr_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->getxattr(&ctx, *request, response);
}

grpc::Status MetadataServer::listxattr(grpc::ServerContext* server_ctx,
                                       const afs_operation::ListXattr_request* request,
                                       afs_operation::ListXattr_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->listxattr(&ctx, *request, response);
}

grpc::Status MetadataServer::removexattr(grpc::ServerContext* server_ctx,
                                         const afs_operation::RemoveXattr_request* request,
                                         afs_operation::RemoveXattr_response* response) {
    auto stub = get_data_stub();
    if (!stub) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No data server");
    grpc::ClientContext ctx;
    return stub->removexattr(&ctx, *request, response);
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
