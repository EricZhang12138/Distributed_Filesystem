#include "filesystem_server.hpp" 
#include "subscriber_handler.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono> // For timestamp logic
#include <sys/stat.h> // For getting the stats 




int64_t get_file_timestamp(const std::string& path) {
    struct stat s;
    if (stat(path.c_str(), &s) != 0) {
        return 0; // Or handle error appropriately
    }
    // Combine Seconds + Nanoseconds into a single int64 timestamp
    // This guarantees high precision for 'compare' and perfect alignment with 'getattr'
    #ifdef __APPLE__
        int64_t timestamp = (static_cast<int64_t>(s.st_mtimespec.tv_sec) * 1000000000LL) 
                        + s.st_mtimespec.tv_nsec;
    #else 
        int64_t timestamp = (static_cast<int64_t>(s.st_mtim.tv_sec) * 1000000000LL) 
                        + s.st_mtim.tv_nsec;
    #endif

    return timestamp;
}

// temperary debug
void print_unorder(std::unordered_set<std::string> set, std::string path){
    std::cout << "Current client which registered(close): " << path << ": "; 
    for (std::string s: set){
        std::cout << s << ", ";
    }
    std::cout << "\n";
}

void print_notification_queue(const std::string& client_id, std::shared_ptr<NotificationQueue> notif_queue) {
    if (!notif_queue) {
        std::cout << "[DEBUG] Queue for client " << client_id << " is NULL" << std::endl;
        return;
    }
    
    std::lock_guard<std::mutex> lock(notif_queue->mu);
    
    std::cout << "========================================" << std::endl;
    std::cout << "[DEBUG] Notification Queue for Client: " << client_id << std::endl;
    std::cout << "Queue Size: " << notif_queue->queue.size() << std::endl;
    std::cout << "Shutdown: " << (notif_queue->shutdown ? "true" : "false") << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    
    if (notif_queue->queue.empty()) {
        std::cout << "  (Queue is empty)" << std::endl;
    } else {
        // Create a temporary copy to iterate without destroying original
        std::queue<afs_operation::Notification> temp_queue = notif_queue->queue;
        int index = 0;
        
        while (!temp_queue.empty()) {
            const afs_operation::Notification& notif = temp_queue.front();
            
            std::cout << "  [" << index << "] Message: " << notif.message() << std::endl;
            std::cout << "      Directory: " << notif.directory() << std::endl;
            if (!notif.new_directory().empty()) {
                std::cout << "      New Directory: " << notif.new_directory() << std::endl;
            }
            std::cout << "      Timestamp: " << notif.timestamp() << std::endl;
            std::cout << "      ---" << std::endl;
            
            temp_queue.pop();
            index++;
        }
    }
    std::cout << "========================================" << std::endl;
}


// this is called in close()
bool FileSystem::file_change_callback_close(const std::string& path, const std::string& client_id, afs_operation::Notification& notif){
    // close() is called
    {
        std::cout << "myclose is triggered" << std::endl;
        std::lock_guard<std::mutex> lock_file_map(file_map_mutex);
        auto it = file_map.find(path);
        if (it != file_map.end()){  // we find the path entry in the file_map
            std::unordered_set<std::string> client_set = it -> second;
            print_unorder(client_set, path);
            {
                std::lock_guard<std::mutex> lock_subscribers(subscriber_mutex);
                for (const std::string client: client_set){ // iterate through the client_set and update all of them
                    std::cout << client << std::endl;
                    if (client == client_id) continue; // skip the client that initiated the rename
                    auto queue_it = subscribers.find(client);

                    if (queue_it != subscribers.end()) {
                        // copy the shared_ptr in the queue and then this notif_ptr also owns the object now with this new shared_ptr
                        std::shared_ptr<NotificationQueue> notif_queue = queue_it->second;
                        // push to the producer worker queue
                        std::cout << "myclose is pushed to the queue" << std::endl;
                        notif_queue->push(notif);
                        //print_notification_queue(client_id, notif_queue);
                    }else{
                        std::cout << "we don't find "<< client << " in subscribers"<< std::endl;
                    }
                }
            }
        }else{// this may mean that we created the file on the client and we have not registered it on the maps
            // register this file path and the client id in file_map
            // NOTE: We already hold file_map_mutex from line 34, so NO nested lock needed
            file_map[path].insert(client_id); // add the path to the map and add the corresponding client
        }
    }
    return true;
}


