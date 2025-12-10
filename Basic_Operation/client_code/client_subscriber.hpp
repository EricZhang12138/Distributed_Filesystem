#ifndef SUBSCRIBER
#define SUBSCRIBER

#include "filesystem_client.hpp"

void FileSystemClient::RunSubscriber() {
    grpc::ClientContext context;
    afs_operation::SubscribeRequest request;
    request.set_client_id(client_id);

    std::unique_ptr<grpc::ClientReader<afs_operation::Notification>> reader(
        stub_->Subscribe(&context, request));

    afs_operation::Notification note;
    while (reader->Read(&note)) {          // execution blocks here and the while loop won't be executed until a notification arrives
        std::cout << "NOTIFICATION RECEIVED: " << note.message() << " for " << note.filename() << std::endl;

        std::string full_path = note.directory() + (note.directory().back() == '/' ? "" : "/") + note.filename();

        // Thread-safe cache update
        // std::lock_guard<std::mutex> lock(cache_mutex_);

        if (note.message() == "UPDATE") {
            // Mark file as stale so next open_file fetches it again
            // You might need to map full_path back to your local cache key
            // Or simply remove it from cache:
            // cache.erase(...); 
        }
        // Handle DELETE, RENAME, etc.
    }
}



#endif