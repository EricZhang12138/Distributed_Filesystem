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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <queue>

// helper class used for managing the callback system
struct NotificationQueue{
    std::queue<afs_operation::Notification> queue;
    std::mutex mu;
    std::condition_variable cv;
    bool shutdown = true;

    // push for the producer (unlink/close/rename function calls) and it wakes up one thread 
    void push(afs_operation::Notification notif){
        std::lock_guard<std::mutex> lock(mu);
        queue.push(notif);
        cv.notify_one(); // notify one thread that is waiting (sleeping because the condition is false)
    }

    bool pop(afs_operation::Notification& notif){
        std::unique_lock<std::mutex> lock(mu); // unique_lock is a lock management object similar to lock_guard
        cv.wait(lock, [this](){return !queue.empty() || shutdown;});

        if (shutdown && queue.empty()) return false; // graceful shutdown benefitting from the cancel() function
        
        notif = queue.front();
        queue.pop();
        return true;
    }
    void cancel(){
        std::lock_guard<std::mutex> lock(mu);
        shutdown = true; 
        cv.notify_all(); // wake up all the threads and then they return false in pop() for a graceful shutdown 

    }
};

// main filesystem server class 
class FileSystem final : public afs_operation::operators::Service{

public: 
    std::string root_dir;           // "/Users/ericzhang/Documents/Filesystems/Filesystem_server";
    int starting_length;
    std::mutex file_map_mutex;
    std::unordered_map<std::string, std::unordered_set<std::string>> file_map; // a map of file directories to a vector of userIDs
    void RunServer();
    FileSystem(std::string root_dir);
    
    std::mutex subscriber_mutex;
    // map of client ID to NotificationQueue
    std::unordered_map<std::string, std::shared_ptr<NotificationQueue>> subscribers;

private:

    std::mutex client_db_mutex;
    std::unordered_set<std::string> clients_db; // A list of all the clients that is currently connected to the server (For debugging purposes)

    grpc::Status request_dir(grpc::ServerContext* context, const afs_operation::InitialiseRequest* request, afs_operation::InitialiseResponse* response) override;

    grpc::Status open(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter<afs_operation::FileResponse>* writer) override;

    grpc::Status close(grpc::ServerContext* context, grpc::ServerReader<afs_operation::FileRequest>* reader, afs_operation::FileResponse* response) override;

    grpc::Status compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter< ::afs_operation::FileResponse>* writer) override;

    grpc::Status ls(grpc::ServerContext* context, const afs_operation::ListDirectoryRequest* request, afs_operation::ListDirectoryResponse* response) override;

    grpc::Status getattr(grpc::ServerContext* context, const afs_operation::GetAttrRequest* request, afs_operation::GetAttrResponse* response) override;

    grpc::Status rename(grpc::ServerContext* context, const afs_operation::RenameRequest* request, afs_operation::RenameResponse* response) override;

    grpc::Status mkdir(grpc::ServerContext* context, const afs_operation::MakeDir_request* request, afs_operation::MakeDir_response* response) override;

    grpc::Status unlink(grpc::ServerContext* context, const afs_operation::Delete_request* request, afs_operation::Delete_response* response) override;

    grpc::Status subscribe(grpc::ServerContext* context, const afs_operation::SubscribeRequest* request, grpc::ServerWriter<afs_operation::Notification>* writer) override;
};