bool FileSystem::file_change_callback_rename(const std::string& old_path, const std::string& new_path, const std::string& client_id, afs_operation::Notification& notif){
    // rename() is called
    {
        std::lock_guard<std::mutex> lock_file_map(file_map_mutex);
        auto it = file_map.find(old_path);
        if (it != file_map.end()){  // we find the old_path entry in the file_map
            std::unordered_set<std::string> client_set = it->second;
            {
                std::lock_guard<std::mutex> lock_subscribers(subscriber_mutex);
                for (const std::string& client: client_set){ // iterate through the client_set and update all of them
                    if (client == client_id) continue; // skip the client that initiated the rename
                    
                    // copy the shared_ptr in the queue and then this notif_ptr also owns the object now with this new shared_ptr
                    auto queue_it = subscribers.find(client);
                    if (queue_it != subscribers.end()) {
                        std::shared_ptr<NotificationQueue> notif_queue = queue_it->second;
                        // push to the producer worker queue
                        notif_queue->push(notif); 
                    }
                }
            }
            // Update the file_map: move clients from old_path to new_path
            file_map[new_path] = client_set;
            file_map.erase(it);
        } else {
            // If old_path is not in the map, just register the new_path with the client
            std::lock_guard<std::mutex> lock(file_map_mutex);
            file_map[new_path].insert(client_id);
        }
    }
    return true;
}

bool FileSystem::file_change_callback_unlink(const std::string& path, const std::string& client_id, afs_operation::Notification& notif){
    // unlink() is called - notify all clients watching this file
    {
        std::lock_guard<std::mutex> lock_file_map(file_map_mutex);
        auto it = file_map.find(path);
        if (it != file_map.end()){  // we find the path entry in the file_map
            std::unordered_set<std::string> client_set = it->second;
            {
                std::lock_guard<std::mutex> lock_subscribers(subscriber_mutex);
                for (const std::string& client: client_set){ // iterate through the client_set and update all of them
                    if (client == client_id) continue; // skip the client that initiated the unlink
                    
                    // copy the shared_ptr in the queue and then this notif_ptr also owns the object now with this new shared_ptr
                    auto queue_it = subscribers.find(client);
                    if (queue_it != subscribers.end()) {
                        std::shared_ptr<NotificationQueue> notif_queue = queue_it->second;
                        // push to the producer worker queue
                        notif_queue->push(notif); 
                    }
                }
            }
            // Remove the file from file_map since it no longer exists
            file_map.erase(it);
        }
    }
    return true;
}


