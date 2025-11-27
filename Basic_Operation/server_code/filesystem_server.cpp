#include "filesystem_server.hpp" 
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

// REPLACE the entire getattr function with this:
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
    
    afs_operation::FileRequest request;
    std::string filename;
    std::string path;

    std::ofstream outfile;

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
    }

    if(outfile.is_open()) outfile.close();

    // Check if path is empty, which happens if no messages were received
    if (path.empty()) {
        std::cerr << "Close RPC received no file data." << std::endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "No file data received.");
    }

// Get the new authoritative timestamp generated by the OS after the write
    int64_t timestamp_server = get_file_timestamp(path);
    response->set_timestamp(timestamp_server);
    return grpc::Status::OK;
}


grpc::Status FileSystem::compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter< ::afs_operation::FileResponse>* writer) {
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
            while(true){
                new_content.read(buffer, chunk_size);
                std::streamsize len = new_content.gcount();
                if(len <= 0)break;
                
                afs_operation::FileResponse response;
                response.set_content(buffer, len);
                response.set_timestamp(timestamp_server);
                response.set_update_bit(1);
                if (!writer->Write(response)) {
                    std::cerr << "Failed to write chunk " << " to client" << std::endl;
                    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Failed to write chunk");
                }
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
}


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


void FileSystem::RunServer(){
    std::string server_address = "0.0.0.0:50051";
    
    grpc::ServerBuilder builder;
    
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);
    
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    
    server->Wait();
}


/// @brief rename a file based on old filename and new filename
/// @param context 
/// @param request  contains old filename and new filename and its current directory on the server
/// @param response   Success or fail
/// @return 
grpc::Status FileSystem::rename(grpc::ServerContext* context, const afs_operation::RenameRequest* request, afs_operation::RenameResponse* response) {
    std::string directory = request->directory();
    // Handle root dir logic
    std::string s_dir = directory + (directory.back() == '/' ? "" : "/");
    
    std::string old_path = s_dir + request->filename();
    std::string new_path = s_dir + request->new_filename();

    try {
        // std::filesystem::rename is atomic and replaces existing files
        std::filesystem::rename(old_path, new_path);
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


// implement truncate. Since we may only need to truncate





// --- Main Application Entry Point ---

int main(){
    FileSystem filesys;
    filesys.RunServer();

    std::cout << "Server Stopped" << std::endl;
    return 0;
}