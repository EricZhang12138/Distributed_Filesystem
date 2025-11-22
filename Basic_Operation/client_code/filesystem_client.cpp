#include "filesystem_client.hpp"
#include <iostream>
#include <sstream> 
#include <chrono>
#include <thread>

#include <vector> 
#include <sys/stat.h>


 
FileSystemClient::FileSystemClient(std::shared_ptr<grpc::Channel> channel) : stub_(afs_operation::operators::NewStub(channel)){
    afs_operation::InitialiseRequest request;
    request.set_code_to_initialise("I want input/output directory");
    afs_operation::InitialiseResponse response;
    grpc::ClientContext context;
    grpc::Status status = stub_ -> request_dir(&context, request, &response);
    if (!status.ok()){
        std::cerr << "Unable to retrieve the path for input/output files on the server" << std::endl;
        this->server_root_path_ ="/"; // default when it fails
    }else{
        // Store the root path instead of just printing it
        this->server_root_path_ = response.root_path(); 
        std::cerr << "Client initialized. Server root directory: " << this->server_root_path_ << std::endl; 
    }
}

// Going from a path you want to access to the path in cache on the client
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

std::optional<FileSystemClient::FileAttributes> FileSystemClient::get_attributes(const std::string& filename, const std::string& path) {
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
            std::cerr<< "The file is already open, but it is okay if you try multiple times" << std::endl;
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
            cache[file_location].is_changed = false;
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

            if (file_stream.fail()) {
                std::cerr << "Error: Failed to write data to " << filename << std::endl;
                return false;
            }
            
            // 1. Mark the file content as changed for eventual upload
            cache[file_location].is_changed = true;

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

    if (cache_it->second.is_changed) {
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




// --- NEW: Consistency Checker Function ---
// This simulates what the FUSE kernel does to verify if the "metadata" (RAM) 
// matches the "data" (Disk)
bool verify_metadata_consistency(FileSystemClient& client, const std::string& filename, const std::string& directory) {
    // 1. Reconstruct the cache key (Simulation of what FUSE does)
    // Note: This simple concatenation assumes 'directory' matches the server path structure exactly
    // If directory has trailing slash or not, we handle it
    std::string cache_key = client.resolve_server_path(directory) + (directory.back() == '/' ? "" : "/") + filename;

    // 2. Check FUSE Metadata Cache (cached_attr)
    auto attr_it = client.cached_attr.find(cache_key);
    if (attr_it == client.cached_attr.end()) {
        std::cerr << "[Consistency Check] FAIL: File '" << filename << "' is missing from cached_attr." << std::endl;
        return false;
    }
    const auto& attr = attr_it->second;

    // 3. Check Physical Disk Cache
    std::string local_path = "./tmp/cache" + cache_key;
    struct stat local_stat;
    if (stat(local_path.c_str(), &local_stat) != 0) {
        std::cerr << "[Consistency Check] FAIL: Physical file missing at " << local_path << std::endl;
        return false;
    }

    // 4. Verify SIZE Consistency (Most Critical)
    // If these differ, FUSE reads will be truncated or garbage.
    if (attr.size != local_stat.st_size) {
        std::cerr << "[Consistency Check] FAIL: Size Mismatch!" << std::endl;
        std::cerr << "  cached_attr (Logical): " << attr.size << " bytes" << std::endl;
        std::cerr << "  physical (Actual):     " << local_stat.st_size << " bytes" << std::endl;
        return false;
    }

    // 5. Verify TIME Validity
    // We ensure mtime is valid (non-zero).
    // Note: We do NOT strictly compare attr.mtime vs local_stat.mtime because 
    // attr.mtime is "Server Time" and local_stat.mtime is "Local Write Time".
    // They are allowed to differ slightly, but neither should be 0.
    if (attr.mtime == 0) {
        std::cerr << "[Consistency Check] FAIL: MTime is 0 (Invalid)." << std::endl;
        return false;
    }

    std::cout << "[Consistency Check] PASS: Metadata consistent. Size=" << attr.size << ", Time=" << attr.mtime << std::endl;
    return true;
}

/*
int main() {
    // 1. Setup Connection
    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        address,
        grpc::InsecureChannelCredentials()
    );
    FileSystemClient client(channel);
    std::cout << ">>> CLIENT STARTED <<<\n" << std::endl;

    std::string filename = "consistency_check.txt";
    std::string dir = "/test_consistency"; 

    // =================================================================================
    // STEP 1: CREATE & WRITE
    // =================================================================================
    std::cout << "--- STEP 1: Create & Write Local ---" << std::endl;
    if (!client.create_file(filename, dir)) {
        client.open_file(filename, dir); // Recover if exists
    }
    
    std::string data = "Checking metadata integrity is fun.";
    client.write_file(filename, data, dir, 0);

    // VERIFICATION 1:
    // Metadata should match the 35 bytes we just wrote
    if (!verify_metadata_consistency(client, filename, dir)) {
        std::cerr << "!!! TEST FAILED AT STEP 1 !!!" << std::endl;
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));


    // =================================================================================
    // STEP 2: CLOSE (SYNC)
    // =================================================================================
    std::cout << "\n--- STEP 2: Close (Flush to Server) ---" << std::endl;
    client.close_file(filename, dir);
    
    // VERIFICATION 2:
    // Metadata should STILL match 35 bytes.
    // Timestamp should now be Server Time (we can't verify value easily here, but we verify it's valid)
    if (!verify_metadata_consistency(client, filename, dir)) {
        std::cerr << "!!! TEST FAILED AT STEP 2 !!!" << std::endl;
        return 1;
    }

    // =================================================================================
    // STEP 3: RE-OPEN
    // =================================================================================
    std::cout << "\n--- STEP 3: Re-Open (Check Staleness) ---" << std::endl;
    // Sleep to force local time to move forward, ensuring strict timestamp checks are working
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    client.open_file(filename, dir);
    
    // VERIFICATION 3:
    // Metadata should still be consistent after the open logic runs
    if (!verify_metadata_consistency(client, filename, dir)) {
        std::cerr << "!!! TEST FAILED AT STEP 3 !!!" << std::endl;
        return 1;
    }

    std::cout << "\n>>> ALL SYSTEMS GO: METADATA IS CONSISTENT <<<" << std::endl;
    return 0;
}


*/

/*
// --- Helper function for testing ---
// This function neatly prints the attributes returned from your get_attributes call
void print_attributes(const std::string& name, std::optional<FileSystemClient::FileAttributes> attrs_opt) {
    if (!attrs_opt) {
        std::cout << "  Failed to get attributes for '" << name << "' (File not found, this is OK)" << std::endl;
        return;
    }

    auto attrs = *attrs_opt;
    std::cout << "  Attributes for '" << name << "':" << std::endl;
    
    // Print mode in octal (e.g., 0755) and check if it's a file or directory
    std::cout << "    Mode:  0" << std::oct << attrs.mode << std::dec << " (";
    if (S_ISDIR(attrs.mode)) std::cout << "Directory";
    else if (S_ISREG(attrs.mode)) std::cout << "File";
    else std::cout << "Other";
    std::cout << ")" << std::endl;

    std::cout << "    Size:  " << attrs.size << " bytes" << std::endl;
    std::cout << "    Links: " << attrs.nlink << std::endl;
    std::cout << "    Owner: uid=" << attrs.uid << ", gid=" << attrs.gid << std::endl;
    std::cout << "    MTime: " << attrs.mtime << " (Last Modification Time)" << std::endl;
    std::cout << "    ATime: " << attrs.atime << " (Last Access Time)" << std::endl;
    std::cout << "    CTime: " << attrs.ctime << " (Last Status Change Time)" << std::endl;
}


int main() {
    // 1. Setup connection
    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        address,
        grpc::InsecureChannelCredentials()
    );

    FileSystemClient client(channel);
    std::cout << "Client connected to " << address << std::endl;

    // --- Scenario 1: Test Positional Write ---
    std::cout << "\n--- Scenario 1: Create, Write, Overwrite ---" << std::endl;
    std::string filename = "positional_test.txt";
    std::string path = "test_dir"; // Using a new directory

    // Create the file
    std::cout << "Attempting to create: " << path << "/" << filename << std::endl;
    if (!client.create_file(filename, path)) {
        std::cerr << "Create failed. (File might already exist from a previous run.)" << std::endl;
        std::cerr << "Attempting to open it instead..." << std::endl;
        // If create fails, try opening it. If open also fails, then exit.
        if (!client.open_file(filename, path)) {
            std::cerr << "Failed to create OR open file. Exiting." << std::endl;
            return 1;
        }
         std::cout << "File opened successfully. Proceeding to overwrite." << std::endl;
    } else {
        std::cout << "Create success." << std::endl;
    }

    // Write "Hello, world!" at pos 0
    std::string data1 = "Hello, world!";
    std::cout << "Writing '" << data1 << "' at position 0..." << std::endl;
    // Note: The function signature is (filename, data, path, position)
    if (!client.write_file(filename, data1, path, 0)) {
        std::cerr << "Failed to write data1." << std::endl;
    }

    // Write "everyone" at pos 7 (to overwrite "world")
    std::string data2 = "everyone!";
    std::cout << "Writing '" << data2 << "' at position 7..." << std::endl;
    if (!client.write_file(filename, data2, path, 7)) { // "Hello, " is 7 chars
        std::cerr << "Failed to write data2." << std::endl;
    }
    // At this point, the local file content should be "Hello, everyone!"

    // Close the file to flush changes
    std::cout << "Closing file to flush to server..." << std::endl;
    if (!client.close_file(filename, path)) {
        std::cerr << "Failed to close file." << std::endl;
    } else {
        std::cout << "File flushed to server." << std::endl;
    }

    // --- Scenario 2: Verification ---
    std::cout << "\n--- Scenario 2: Re-open and Verify Content ---" << std::endl;
    
    std::cout << "Opening '" << filename << "' for verification..." << std::endl;
    if (!client.open_file(filename, path)) {
        std::cerr << "Failed to re-open file for verification." << std::endl;
        return 1;
    }

    std::cout << "Reading content from file..." << std::endl;
    
    std::vector<char> read_buffer;
    // We know the content is "Hello, everyone!" (17 chars).
    // Let's read 100 bytes from offset 0 to test.
    const int read_size = 100;
    const int read_offset = 0;

    // Call the new, correct read_file function
    if (client.read_file(filename, path, read_size, read_offset, read_buffer)) {
        
        // Convert the vector<char> to a std::string for comparison
        std::string content(read_buffer.begin(), read_buffer.end());
        
        std::cout << "Read: '" << content << "'" << std::endl;
        
        std::string expected = "Hello, everyone!";
        if (content == expected) {
            std::cout << "SUCCESS: Content is correct!" << std::endl;
        } else {
            std::cout << "FAILURE: Expected '" << expected << "'" << std::endl;
            std::cout << "Read size: " << content.size() << ", Expected size: " << expected.size() << std::endl;
        }
    } else {
        std::cerr << "Failed to read from file." << std::endl;
    }



    // Get Attributes
    auto attributes = client.get_attributes("test.txt", "/ABC");
    print_attributes("test.txt", attributes);


    // Clean up
    std::cout << "Closing file." << std::endl;
    client.close_file(filename, path);

    // --- Scenario 3: Directory Listing ---
    std::cout << "\n--- Scenario 3: Listing Root Directory ---" << std::endl;
    client.ls_contents(""); // List root

    std::cout << "\n--- Scenario 4: Listing New Directory (" << path << ") ---" << std::endl;
    client.ls_contents(path); // List "test_dir"

    std::cout << "\n--- Script finished ---" << std::endl;
    return 0;
}*/