// when a client disconnects, we need to clean up the maps on the server which contained info about the client
// this is called when the subscribe() method ends
void FileSystem::cleanup_client(const std::string& client_id) {
    std::cout << "Cleaning up client: " << client_id << std::endl;
    
    // Remove from client_db
    {
        std::lock_guard<std::mutex> lock(client_db_mutex);
        clients_db.erase(client_id);
    }
    
    // Remove from file_map
    {
        std::lock_guard<std::mutex> lock(file_map_mutex);
        for (auto it = file_map.begin(); it != file_map.end(); ) {
            it->second.erase(client_id);
            // Optionally remove empty entries
            if (it->second.empty()) {
                it = file_map.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Remove from subscribers and signal shutdown
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        auto it = subscribers.find(client_id);
        if (it != subscribers.end()) {
            it->second->cancel();  // Signal the queue to shutdown
            subscribers.erase(it);
        }
    }
    
    std::cout << "Client " << client_id << " cleanup complete" << std::endl;
}



grpc::Status FileSystem::getattr(grpc::ServerContext* context, const afs_operation::GetAttrRequest* request, afs_operation::GetAttrResponse* response) {
    std::string directory = request->directory();
    std::string filename = request->filename();
    std::string path;

    // Handle FUSE's request for root "/"
    // Your FUSE code will split "/" into (dir="/", filename="") or similar.
    // We'll reconstruct the intended path on the server.
    /*if (directory.empty() || directory == "/") {
        if (filename.empty() || filename == ".") {
            path = root_dir; // Get stats for the root directory
        } else {
            // Re-join filename to root, avoiding double-slash
            path = root_dir + (root_dir.back() == '/' ? "" : "/") + filename;
        }
    } else {
        path = directory + (directory.back() == '/' ? "" : "/") + filename;
    }*/
    path = directory + (directory.back() == '/' ? "" : "/") + filename;
    
    std::cout << "GetAttr request for resolved path: " << path << std::endl;

    try {
        // We must use stat() from <sys/stat.h> to get all POSIX info

        // The macOS can be asking whether metadata files like ._file1.txt exists or not
        // However, we can only see the files that can be listed with ls -a which doesn't contain ._file1.txt
        // so for every ._file1.txt, we see file not found error and this is totally normal
        struct stat s;
        if (stat(path.c_str(), &s) != 0) {
            if (errno == ENOENT) {
                // 1. ENOENT means "Entry Not Found". 
                // This is NORMAL behavior when FUSE asks for a file that doesn't exist. 
                // We silently return NOT_FOUND so FUSE knows to say "No" to the OS.
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found");
            } else {
                // 2. Any other error (Permission denied, IO Error) IS a real problem.
                std::cerr << "Critical Error: stat() failed with errno " << errno 
                        << " for path: " << path << std::endl;
                return grpc::Status(grpc::StatusCode::INTERNAL, "stat() system call failed");
            }
        }
        
        // Populate the response
        response->set_size(s.st_size);   // file size in bytes
        response->set_mode(s.st_mode);      // This includes file type (S_IFREG/S_IFDIR) AND permissions
        response->set_nlink(s.st_nlink);      // number of hard links to the file
        response->set_uid(s.st_uid);       // user id and group id of the file's owner
        response->set_gid(s.st_gid);

        int64_t precise_time = get_file_timestamp(path);
        response->set_mtime(precise_time);
        response->set_atime(precise_time); // Or create a similar helper for atime if needed
        response->set_ctime(precise_time);     

        // Log mode in octal, which is standard for permissions
        std::cout << "Attributes sent (mode): " << std::oct << s.st_mode << std::dec << std::endl;
        return grpc::Status::OK;

    } catch (const std::exception& e) { // Catch generic exceptions too
        std::cerr << "Exception in getattr: " << e.what() << std::endl;
        return grpc::Status(grpc::StatusCode::INTERNAL, "An exception occurred");
    }
}


grpc::Status FileSystem::request_dir(grpc::ServerContext* context, const afs_operation::InitialiseRequest* request, afs_operation::InitialiseResponse* response) {
    if (request->code_to_initialise() == "I want input/output directory"){
        std::cout << "Received client request(later I should add the name of the client)" << std::endl;
        response->set_root_path(root_dir);
        if (request -> client_id() != ""){
            std::string client_id = request -> client_id();
            if (clients_db.find(client_id) == clients_db.end()){ // client is not in the clients_db yet so we are good
                clients_db.insert(client_id);
                std::cout << "Connection successful and the client ID is " << client_id << std::endl;
            }else{
                std::cout << "Client ID already exists, please retry later ...." << std::endl;
            }
        }
        return grpc::Status::OK;
    }else{
        std::cerr << "There is an error while passing the input/output files directory"<<std::endl;
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "You need the correct code to retrieve requested data.");
    }
}

grpc::Status FileSystem::open(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter<afs_operation::FileResponse>* writer) {
    //get the file name
    std::string filename = request -> filename(); // gRPC generates getter methods
    std::string directory = request -> directory();
    std::cout << "Client wants " << directory<<(directory.back()=='/'? "" : "/")<<filename << std::endl;
    std::string path = directory + (directory.back()=='/'? "" : "/") + filename;
    // this client is registering its interest
    std::string client_id = request->client_id();
    // update the file_map
    {
        std::lock_guard<std::mutex> lock(file_map_mutex);
        file_map[path].insert(client_id); // add the path to the map and add the corresponding client
    }

    // update the file_map_open
    {
        std::lock_guard<std::mutex> lock(file_map_open_mutex);
        file_map_open[path].insert(client_id); // add the path to the map and add the corresponding client
    }

    // have to read the file in binary mode to avoid line ending translation
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()){
        std::cerr << "file: " << path << " not found" << std::endl;
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found on the server.");
    }

    const std::size_t chunk_size = 4096;
    char buffer[chunk_size];
    afs_operation::FileResponse fr;
    // Get the authoritative timestamp via stat
    int64_t timestamp_server = get_file_timestamp(path);

    while(true){
        file.read(buffer, chunk_size);
        std::streamsize len = file.gcount();
        
        if(len <= 0)break;
        fr.clear_content();
        fr.set_content(buffer, len);
        fr.set_length(static_cast<int32_t>(len));
        // Use the consistent stat-based timestamp
        fr.set_timestamp(timestamp_server);

        if(!writer->Write(fr)){
            std::cerr << "Error: Failed write " << std::endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, "Server failed to create the file.");
        }
    }
    // writer->WritesDone();
    std::cout << "File: " << filename << " successfully retrieved." << std::endl;
    return grpc::Status::OK;
}


