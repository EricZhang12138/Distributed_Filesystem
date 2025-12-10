#include "filesystem_client.hpp"
#include <iostream>
#include <sstream> 
#include <chrono>
#include <thread>
#include <filesystem>
#include <vector> 
#include <sys/stat.h>
#include <thread>


 
FileSystemClient::FileSystemClient(std::shared_ptr<grpc::Channel> channel) : stub_(afs_operation::operators::NewStub(channel)){
    // Generate a universally unique identifier for client ID
    boost::uuids::random_generator gen;
    boost::uuids::uuid id = gen();
    client_id = boost::uuids::to_string(id); // convert to string
    std::cout << "Client ID: " << client_id << std::endl;

    afs_operation::InitialiseRequest request;
    request.set_code_to_initialise("I want input/output directory");
    request.set_client_id(client_id);
    afs_operation::InitialiseResponse response;
    grpc::ClientContext context;
    grpc::Status status = stub_ -> request_dir(&context, request, &response);

    // set up the subscribe channel to the server

    if (!status.ok()){
        std::cerr << "Unable to retrieve the path for input/output files on the server" << std::endl;
        this->server_root_path_ ="/"; // default when it fails
    }else{
        // Store the root path instead of just printing it
        this->server_root_path_ = response.root_path(); 
        std::cerr << "Client initialized. Server root directory: " << this->server_root_path_ << std::endl; 
    }
}


FileSystemClient::~FileSystemClient(){
    // 1. stop the thread using subscriber_context
    if (subscriber_context_){
        subscriber_context_ -> TryCancel();
    }
    // join the thread before the object is destroyed
    if (subscriber_thread.joinable()){
        subscriber_thread.join();
    }
}


std::string FileSystemClient::resolve_server_path(const std::string& user_path) {
    std::filesystem::path root(this->server_root_path_);
    std::filesystem::path user(user_path);

    // If user_path is absolute (e.g., "/test_data"), 
    // we need its relative part to join with our root.
    if (user.is_absolute()) {
        user = user.relative_path();
    }

    // The '/' operator handles joining correctly (e.g., "/" + "test" -> "/test")
    std::filesystem::path full_path = root / user;
    return full_path.generic_string(); // Use generic_string for consistent '/' separators
}

std::optional<FileAttributes> FileSystemClient::get_attributes(const std::string& filename, const std::string& path) {
    // Note: resolve_server_path is still correct, as it gives the gRPC
    // server the "directory" string it expects (e.g., /path/to/root/test_dir)
    std::string resolved_path = resolve_server_path(path);
    
    afs_operation::GetAttrRequest request;
    request.set_filename(filename);
    request.set_directory(resolved_path); // Pass the server-resolved path 
    
    std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    auto it = cached_attr.find(file_loca_server);
    if (it != cached_attr.end()){   // this means file_loca_server exists in the cached_attr already
        return it -> second;
    }

    afs_operation::GetAttrResponse response;
    grpc::ClientContext context;

    grpc::Status status = stub_->getattr(&context, request, &response);

    if (!status.ok()) {
        // Don't log an error here if it's just "NOT_FOUND",
        // For example when we do ls, the OS asks the filesystem whether it has .bash_profile, .gitconfig etc.
        // If they are not found, it is normal and we don't want errors
        // as FUSE will check for non-existent files all the time.
        if (status.error_code() != grpc::StatusCode::NOT_FOUND) {
            std::cerr << "GetAttributes RPC failed: " << status.error_message() << std::endl;
        }
        return std::nullopt;
    }

    FileAttributes attrs;
    attrs.size = response.size();
    attrs.atime = response.atime();
    attrs.mtime = response.mtime();
    attrs.ctime = response.ctime();
    attrs.mode = response.mode();
    attrs.nlink = response.nlink();
    attrs.uid = getuid();
    attrs.gid = getgid(); // have to change the uid and gid to my local machine's to access them freely
                          // This approach assumes the client is authentic
    
    cached_attr[file_loca_server] = attrs;
    
    return attrs;
}


