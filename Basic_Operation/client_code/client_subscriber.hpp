
#ifndef SUBSCRIBER
#define SUBSCRIBER

#include "filesystem_client.hpp"

void FileSystemClient::RunSubscriber() {
    afs_operation::SubscribeRequest request;
    request.set_client_id(client_id);

    std::unique_ptr<grpc::ClientReader<afs_operation::Notification>> reader(
        stub_->subscribe(subscriber_context_.get(), request));

    if (!reader) {
        std::cerr << "ERROR: Failed to create subscription reader" << std::endl;
        return;
    }

    afs_operation::Notification note;
    while (reader->Read(&note)) {          // execution blocks here and the while loop won't be executed until a notification arrives
        std::cout << "NOTIFICATION RECEIVED: " << note.message() << " for " << note.directory() << std::endl;

        std::string path_on_server = note.directory() + (note.directory().back() == '/' ? "" : "/");
        std::string path_on_client = std::string("./tmp/cache") + (path_on_server.front()=='/'? "" : "/") + path_on_server;

        if (note.message() == "UPDATE") {
            // we will erase the corresponding file in cache if we want to update
            // it is important to note that two clients can't open the same file at the same time
            // otherwise, if one flushes its changes to the server and the other still has the file opened and the file in cache is removed
            // It can lead to undefined behaviour

            // check if file is opened
            cache_mutex.lock();
            if (opened_files.find(path_on_client) != opened_files.end()){
                std::cout << "Error: File currently open and updates from server failed for " << path_on_client << std::endl;
                cache_mutex.unlock();
                continue; // go to the top of the while loop and abort this update
            }
            auto it = cache.find(path_on_client);
            if (it != cache.end()){ // we can find the file in cache. Since we registered, it should always be in cache
                cache.erase(it); // erase the cache
            }else{ // not in cache
                std::cout << "Inconsistent State: File: " << path_on_client << " is registered but not in cache"<< std::endl;
                cache_mutex.unlock();
                continue; 
            }
            auto its = cached_attr.find(path_on_server);
            if (its != cached_attr.end()){
                cached_attr.erase(its);
            }else{
                std::cout << "Inconsistent State: File: " << path_on_server << " is registered but not in cache"<< std::endl;
                cache_mutex.unlock();
                continue; 
            }
            cache_mutex.unlock();
        }
        // Handle DELETE, RENAME, etc.
    }

    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        std::cerr << "Subscriber stream failed: " << status.error_code() 
            << " - " << status.error_message() << std::endl;
    }
}

#endif