grpc::Status FileSystem::close(grpc::ServerContext* context, grpc::ServerReader<afs_operation::FileRequest>* reader, afs_operation::FileResponse* response) {
    std::cout << "[SERVER] close() called" << std::endl;

    afs_operation::FileRequest request;
    std::string filename;
    std::string path;
    std::string client_id;

    std::ofstream outfile;

    std::cout << "[SERVER] Starting to read chunks..." << std::endl;
    while(reader->Read(&request)){
        if(filename.empty() || path.empty()){
            filename = request.filename();
            path = request.directory() + (request.directory().back()=='/'? "" : "/") + filename;
            std::filesystem::path file_path(path);
            std::filesystem::create_directories(file_path.parent_path());
            outfile.open(path, std::ios::binary);
            if(!outfile.is_open()){
                std::cerr << "failed to open file: " << path << std::endl;
                return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                    "cant open file to write");
            }
        }
        outfile.write(request.content().data(), request.content().size());
        client_id = request.client_id();
    }

    std::cout << "close is in progress" <<std::endl;
    if(outfile.is_open()) outfile.close();
    
    // Check if path is empty, which happens if no messages were received
    if (path.empty()) {
        std::cerr << "Close RPC received no file data." << std::endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "No file data received.");
    }

    // Get the new authoritative timestamp generated by the OS after the write
    int64_t timestamp_server = get_file_timestamp(path);
    response->set_timestamp(timestamp_server);


    // generate the Notification object that we are gonna use to pass to all related clients
    afs_operation::Notification notif;
    notif.set_directory(path);
    notif.set_message("UPDATE");
    notif.set_timestamp(timestamp_server);
    // then we start updating the maps for the specific file
    std::cout << "[SERVER] Calling file_change_callback_close..." << std::endl;
    file_change_callback_close(path, client_id, notif);
    std::cout << "[SERVER] Callback complete, returning OK" << std::endl;
    std::cout.flush();

        // update the file_map_open
    {
        std::lock_guard<std::mutex> lock(file_map_open_mutex);
        
        auto it = file_map_open.find(path);
        if (it != file_map_open.end()) {
            it->second.erase(client_id);
            
            if (it->second.empty()) {
                file_map_open.erase(it);
            }
        }
        
        std::cout << path << " is closed by " << client_id << std::endl;
    }


    return grpc::Status::OK;
}