bool FileSystemClient::open_file(std::string filename, std::string path){
    std::string resolved_path = resolve_server_path(path);
    std::cout << "DEBUG: Opening '" << filename << "' at resolved path: " << resolved_path << std::endl;
    
    std::string cache_dir = "./tmp/cache" + resolved_path; // Use resolved_path
    std::string file_location = cache_dir + (cache_dir.back() == '/' ? "" : "/") + filename;
    
    // Case 1: File is NOT in the local cache
    if (cache.find(file_location) == cache.end()){
        // Create the local directory structure if it doesn't exist
        try {
            std::filesystem::create_directories(cache_dir);
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error creating local cache directory: " << e.what() << std::endl;
            return false; // Fail early if the directory can't be created
        }
        
        afs_operation::FileRequest request;
        request.set_filename(filename);
        request.set_directory(resolved_path);
        
        std::string file_path = file_location; // Use the full file_location path
        
        afs_operation::FileResponse response_temp;
        
        int num_of_retries = 0;
        grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");
        
        while(num_of_retries < 3 && !status.ok()){
            grpc::ClientContext context;
            std::unique_ptr<grpc::ClientReader<afs_operation::FileResponse>> reader(stub_->open(&context, request));

            std::ofstream outfile(file_path, std::ios::binary);
            if (!outfile.is_open()){
                std::cerr << "Failed to create file at " << file_path << std::endl;
                return false ;
            }

            int64_t last_timestamp = 0;
            while(reader->Read(&response_temp)){
                last_timestamp = response_temp.timestamp();
                if(response_temp.length() > 0)
                    outfile.write(response_temp.content().data(), response_temp.content().size());
                
                if (outfile.fail()){
                    std::cerr << "Can not write data to the local cache" << std::endl;
                    outfile.close();
                    return false;
                }
            }
            outfile.close();
            status = reader->Finish();
            
            if (status.ok()) {
                // Only add to cache on success
                struct FileInfo file_info{false, last_timestamp, filename};
                cache[file_location] = file_info;
                std::cout << "File cached successfully" << std::endl;
            }
            
            num_of_retries++;
        }

        if (!status.ok()){
            std::cerr << "After 3 tries, Failed to Open the file from the server" << std::endl;
            std::filesystem::remove(file_path); // Clean up partial file
            return false;
        }

        auto read_stream = std::make_unique<std::ifstream>(file_location, std::ios::binary);
        auto write_stream = std::make_unique<std::ofstream>(file_location, std::ios::binary | std::ios::in);

        if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
            std::cerr << "Failed to open local file streams after download." << std::endl;
            cache.erase(file_location);
            return false;
        }
        opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
        return true;

    } else { 
        // Case 2: File IS in the local cache, check for updates
        
        // Check if file is already open by this client
        if (opened_files.find(file_location)!=opened_files.end()){
            std::cerr<< "The file: " << file_location << "is already open, but it is okay if you try multiple times" << std::endl;
            return true;
        }
        
        int64_t timestamp = cache[file_location].timestamp;
        
        afs_operation::FileRequest request;
        request.set_filename(filename);
        request.set_timestamp(timestamp);
        request.set_directory(resolved_path); 

        int num_of_retries = 0;
        grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");
        
        std::stringstream content_buffer(std::ios::binary | std::ios::in | std::ios::out);
        int update_bit = 0;
        int64_t new_timestamp = 0;
        
        while(num_of_retries < 3 && !status.ok()){
            grpc::ClientContext context;
            afs_operation::FileResponse response_chunk;

            content_buffer.str(""); // Clear the stringstream's content
            content_buffer.clear(); // Clear its error flags
            update_bit = 0;
            new_timestamp = 0;
            
            std::unique_ptr<grpc::ClientReader<afs_operation::FileResponse>> reader(stub_->compare(&context, request));

            while(reader->Read(&response_chunk)){
                update_bit = response_chunk.update_bit();
                new_timestamp = response_chunk.timestamp();
                
                if(update_bit == 1) { 
                    content_buffer.write(response_chunk.content().data(), response_chunk.content().size());
                }
            }
            
            status = reader->Finish();
            num_of_retries++;
        }

        if (!status.ok()) {
            std::cerr << "RPC failed during compare after 3 attempts: " << status.error_message() << std::endl;
            return false; 
        }

        if (new_timestamp != 0) {
            cache[file_location].timestamp = new_timestamp;
            cache[file_location].locally_modified = false;
        }

        if (update_bit == 1) {
            std::cout << "Cache is stale. Overwriting local file..." << std::endl;
            std::ofstream outfile(file_location, std::ios::binary | std::ios::trunc);
            
            if (!outfile.is_open()) {
                std::cerr << "Error: Could not open the file at " << file_location << " to overwrite." << std::endl;
                return false;
            }
            outfile << content_buffer.rdbuf(); 
            if (outfile.fail()) {
                std::cerr << "Error: Failed to write new content to local file." << std::endl;
                outfile.close();
                return false;
            }
            outfile.close();
            std::cout << "Successfully overwrote file: " << file_location << std::endl;
        
        } else {
            std::cout << "File: "<< file_location << " is valid and is now opened." << std::endl;
        }
        
        auto read_stream = std::make_unique<std::ifstream>(file_location, std::ios::binary);
        auto write_stream = std::make_unique<std::ofstream>(file_location, std::ios::binary | std::ios::in);

        if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
            std::cerr << "Failed to open the local file stream." << std::endl;
            return false;
        }
        
        opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
        std::cout << "File '" << filename << "' is now open for use." << std::endl;
        return true;
    }
}

