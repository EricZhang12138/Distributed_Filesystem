#ifndef SUBSCRIBER_HANDLER_HPP
#define SUBSCRIBER_HANDLER_HPP

#include "filesystem_server.hpp"

grpc::Status FileSystem::subscribe(grpc::ServerContext* context, 
                                   const afs_operation::SubscribeRequest* request, 
                                   grpc::ServerWriter<afs_operation::Notification>* writer) {
    
    std::string client_id = request->client_id();
    std::cout << "Client subscribed: " << client_id << std::endl;
    

    // Create a queue for this client
    std::shared_ptr<NotificationQueue> queue = std::make_shared<NotificationQueue>(); // a notification queue shared pointer
    queue -> shutdown = false;
    // when we call make_shared, we create a new object on the heap and then a pointer on the stack that points to the object
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        subscribers[client_id] = queue;
    }

    std::cout << "Client " << client_id << " subscribed for notifications" << std::endl;
    // 2. The Blocking Loop
    // This thread will now "sleep" inside queue->pop() until an event happens

    std::thread monitor([context, queue, client_id]() {
        while (!context->IsCancelled()) { // constantly checking whether context is cancelled, the context may be cancelled when the client disconnects or crashes
            std::this_thread::sleep_for(std::chrono::seconds(5)); // check every 5s
        }
        std::cout << "Client " << client_id << " context cancelled, shutting down queue" << std::endl;
        queue->cancel();
    });
    monitor.detach();
    // when the thread function completes it is automatically handled by the OS
    // it completely detaches from this current thread and becomes a daemon thread 


    afs_operation::Notification note;
    while(queue->pop(note)){                           // this keeps returning true unless shutdown + queue empty
        // If Write fails (client disconnected), we break the loop
        std::cout << "popping: " << note.directory() << " " << note.message() << std::endl;
        if (!writer->Write(note)) {
            std::cout << "Client disconnected: " << client_id << std::endl;
            break; 
        }
    }

    // clean up the three maps: file_map, client_db, subscribers
    cleanup_client(client_id);
    
    return grpc::Status::OK;
}


#endif 