/*grpc::Status FileSystem::compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter< ::afs_operation::FileResponse>* writer) {
    std::string filename = request->filename();
    int64_t timestamp = request -> timestamp();
    std::string path = request->directory() + (request->directory().back()=='/'? "" : "/") + request->filename();
            
    try {
        int64_t timestamp_server = get_file_timestamp(path);
        
        if (timestamp_server > timestamp){
            // Server version is newer, send update
            std::ifstream new_content(path,std::ios::binary);
            
            if (!new_content.is_open()) {
                std::cerr << "Failed to open file for compare-read: " << path << std::endl;
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found during compare.");
            }
        
            const std::size_t chunk_size = 4096;
            char buffer[chunk_size];
            bool sent_at_least_once = false; // Flag to track if we sent anything
            while(true){
                new_content.read(buffer, chunk_size);
                std::streamsize len = new_content.gcount();
                if(len <= 0 && sent_at_least_once)break;
                
                afs_operation::FileResponse response;
                response.set_content(buffer, len);
                response.set_timestamp(timestamp_server);
                response.set_update_bit(1);
                if (!writer->Write(response)) {
                    std::cerr << "Failed to write chunk " << " to client" << std::endl;
                    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Failed to write chunk");
                }
                sent_at_least_once = true;
            }

            std::cout << "Cache for '" << filename << "' is stale. Sent update." << std::endl;
            return grpc::Status::OK;

        } else {
            // Client version is valid
            afs_operation::FileResponse response;
            response.set_update_bit(0);
            response.set_timestamp(timestamp_server); // Send back server time for consistency
            writer->Write(response);

            std::cout << "Cache for '" << filename << "' is valid." << std::endl;
            return grpc::Status::OK;
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Warning: Could not get timestamp for file: " << e.what() << std::endl;
        // This could also be NOT_FOUND if the file doesn't exist
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found on server for compare.");
    }
}*/


grpc::Status FileSystem::ls(grpc::ServerContext* context, const afs_operation::ListDirectoryRequest* request, afs_operation::ListDirectoryResponse* response){
    std::string directory = request -> directory();
    
    std::filesystem::path directory_path(directory);
    
    try {
        // Check if the path exists and is a directory
        if (!std::filesystem::exists(directory_path)) {
            std::cerr << "Error: Directory not found: " << directory << std::endl;
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Specified Directory not found");
        }
        if (!std::filesystem::is_directory(directory_path)) {
            std::cerr << "Error: Path is not a directory: " << directory << std::endl;
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Path is not a directory");
        }

        std::cout << "Listing contents for: " << directory_path.string() << std::endl;

        // Get mutable pointer to protobuf map
        auto* entry_map = response->mutable_entry_list();  
        
        // Iterate over the path provided in the request
        for(const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory_path)){
            std::string name = entry.path().filename().string();
            if (entry.is_directory()){
                (*entry_map)[name] = "Directory";
            } else if(entry.is_regular_file()){
                (*entry_map)[name] = "Regular_File";
            }
        }
    } catch(std::filesystem::filesystem_error& e){
        std::cerr << "Error: " << e.what() << std::endl;
        return grpc::Status(grpc::StatusCode::ABORTED, "Error occurred while iterating through the directory");
    }
    
    return grpc::Status::OK;
}