bool FileSystemClient::read_file(const std::string& filename, const std::string& directory, const int size, const int offset, std::vector<char>& buffer){
    
    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = "./tmp/cache" + resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;

    if (cache.find(file_location)==cache.end()){
        std::cerr << "File not in cache. Get the file from the server by calling open_file()" << std::endl;
        return false;
    } else {
        if (opened_files.find(file_location) == opened_files.end()){
            std::cerr << "File found in cache but it is not opened. Open the file by calling open_file()" << std::endl;
            return false;
        } else {
            auto it = opened_files.find(file_location);
            std::ifstream& file_stream = *(it->second.read_stream);
            // read the file from the cache
            file_stream.seekg(offset, std::ios::beg);
            if (!file_stream) {
                // This check catches errors like seeking past the end of the file
                std::cerr << "Error: Could not seek to offset " << offset << "." << std::endl;
                return false;
            }
            buffer.resize(size);
                                
            file_stream.read(buffer.data(), size);
            
            size_t bytes_read = file_stream.gcount();

            std::cout << "Requested " << size << " bytes, actually read " << bytes_read << "." << std::endl;

            if (bytes_read < size) {
                std::cout << "Warning: Reached end of file early." << std::endl;
                // The buffer will contain 'bytes_read' valid characters.
                // resize it to remove the unused space.
                buffer.resize(bytes_read);
            }
            return true;
        }
    }
}


