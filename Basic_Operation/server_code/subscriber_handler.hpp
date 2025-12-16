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
    // when we call make_shared, we create a new object on the heap and then a pointer on the stack that points to the object
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        subscribers[client_id] = queue;
    }

    // 2. The Blocking Loop
    // This thread will now "sleep" inside queue->pop() until an event happens
    afs_operation::Notification note;
    while (queue->pop(note)) {                           // this keeps returning true unless shutdown + queue empty
        // If Write fails (client disconnected), we break the loop
        if (!writer->Write(note)) {
            std::cout << "Client disconnected: " << client_id << std::endl;
            break; 
        }
    }

    // 3. Cleanup when client disconnects
    { // subscribers
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        subscribers.erase(client_id);
    }
    { // clients_db
        std::lock_guard<std::mutex> lock(client_db_mutex);
        clients_db.erase(client_id);
    }
    
    return grpc::Status::OK;
}






#endif 