grpc::Status FileSystem::mkdir(grpc::ServerContext* context, const afs_operation::MakeDir_request* request, afs_operation::MakeDir_response* response){
    std::string directory = request -> directory();
    uint32_t mode = request -> mode();
    std::filesystem::path path(directory);

    if (std::filesystem::exists(path)){
        // folder already exists
        std::cout << "Path already exists : "<< directory << std::endl;
        
        if (!std::filesystem::is_directory(path)){
            std::cout << "The directory you want to create exists as a path: " << directory << std::endl;
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Path is not a directory");
        }
        //return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Path is not a directory");
        return grpc::Status::OK;
    }
    try {
        if (!std::filesystem::create_directory(path)){
            std::cout << "Directory creation failed for: " << directory << std::endl;
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Error occurred trying to create directory");
        }else{
            // successfully created the directory
            std::filesystem::permissions(directory, static_cast<std::filesystem::perms>(mode));
            std::cout << "Directory creation successful: " << directory << std::endl;
        }
    }catch(const std::filesystem::filesystem_error& e){
        std::cerr << "Error: " << e.what() << std::endl;
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    return grpc::Status::OK;
}


/// @brief rename a file based on old filename and new filename
/// @param context 
/// @param request  contains old filename and new filename and its current directory on the server
/// @param response   Success or fail
/// @return 
grpc::Status FileSystem::rename(grpc::ServerContext* context, const afs_operation::RenameRequest* request, afs_operation::RenameResponse* response) {
    std::cout << "Rename on the server starts ..." << std::endl;
    std::string directory = request->directory();
    std::string directory_new = request -> new_directory();
    std::string client_id = request -> client_id();
    // Handle root dir logic
    std::string s_dir = directory + (directory.back() == '/' ? "" : "/");
    std::string s_dir_new = directory_new + (directory_new.back() == '/' ? "" : "/");
    
    std::string old_path = s_dir + request->filename();
    std::string new_path = s_dir_new + request->new_filename();

    try {
        // std::filesystem::rename is atomic and replaces existing files

        // ensure the destination folder exists by create_directories()
        std::filesystem::create_directories(std::filesystem::path(new_path).parent_path());

        std::filesystem::rename(old_path, new_path);
        afs_operation::Notification notif;
        notif.set_message("Rename");
        notif.set_new_directory(new_path);
        notif.set_directory(old_path);

        int64_t timestamp = get_file_timestamp(new_path);
        notif.set_timestamp(timestamp);
        file_change_callback_rename(old_path,new_path,client_id,notif);

        std::cout << "Server Renamed: " << request->filename() << " -> " << request->new_filename() << std::endl;
        response->set_success(true);
        return grpc::Status::OK;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Rename failed: " << e.what() << std::endl;
        // If source file doesn't exist, it might be a new local file not yet uploaded.
        // We return NOT_FOUND so the client knows it's local-only.
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Source file not found");
    }
}


grpc::Status FileSystem::unlink(grpc::ServerContext* context, const afs_operation::Delete_request* request, afs_operation::Delete_response* response){
    std::string directory = request -> directory();
    std::string client_id = request -> client_id();
    std::error_code ec;
    if (std::filesystem::remove(directory, ec)) {
        // now generate the notif message
        afs_operation::Notification notif;
        notif.set_directory(request -> directory());
        notif.set_message("DELETE");
        file_change_callback_unlink(directory, client_id, notif);
        std::cout << "File deleted successfully on the server at: " << directory << std::endl;
    } else {
        if (ec){
            std::cout << "Error: "<< ec.message() << std::endl;
            if (ec == std::errc::permission_denied){
                std::cout << "Permission denied" << std::endl;;
                return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Permission denied");
            }else if (ec == std::errc::no_such_file_or_directory){
                std::cout << "File not found: " << directory << std::endl; 
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "Source file not found");
            }
        } else{
            std::cout << "File doesn't exist at: " << directory << std::endl;
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Source file not found");
        }
    }
    return grpc::Status::OK;
}

// gRPC for the dashboard
grpc::Status FileSystem::GetStatus(grpc::ServerContext* context, 
                                   const afs_operation::GetStatusRequest* request, 
                                   afs_operation::GetStatusResponse* response) {
    
    // 1. Handle Connected Clients (Cleaner using repeated)
    {
        std::lock_guard<std::mutex> lock(client_db_mutex);
        
        for (const auto& client_id : clients_db) {
            // "add_connected_clients" is automatically generated for repeated fields
            response->add_connected_clients(client_id);
        }
    }

    // 2. Handle File Map (Same as before)
    {
        std::lock_guard<std::mutex> lock(file_map_open_mutex);
        auto* response_map = response->mutable_file_to_clients();

        for (const auto& [file_path, user_set] : file_map_open) {
            afs_operation::FileUsers file_users_msg;
            for (const auto& user_id : user_set) {
                file_users_msg.add_users(user_id);
            }
            (*response_map)[file_path] = file_users_msg;
        }
    }

    return grpc::Status::OK;
}

void FileSystem::RunServer(){
    std::string server_address = "0.0.0.0:50051";
    
    grpc::ServerBuilder builder;
    
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);
    
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    
    server->Wait();
}

FileSystem::FileSystem(std::string root_dir_input): root_dir(root_dir_input){
    starting_length = root_dir.size();
}

// implement truncate. Since we may only need to truncate

// --- Main Application Entry Point ---

int main(int argc, char** argv){
    if (argc != 2){
        return 1; // fail and end
    }
    std::string path(argv[1]);
    FileSystem filesys(path);
    std::cout << "Running filesystem server...... Current root directory on the server is " << path << std::endl;
    filesys.RunServer();


    std::cout << "Server Stopped" << std::endl;
    return 0;
}