bool FileSystemClient::write_file(const std::string& filename, const std::string& data, const std::string& directory, std::streampos position){

    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = "./tmp/cache" + resolved_path + (resolved_path.back()=='/' ? "" : "/") + filename; 

    if (cache.find(file_location) == cache.end()){
        std::cerr << "File not in cache. Get the file from the server by calling open_file()" << std::endl;
        return false;
    } else {
        if (opened_files.find(file_location) == opened_files.end()){
            std::cerr << "File found in cache but it is not opened. Open the file by calling open_file()" << std::endl;
            return false;
        } else {
            auto it = opened_files.find(file_location);
            std::ofstream& file_stream = *(it->second.write_stream);

            file_stream.seekp(position);
            if (file_stream.fail()){
                std::cerr << "Error: Failed to seek to position " << position << " in " << filename << std::endl; 
                file_stream.clear(); // clear the fail bit
                return false;
            }

            file_stream.write(data.data(), data.size());
            file_stream.flush();

            if (file_stream.fail()) {
                std::cerr << "Error: Failed to write data to " << filename << std::endl;
                return false;
            }
            
            // 1. Mark the file content as changed for eventual upload
            cache[file_location].locally_modified = true;

            // Update the cached_attr to reflect changes immediately

            // Reconstruct the key exactly how get_attributes does it
            std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
            
            auto attr_it = cached_attr.find(file_loca_server);
            if (attr_it != cached_attr.end()) {
                try {
                    // Update Size: Get the actual size of the local file on disk
                    // This handles both overwrites (size same) and appends (size grows) automatically
                    uintmax_t new_size = std::filesystem::file_size(file_location);
                    attr_it->second.size = static_cast<int64_t>(new_size);

                    // Get current time in nanoseconds
                    auto now = std::chrono::system_clock::now();
                    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

                    attr_it->second.mtime = static_cast<int64_t>(now_ns);
                    
                    // We do not change uid/gid here, preserving the "local machine" spoofing 
                    // I set in get_attributes.

                    std::cout << "Updated cached attributes for " << filename << ": Size=" << new_size << std::endl;

                } catch (const std::filesystem::filesystem_error& e) {
                    std::cerr << "Warning: Failed to update cached attributes size: " << e.what() << std::endl;
                    // We don't return false here because the write physically succeeded
                }
            }

            std::cout << "Successfully wrote to " << filename << " and marked as changed." << std::endl;
            return true;
        }
    } 
}

bool FileSystemClient::create_file(const std::string& filename, const std::string& path) {

    std::string resolved_path = resolve_server_path(path);
    std::string file_location = "./tmp/cache" + resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::string cache_dir = "./tmp/cache" + resolved_path;

    if (cache.count(file_location) || opened_files.count(file_location)) {
        std::cerr << "Error: File '" << filename << "' already exists." << std::endl;
        return false;
    }
    
    try {
        std::filesystem::create_directories(cache_dir);;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error creating local cache directory: " << e.what() << std::endl;
        return false; 
    }

    std::ofstream outfile(file_location); 
    if (!outfile.is_open()) {
        std::cerr << "Error: Failed to create local file at " << file_location << std::endl;
        return false;
    }
    outfile.close(); 

    struct FileInfo file_info{true, 0, filename};
    cache[file_location] = file_info;

    auto read_stream = std::make_unique<std::ifstream>(file_location, std::ios::binary);
    auto write_stream = std::make_unique<std::ofstream>(file_location, std::ios::binary | std::ios::in);
    
    if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
        std::cerr << "Failed to open newly created file stream for: " << file_location<< std::endl;
        cache.erase(file_location); 
        return false;
    }
    opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};

    FileAttributes attr;
    // We must use stat() from <sys/stat.h> to get all POSIX info
    struct stat s;
    if (stat(file_location.c_str(), &s) != 0) {
            std::cerr << "Error: stat() failed for path: " << file_location << std::endl;
            return false;
    }
    attr.size = s.st_size;
    #ifdef __APPLE__
        attr.atime = s.st_atimespec.tv_sec;
        attr.mtime = s.st_mtimespec.tv_sec;
        attr.ctime = s.st_ctimespec.tv_sec;
    #else 
        attr.atime = s.st_atim.tv_sec;
        attr.mtime = s.st_mtim.tv_sec;
        attr.ctime = s.st_ctim.tv_sec;
    #endif

    attr.mode = s.st_mode;
    attr.nlink = s.st_nlink;
    attr.uid = getuid();
    attr.gid = getgid();

    std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    cached_attr[file_loca_server] = attr;

    std::cout << "Successfully created and opened '" << filename << "' for writing." << std::endl;
    return true;
}


bool FileSystemClient::close_file(const std::string& filename, const std::string& directory) {
    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = "./tmp/cache" + resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    
    auto opened_file_it = opened_files.find(file_location);
    if (opened_file_it == opened_files.end()) {
        std::cerr << "Error: Cannot close '" << filename << "' because it is not open." << std::endl;
        return false;
    }

    auto cache_it = cache.find(file_location);
    if (cache_it == cache.end()) {
        std::cerr << "Error: Inconsistent state. File is open but not in cache." << std::endl;
        opened_files.erase(opened_file_it); 
        return false;
    }

    if (cache_it->second.locally_modified) {
        std::cout << "File '" << filename << "' was modified. Flushing to server..." << std::endl;

        std::ofstream& write_stream = *(opened_file_it->second.write_stream);
        write_stream.flush();
        if(write_stream.fail()) {
            std::cerr << "Error: Failed to flush write stream before closing." << std::endl;
            return false;
        }
        
        // We must close the streams before we can reliably read the file
        opened_file_it->second.read_stream->close();
        opened_file_it->second.write_stream->close();

        // Now open a new read stream for uploading
        std::ifstream file_stream(file_location, std::ios::binary);
        if (!file_stream.is_open()) {
            std::cerr << "Error: Could not re-open file for flushing: " << file_location << std::endl;
            return false; 
        }

        const std::size_t chunk_size = 4096;
        char buffer[chunk_size];

        afs_operation::FileResponse response; 
        int num_of_tries = 0;
        grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");
        
        while (num_of_tries < 3 && !status.ok()){
            grpc::ClientContext context;     
            std::unique_ptr<grpc::ClientWriter<afs_operation::FileRequest>> writer(
                stub_->close(&context, &response)
            );
            
            file_stream.clear(); // Clear EOF flag
            file_stream.seekg(0, std::ios::beg); // Rewind to start
            
            while(true){
                file_stream.read(buffer, chunk_size);
                std::streamsize len = file_stream.gcount();

                if(len<=0)break;
                afs_operation::FileRequest request;
                request.set_directory(resolved_path);
                request.set_filename(filename);
                request.set_content(buffer, len);

                if (!writer->Write(request)){
                    break;
                }
            }
            writer->WritesDone();
            status = writer->Finish();
            num_of_tries ++;
        }
        
        file_stream.close(); // Close the temporary read stream

        if (!status.ok()) {
            std::cerr << "RPC failed while flushing file to server: " << status.error_message() << std::endl;
            // Re-open the file handles so the user can retry
            auto read_s = std::make_unique<std::ifstream>(file_location, std::ios::binary);
            auto write_s = std::make_unique<std::ofstream>(file_location, std::ios::binary | std::ios::in);
            opened_files[file_location] = FileStreams{std::move(read_s), std::move(write_s)};
            std::cerr << "File handles have been re-opened. Please try closing again." << std::endl;
            return false; 
        }


        // 1. Get the authoritative time from the server (e.g., 1710000000000000000 ns)
        int64_t server_nanos = response.timestamp();

        // 2. Update internal cache for 'compare' logic (Keep precision!)
        cache_it->second.timestamp = server_nanos; 

        // 3. Update FUSE attributes (Convert to Seconds for the OS)
        // integer division removes the extra zeros
        
        std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;

        auto attr_it = cached_attr.find(file_loca_server);
        if (attr_it != cached_attr.end()) {
            attr_it->second.mtime = server_nanos; // Now 'ls -l' shows the correct time!
            attr_it->second.ctime = server_nanos;
            
            // Also update the size, because you just wrote data!
            try {
                attr_it->second.size = std::filesystem::file_size(file_location);
            } catch (...) {}
        }


        std::cout << "File flushed successfully." << std::endl;
    
    } else {
        std::cout << "File '" << filename << "' was not modified. No flush needed." << std::endl;
    }

    opened_files.erase(opened_file_it);
    
    std::cout << "File '" << filename << "' is now closed." << std::endl;
    return true;
}


std::optional<std::map<std::string, std::string>> FileSystemClient::ls_contents(const std::string& directory){
    grpc::ClientContext context;  
    afs_operation::ListDirectoryRequest request;
    afs_operation::ListDirectoryResponse response;
    std::string resolved_path = resolve_server_path(directory);
    std::cout << "DEBUG: Listing contents for resolved path: " << resolved_path << std::endl;
    request.set_directory(resolved_path); // Use resolved_path

    grpc::Status status = stub_ -> ls(&context, request, &response);
    if (!status.ok()){
        std::cerr << "Failed to load the directory content from the server: " << status.error_message() << std::endl;
        return std::nullopt;
    }

    for (const auto& [name, type] : response.entry_list()) {
    std::cout << name << ": " << type << std::endl;
    }
    // now we get the current directory
    std::map<std::string, std::string> entry_map;
    for (const auto& [name, type] : response.entry_list()) {
        entry_map[name] = type;
    }
    return entry_map;
}


// In filesystem_client.cpp

bool FileSystemClient::rename_file(const std::string& from_name, const std::string& to_name, const std::string& old_path, const std::string& new_path) {
    std::string resolved_path = resolve_server_path(old_path);
    std::string resolved_path_new = resolve_server_path(new_path);
    
    // 1. Prepare Paths
    // We add a trailing slash to base_dir so we can build generic paths safely
    std::string base_dir = "./tmp/cache" + resolved_path + (resolved_path.back() == '/' ? "" : "/");
    std::string base_dir_new = "./tmp/cache" + resolved_path_new + (resolved_path_new.back() == '/' ? "" : "/");
    std::string old_local_path = base_dir + from_name; 
    std::string new_local_path = base_dir_new + to_name;

    // 2. SAFETY CHECK: Destination Collision
    // If the new name already exists, we must be careful. 
    if (std::filesystem::exists(new_local_path)) {   // whether this new local path exists
        if (std::filesystem::is_directory(new_local_path)) {    // if it exists, whether it is a file or a directory, we don't allow it 
            // If target is a folder and the new name is an existing folder with contents in it, forbid the move.
            if (!std::filesystem::is_empty(new_local_path)) {
                std::cerr << "Error: Target '" << to_name << "' is a non-empty directory." << std::endl;
                return false; 
            }
            // If target is an empty folder, it's safe to remove it to make way.
            std::filesystem::remove(new_local_path);
        } else {
            // If target is a file, standard rules allow overwriting (Atomic Save). *****
            std::filesystem::remove(new_local_path);
        }
    }

    // 3. Perform the Physical Rename (Moves folder AND contents)
    try {
        std::filesystem::rename(old_local_path, new_local_path);
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Rename failed: " << e.what() << std::endl;
        return false;
    }

    // 4. Update In-Memory Maps
    // We must update the keys for the folder AND all files inside it.
    
    std::string old_server_path = resolved_path + (resolved_path.back() == '/' ? "" : "/") + from_name;
    std::string new_server_path = resolved_path_new + (resolved_path_new.back() == '/' ? "" : "/") + to_name;


    // helper function to update keys in the map
    auto update_map_keys = [&](auto& map, const std::string& old_p, const std::string& new_p) {
        auto it = map.begin();
        while (it != map.end()) {
            const std::string& key = it->first;
            
            // 1. Check if it strictly starts with the old path
            if (key.rfind(old_p, 0) == 0) {
                
                // 2. SAFETY CHECK: Ensure we aren't matching a partial folder name.
                // valid if:
                //   a) key is EXACTLY old_p (Renaming a specific file)
                //   b) key continues with '/' (Renaming a directory containing this file)
                
                bool is_exact_match = (key.length() == old_p.length());
                bool is_child = (key.length() > old_p.length() && key[old_p.length()] == '/');
                
                if (is_exact_match || is_child) {
                    // Correctly perform the replacement
                    std::string suffix = key.substr(old_p.length());
                    std::string new_key = new_p + suffix;

                    map[new_key] = std::move(it->second);
                    it = map.erase(it);
                    continue; // Move to next iteration
                }
            }
            ++it;
        }
    };

    // Update all client structures
    update_map_keys(cache, old_local_path, new_local_path);
    update_map_keys(opened_files, old_local_path, new_local_path);
    update_map_keys(cached_attr, old_server_path, new_server_path);

    // 5. Send RPC to Server (Implementation depends on your proto)
    grpc::ClientContext context;  
    afs_operation::RenameRequest request;
    afs_operation::RenameResponse response;
    request.set_new_filename(to_name);
    request.set_filename(from_name);
    request.set_directory(resolved_path);
    request.set_new_directory(resolved_path_new);

    grpc::Status status = stub_ -> rename(&context, request, &response);

    if (!status.ok()) {
        if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
            // This is fine! It means the file is new and exists ONLY in our local cache.
            // We just proceed to rename it locally.
        } else {
            std::cerr << "Server rename failed: " << status.error_message() << std::endl;
            return false; 
        }
    }

    std::cout << "Successfully renamed '" << from_name << "' to '" << to_name << "'" << std::endl;
    return true;
}



bool FileSystemClient::truncate_file(const std::string& filename, const std::string& path, const int size){
    std::string resolved_path = resolve_server_path(path);
    std::string cache_path = std::string("./tmp/cache") + (resolved_path[0] == '/'? "" : "/" ) + resolved_path +filename;
    try{
        std::filesystem::resize_file(cache_path, size);
    } catch(std::filesystem::filesystem_error& e){
        std::cerr << "Error: " << e.what() << '\n';
        std::cerr << "Path1: " << e.path1() << '\n';
        std::cerr << "Error code: " << e.code().message() << '\n';
        return false;
    }
    return true;
}


bool FileSystemClient::make_directory(const std::string& directory, const uint32_t mode){
    std::string resolved_path = resolve_server_path(directory);
    grpc::ClientContext context;
    afs_operation::MakeDir_request request;
    afs_operation::MakeDir_response response;
    request.set_directory(resolved_path);
    request.set_mode(mode);
    
    grpc::Status status = stub_ -> mkdir(&context, request, &response);
    if (!status.ok()){
        std::cout << "Directory Creation Failed: " << status.error_message() << std::endl;
        return false;
    }
    return true;
}

bool FileSystemClient::delete_file(const std::string& directory){
    std::string resolved_path = resolve_server_path(directory);
    std::string cache_path = std::string("./tmp/cache") + (resolved_path[0] == '/'? "" : "/" ) + resolved_path;
    std::filesystem::path path(directory);
    
    // we need to clean the the three caches we have 
    auto it_op = opened_files.find(cache_path);
    if (it_op != opened_files.end()){
        // we find it in opened_files
        opened_files.erase(it_op);   // erase by iterator (O(1))
        std::cout << "Delete file: "<< cache_path << "in opened_files";
    }

    auto it_ca = cache.find(cache_path);
    if (it_ca != cache.end()){
        cache.erase(it_ca);
        std::cout << "Delete file in: "<< cache_path << "in cache";
    }

    auto it_attr = cached_attr.find(resolved_path);
    if (it_attr != cached_attr.end()){
        cached_attr.erase(it_attr);
        std::cout << "Delete file in: "<< resolved_path << "in cached_attr";
    }

    // And then we actually delete the files physically
    grpc::ClientContext context;
    afs_operation::Delete_request request;
    afs_operation::Delete_response response;
    request.set_directory(resolved_path);
    grpc::Status status = stub_ -> unlink(&context, request, &response);
    if (!status.ok()){
        std::cout << "Directory Deletion Failed: " << status.error_message() << std::endl;
        return false;
    }
    // deletion successful, but don't forget to delete the file in the cache of the client (./tmp/cache)
    std::error_code ec;
    if (std::filesystem::remove(cache_path, ec) || !ec){
        std::cout << "File deleted successfully on the local cache at: " << cache_path << std::endl;
    }else{
        std::cerr << "Warning: Failed to remove local cache file: " << ec.message() << std::endl;
    }
    return true;
}




