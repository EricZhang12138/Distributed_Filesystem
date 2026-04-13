#include "filesystem_client.hpp"
#include "client_subscriber.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <vector>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <cstring>
#include <cerrno>
#include <thread>



 
afs_operation::operators::Stub* FileSystemClient::get_data_stub(const std::string& address) {
    std::lock_guard<std::mutex> lock(stubs_mutex_);
    auto it = data_stubs_.find(address);
    if (it != data_stubs_.end()) {
        return it->second.get();
    }
    // Create a new stub for this data server
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto stub = afs_operation::operators::NewStub(channel);
    auto* raw = stub.get();
    data_stubs_[address] = std::move(stub);
    return raw;
}

FileSystemClient::FileSystemClient(std::shared_ptr<grpc::Channel> metadata_channel, std::string cache_path) : metadata_stub_(metadata::MetadataService::NewStub(metadata_channel)), cache_directory(cache_path){
    // Generate a universally unique identifier for client ID
    boost::uuids::random_generator gen;
    boost::uuids::uuid id = gen();
    client_id = boost::uuids::to_string(id); // convert to string
    std::cout << "Client ID: " << client_id << std::endl;

    // Call metadata server Init to get root path
    metadata::MetadataInitRequest meta_request;
    meta_request.set_client_id(client_id);
    metadata::MetadataInitResponse meta_response;
    grpc::ClientContext meta_context;
    grpc::Status status = metadata_stub_->Init(&meta_context, meta_request, &meta_response);

    if (!status.ok()){
        std::cerr << "FATAL: Unable to connect to metadata server: " << status.error_message() << std::endl;
        throw std::runtime_error("Failed to initialize: metadata server unreachable");
    }

    this->server_root_path_ = meta_response.root_path();
    std::cerr << "Client initialized via metadata server. Root: " << this->server_root_path_ << std::endl;

    // Phase 2: No direct data server connection at startup.
    // Data servers are discovered per-file via Lookup/CreateFile.
    // Subscriber connects to metadata server instead of data server.

    subscriber_context_ = std::make_unique<grpc::ClientContext>();
    subscriber_thread = std::thread(&FileSystemClient::RunSubscriber, this);
}


FileSystemClient::~FileSystemClient(){
    // stop the thread using subscriber_context
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
    cache_mutex.lock();
    auto it = cached_attr.find(file_loca_server);
    if (it != cached_attr.end()){   // this means file_loca_server exists in the cached_attr already
        FileAttributes result = it->second; // copy under lock before another thread invalidates the iterator
        cache_mutex.unlock();
        return result;
    }
    cache_mutex.unlock();

    afs_operation::GetAttrResponse response;
    grpc::ClientContext context;

    grpc::Status status = metadata_stub_->getattr(&context, request, &response);

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
    cache_mutex.lock();
    cached_attr[file_loca_server] = attrs;
    cache_mutex.unlock();
    
    return attrs;
}


bool FileSystemClient::open_file(std::string filename, std::string path){
    std::string resolved_path = resolve_server_path(path);
    std::cout << "DEBUG: Opening '" << filename << "' at resolved path: " << resolved_path << std::endl;

    std::string cache_dir = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") +resolved_path; // Use resolved_path
    std::string file_location = cache_dir + (cache_dir.back() == '/' ? "" : "/") + filename;

    // Lookup: ask metadata server which data server holds this file
    std::string file_server_path = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::string ds_address;
    std::string ds_root;
    {
        grpc::ClientContext lookup_ctx;
        metadata::LookupRequest lookup_req;
        metadata::LookupResponse lookup_resp;
        lookup_req.set_path(file_server_path);
        grpc::Status lookup_status = metadata_stub_->Lookup(&lookup_ctx, lookup_req, &lookup_resp);
        if (!lookup_status.ok()) {
            std::cerr << "Lookup failed for " << file_server_path << ": " << lookup_status.error_message() << std::endl;
            return false;
        }
        ds_address = lookup_resp.data_server_address();
        ds_root = lookup_resp.data_server_root();
    }

    // Register interest so we get notifications for this file
    {
        grpc::ClientContext reg_ctx;
        metadata::RegisterFileInterestRequest reg_req;
        metadata::RegisterFileInterestResponse reg_resp;
        reg_req.set_path(file_server_path);
        reg_req.set_client_id(client_id);
        metadata_stub_->RegisterFileInterest(&reg_ctx, reg_req, &reg_resp);
    }

    auto* ds_stub = get_data_stub(ds_address);

    // Build data-server-local path for RPCs (ds_root + virtual_path)
    std::string ds_resolved_path = ds_root + resolved_path;

    // Case 1: File is NOT in the local cache
    cache_mutex.lock();
    if (cache.find(file_location) == cache.end()){
        cache_mutex.unlock();
        // Create the local directory structure if it doesn't exist
        try {
            std::filesystem::create_directories(cache_dir);
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error creating local cache directory: " << e.what() << std::endl;
            return false; // Fail early if the directory can't be created
        }

        afs_operation::FileRequest request;
        request.set_filename(filename);
        request.set_directory(ds_resolved_path);
        request.set_client_id(client_id);

        std::string file_path = file_location; // Use the full file_location path

        afs_operation::FileResponse response_temp;

        int num_of_retries = 0;
        grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");

        while(num_of_retries < 3 && !status.ok()){
            grpc::ClientContext context;
            std::unique_ptr<grpc::ClientReader<afs_operation::FileResponse>> reader(ds_stub->open(&context, request));

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
                struct FileInfo file_info{false, last_timestamp, filename, ds_address, ds_root};
                cache_mutex.lock();
                cache[file_location] = file_info;
                cache_mutex.unlock();
                std::cout << "File cached successfully" << std::endl;
            }
            // update cached_attr
            std::string server_key = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
            cache_mutex.lock();
            auto attr_it = cached_attr.find(server_key);
            if (attr_it != cached_attr.end()) {
                attr_it->second.mtime = last_timestamp;
                attr_it->second.atime = last_timestamp;
                attr_it->second.ctime = last_timestamp;
                try {
                    attr_it->second.size = std::filesystem::file_size(file_location);
                } catch (...) {}
                std::cout << "Synced cached_attr for new download." << std::endl;
            }
            cache_mutex.unlock();

            num_of_retries++;
        }

        if (!status.ok()){
            std::cerr << "After 3 tries, Failed to Open the file from the server" << std::endl;
            std::filesystem::remove(file_path); // Clean up partial file
            return false;
        }

        auto read_stream = std::make_shared<std::ifstream>(file_location, std::ios::binary);
        auto write_stream = std::make_shared<std::ofstream>(file_location, std::ios::binary | std::ios::in);

        if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
            std::cerr << "Failed to open local file streams after download." << std::endl;
            cache_mutex.lock();
            cache.erase(file_location);
            cache_mutex.unlock();
            return false;
        }
        cache_mutex.lock();
        opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
        // Create per-file mutex if not present
        if (file_mutexes.find(file_location) == file_mutexes.end()) {
            file_mutexes[file_location] = std::make_shared<std::mutex>(); // default-constructs a mutex
        }
        cache_mutex.unlock();
        return true;

    }else {
        // found the file in cache
        if (opened_files.find(file_location) != opened_files.end()){
            std::cout << "The file: " << file_location << " is already open" << std::endl;
            cache_mutex.unlock();
            return true;
        }
        cache_mutex.unlock();
        // File is in cache but not open - just open the streams
        // No need to compare with server since subscriber invalidates cache when needed
        std::cout << "File: " << file_location << " found in cache, opening..." << std::endl;

        auto read_stream = std::make_shared<std::ifstream>(file_location, std::ios::binary);
        auto write_stream = std::make_shared<std::ofstream>(file_location, std::ios::binary | std::ios::in);

        if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
            std::cerr << "Failed to open the local file stream for: " << file_location << std::endl;
            return false;
        }

        // Update stored server address/root in case it changed
        cache_mutex.lock();
        auto cache_it = cache.find(file_location);
        if (cache_it != cache.end()) {
            cache_it->second.server_address = ds_address;
            cache_it->second.server_root = ds_root;
        }
        opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
        if (file_mutexes.find(file_location) == file_mutexes.end()) {
            file_mutexes[file_location] = std::make_shared<std::mutex>();
        }
        cache_mutex.unlock();
        std::cout << "File '" << filename << "' is now open for use." << std::endl;
        return true;

    }
    
    /*else { 
        cache_mutex.unlock();
        // Case 2: File IS in the local cache, check for updates
        
        // Check if file is already open by this client
        cache_mutex.lock();
        if (opened_files.find(file_location)!=opened_files.end()){
            std::cerr<< "The file: " << file_location << "is already open, but it is okay if you try multiple times" << std::endl;
            cache_mutex.unlock();
            return true;
        }
        int64_t timestamp = cache[file_location].timestamp;
        cache_mutex.unlock();
        afs_operation::FileRequest request;
        request.set_filename(filename);
        request.set_timestamp(timestamp);
        request.set_directory(resolved_path); 
        request.set_client_id(client_id);

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
            cache_mutex.lock();
            cache[file_location].timestamp = new_timestamp;
            cache[file_location].locally_modified = false;
            cache_mutex.unlock();
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
            std::string server_key = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
            cache_mutex.lock();
            auto attr_it = cached_attr.find(server_key);
            if (attr_it != cached_attr.end()) {
                // Update timestamp to what the server just sent us
                attr_it->second.mtime = new_timestamp; 
                attr_it->second.atime = new_timestamp;
                attr_it->second.ctime = new_timestamp;
                // Update size from the actual file on disk
                try {
                    attr_it->second.size = std::filesystem::file_size(file_location);
                } catch (...) {}
                std::cout << "Synced cached_attr after update." << std::endl;
            }
            cache_mutex.unlock();
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
        cache_mutex.lock();
        opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
        // Create per-file mutex if not present
        if (file_mutexes.find(file_location) == file_mutexes.end()) {
            file_mutexes[file_location];
        }
        cache_mutex.unlock();
        std::cout << "File '" << filename << "' is now open for use." << std::endl;
        return true;
    }*/
}

bool FileSystemClient::read_file(const std::string& filename, const std::string& directory, const int size, const int offset, std::vector<char>& buffer){

    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") + resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;

    // Capture both the per-file mutex and the stream as shared_ptrs under cache_mutex,
    // so we can drop cache_mutex without leaving dangling iterators or raw pointers.
    std::shared_ptr<std::mutex> file_mtx;
    std::shared_ptr<std::ifstream> read_stream;
    {
        std::lock_guard<std::mutex> g(cache_mutex);
        if (cache.find(file_location) == cache.end()) {
            std::cerr << "File not in cache. Get the file from the server by calling open_file()" << std::endl;
            return false;
        }
        auto it = opened_files.find(file_location);
        if (it == opened_files.end()) {
            std::cerr << "File found in cache but it is not opened. Open the file by calling open_file()" << std::endl;
            return false;
        }
        auto m_it = file_mutexes.find(file_location);
        if (m_it == file_mutexes.end()) {
            std::cerr << "No mutex for file: " << file_location << " when reading" << std::endl;
            return false;
        }
        file_mtx = m_it->second;
        read_stream = it->second.read_stream;
    }
    std::lock_guard<std::mutex> file_lock(*file_mtx);
    std::ifstream& file_stream = *read_stream;
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


bool FileSystemClient::write_file(const std::string& filename, const std::string& data, const std::string& directory, std::streampos position){

    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") + resolved_path + (resolved_path.back()=='/' ? "" : "/") + filename; 

    // Capture the per-file mutex and the stream as shared_ptrs under cache_mutex,
    // so we can drop cache_mutex without leaving dangling iterators or raw pointers.
    std::shared_ptr<std::mutex> file_mtx;
    std::shared_ptr<std::ofstream> write_stream;
    {
        std::lock_guard<std::mutex> g(cache_mutex);
        if (cache.find(file_location) == cache.end()) {
            std::cerr << "File not in cache. Get the file from the server by calling open_file()" << std::endl;
            return false;
        }
        auto it = opened_files.find(file_location);
        if (it == opened_files.end()) {
            std::cerr << "File found in cache but it is not opened. Open the file by calling open_file()" << std::endl;
            return false;
        }
        auto m_it = file_mutexes.find(file_location);
        if (m_it == file_mutexes.end()) {
            std::cerr << "No mutex for file: " << file_location << " when writing" << std::endl;
            return false;
        }
        file_mtx = m_it->second;
        write_stream = it->second.write_stream;
    }

    std::lock_guard<std::mutex> file_lock(*file_mtx);
    std::ofstream& file_stream = *write_stream;
    file_stream.seekp(position);
    if (file_stream.fail()) {
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

    // Mark the file content as changed for eventual upload, and update cached_attr.
    // Single critical section instead of three lock/unlock pairs.
    {
        std::lock_guard<std::mutex> g(cache_mutex);
        auto cache_it = cache.find(file_location);
        if (cache_it != cache.end()) {
            cache_it->second.locally_modified = true;
        }

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
    }

    std::cout << "Successfully wrote to " << filename << " and marked as changed." << std::endl;
    return true;
}

bool FileSystemClient::create_file(const std::string& filename, const std::string& path) {

    std::string resolved_path = resolve_server_path(path);
    std::string file_location = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") + resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::string cache_dir = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") + resolved_path;

    cache_mutex.lock();
    if (cache.count(file_location) || opened_files.count(file_location)) {
        std::cerr << "Error: File '" << filename << "' already exists." << std::endl;
        cache_mutex.unlock();
        return false;
    }
    cache_mutex.unlock();

    // Ask metadata server to create the file and assign a data server
    std::string file_server_path = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::string ds_address;
    std::string ds_root;
    {
        grpc::ClientContext ctx;
        metadata::CreateFileRequest req;
        metadata::CreateFileResponse resp;
        req.set_path(file_server_path);
        req.set_mode(0644);
        req.set_client_id(client_id);
        grpc::Status status = metadata_stub_->CreateFile(&ctx, req, &resp);
        if (!status.ok()) {
            std::cerr << "CreateFile RPC failed: " << status.error_message() << std::endl;
            return false;
        }
        ds_address = resp.data_server_address();
        ds_root = resp.data_server_root();
    }

    // Register interest for notifications
    {
        grpc::ClientContext reg_ctx;
        metadata::RegisterFileInterestRequest reg_req;
        metadata::RegisterFileInterestResponse reg_resp;
        reg_req.set_path(file_server_path);
        reg_req.set_client_id(client_id);
        metadata_stub_->RegisterFileInterest(&reg_ctx, reg_req, &reg_resp);
    }

    // Create the local directory structure if it doesn't exist
    try {
        std::filesystem::create_directories(cache_dir);
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

    struct FileInfo file_info{true, 0, filename, ds_address, ds_root};
    cache_mutex.lock();
    cache[file_location] = file_info;
    cache_mutex.unlock();

    auto read_stream = std::make_shared<std::ifstream>(file_location, std::ios::binary);
    auto write_stream = std::make_shared<std::ofstream>(file_location, std::ios::binary | std::ios::in);

    if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
        std::cerr << "Failed to open newly created file stream for: " << file_location<< std::endl;
        cache_mutex.lock();
        cache.erase(file_location); 
        cache_mutex.unlock();
        return false;
    }
    cache_mutex.lock();
    opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
    // Create per-file mutex
    file_mutexes[file_location] = std::make_shared<std::mutex>();
    cache_mutex.unlock();

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
    cache_mutex.lock();
    cached_attr[file_loca_server] = attr;
    cache_mutex.unlock();
    std::cout << "Successfully created and opened '" << filename << "' for writing." << std::endl;
    return true;
}


bool FileSystemClient::close_file(const std::string& filename, const std::string& directory) {

    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") + resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;

    // 1. Lock Global State
    std::unique_lock<std::mutex> global_lock(cache_mutex);

    auto opened_file_it = opened_files.find(file_location);
    if (opened_file_it == opened_files.end()) {
        std::cerr << "Error: Cannot close '" << filename << "' because it is not open." << std::endl;
        return false;
    }

    auto m_it = file_mutexes.find(file_location);
    if (m_it == file_mutexes.end()) {
        std::cerr << "No mutex for file: " << file_location << " when closing" << std::endl;
        return false;
    }
    // Copy shared_ptr so mutex stays alive even if erased from map
    std::shared_ptr<std::mutex> file_mtx = m_it->second;

    auto cache_it = cache.find(file_location);
    if (cache_it == cache.end()) {
        opened_files.erase(opened_file_it); 
        file_mutexes.erase(file_location);
        std::cerr << "Error: Inconsistent state. File is open but not in cache." << std::endl;
        return false;
    }


    bool needs_flush = cache_it->second.locally_modified;
    std::string ds_address = cache_it->second.server_address;
    std::string ds_root = cache_it->second.server_root;
    // shared_ptr copies keep the streams alive even if another thread replaces or
    // erases the opened_files entry between dropping global_lock and taking file_lock.
    std::shared_ptr<std::ofstream> write_stream = opened_file_it->second.write_stream;
    std::shared_ptr<std::ifstream> read_stream  = opened_file_it->second.read_stream;


    // 2. Unlock Global Mutex (Prevent freezing the whole client during upload)
    global_lock.unlock();

    // 3. Lock File Mutex (Prevent local modification during upload)
    std::lock_guard<std::mutex> file_lock(*file_mtx);

    if (needs_flush) {
        std::cout << "File '" << filename << "' was modified. Flushing to server..." << std::endl;

        write_stream->flush();

        if(write_stream->fail()) {
            std::cerr << "Error: Failed to flush write stream before closing." << std::endl;
            return false;
        }

        // Close streams to release OS locks
        read_stream->close();
        write_stream->close();
        
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
        
        // RPC Loop — use data server stub for the file's owning server
        auto* ds_stub = get_data_stub(ds_address);
        while (num_of_tries < 3 && !status.ok()){
            grpc::ClientContext context;
            std::unique_ptr<grpc::ClientWriter<afs_operation::FileRequest>> writer(
                ds_stub->close(&context, &response)
            );
            
            
            file_stream.clear(); // Clear EOF flag
            file_stream.seekg(0, std::ios::beg); // Rewind to start

            bool sent_at_least_once = false;
            while(true){
                file_stream.read(buffer, chunk_size);
                std::streamsize len = file_stream.gcount();

                // Always send at least one request, even if file is empty
                if(len <= 0 && sent_at_least_once) break;

                afs_operation::FileRequest request;
                request.set_directory(ds_root + resolved_path);
                request.set_filename(filename);
                request.set_content(buffer, len);
                request.set_client_id(client_id);

                if (!writer->Write(request)){
                    break;
                }
                sent_at_least_once = true;

                if(len <= 0) break; // Break after sending empty chunk
            }
            std::cout << "[CLIENT] Calling WritesDone()..." << std::endl;
            writer->WritesDone();
            std::cout << "[CLIENT] WritesDone() complete, calling Finish()..." << std::endl;
            std::cout.flush(); // Force output before potentially blocking call
            status = writer->Finish();
            std::cout << "[CLIENT] Finish() returned with status: " << status.error_code() << std::endl;
            num_of_tries ++;
        }
        
        file_stream.close();
        
        if (!status.ok()) {
            std::cerr << "RPC failed while flushing file to server: " << status.error_message() << std::endl;
            return false; 
        }

        // 4. Update Metadata (MUST Re-Lock Global)
        global_lock.lock();

        // SAFETY: We must find the cache entry again. 
        // The old 'cache_it' is invalid because we unlocked the mutex earlier.
        auto fresh_cache_it = cache.find(file_location);
        if (fresh_cache_it != cache.end()) {
            fresh_cache_it->second.timestamp = response.timestamp();
            fresh_cache_it->second.locally_modified = false;
        }
        
        std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
        auto attr_it = cached_attr.find(file_loca_server);
        if (attr_it != cached_attr.end()) {
            attr_it->second.mtime = response.timestamp();
            attr_it->second.ctime = response.timestamp();
            try { 
                attr_it->second.size = std::filesystem::file_size(file_location); 
            } catch (...) {}
        }

        std::cout << "File flushed successfully." << std::endl;
        
        // global_lock is still held here, ready for erase
    } else {
        std::cout << "File '" << filename << "' was not modified. No flush needed." << std::endl;
        // Re-lock to perform erasure
        global_lock.lock();
    }

    // 5. Final Cleanup
    // We erase by KEY, which is safer than using the old (potentially stale) iterator
    opened_files.erase(file_location);
    file_mutexes.erase(file_location);
    
    // global_lock unlocks automatically when function returns
    
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

    grpc::Status status = metadata_stub_->ls(&context, request, &response);
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
    std::cout << "[RENAME] from='" << from_name << "' to='" << to_name << "'" << std::endl;
    std::string resolved_path = resolve_server_path(old_path);
    std::string resolved_path_new = resolve_server_path(new_path);

    // 1. Prepare Paths
    // We add a trailing slash to base_dir so we can build generic paths safely
    std::string base_dir = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") + resolved_path + (resolved_path.back() == '/' ? "" : "/");
    std::string base_dir_new = std::string(cache_directory)+ (resolved_path_new.front() == '/' ? "" : "/")+ resolved_path_new + (resolved_path_new.back() == '/' ? "" : "/");
    std::string old_local_path = base_dir + from_name;
    std::string new_local_path = base_dir_new + to_name;

    std::cout << "[RENAME] old_local_path='" << old_local_path << "'" << std::endl;
    std::cout << "[RENAME] new_local_path='" << new_local_path << "'" << std::endl;
    std::cout << "[RENAME] Old exists? " << std::filesystem::exists(old_local_path) << std::endl;

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
    // Only rename locally if the item exists in cache
    if (std::filesystem::exists(old_local_path)) {
        try {
            // Ensure the destination's cache parent directory exists.
            // Without this, moving a file into a folder whose cache subdir
            // hasn't been materialized yet fails with ENOENT on the rename.
            std::error_code mk_ec;
            std::filesystem::create_directories(
                std::filesystem::path(new_local_path).parent_path(), mk_ec);
            std::filesystem::rename(old_local_path, new_local_path);
            std::cout << "[RENAME] Successfully renamed locally" << std::endl;
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Rename failed: " << e.what() << std::endl;
            return false;
        }
    } else {
        std::cout << "[RENAME] Item not in local cache, skipping local rename (will rename on server)" << std::endl;
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

    cache_mutex.lock();
    update_map_keys(cache, old_local_path, new_local_path);
    update_map_keys(opened_files, old_local_path, new_local_path);
    update_map_keys(cached_attr, old_server_path, new_server_path);
    cache_mutex.unlock();

    // 5. Send RPC to Server (Implementation depends on your proto)
    grpc::ClientContext context;  
    afs_operation::RenameRequest request;
    afs_operation::RenameResponse response;
    request.set_new_filename(to_name);
    request.set_filename(from_name);
    request.set_directory(resolved_path);
    request.set_new_directory(resolved_path_new);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->rename(&context, request, &response);

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
    std::string cache_path = std::string(cache_directory) + (resolved_path[0] == '/' ? "" : "/") + resolved_path
                           + (resolved_path.back() == '/' ? "" : "/") + filename;

    // Look up data server: check if file is open (use cached address), otherwise Lookup
    std::string ds_address;
    std::string ds_root;
    {
        std::lock_guard<std::mutex> g(cache_mutex);
        auto cache_it = cache.find(cache_path);
        if (cache_it != cache.end() && !cache_it->second.server_address.empty()) {
            ds_address = cache_it->second.server_address;
            ds_root = cache_it->second.server_root;
        }
    }
    if (ds_address.empty()) {
        std::string file_server_path = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
        grpc::ClientContext lookup_ctx;
        metadata::LookupRequest lookup_req;
        metadata::LookupResponse lookup_resp;
        lookup_req.set_path(file_server_path);
        grpc::Status lookup_status = metadata_stub_->Lookup(&lookup_ctx, lookup_req, &lookup_resp);
        if (!lookup_status.ok()) {
            std::cerr << "Lookup failed for truncate: " << lookup_status.error_message() << std::endl;
            return false;
        }
        ds_address = lookup_resp.data_server_address();
        ds_root = lookup_resp.data_server_root();
    }

    // 1. RPC-first: server is authoritative — use data-server-local path
    grpc::ClientContext context;
    afs_operation::Truncate_request request;
    afs_operation::Truncate_response response;
    request.set_directory(ds_root + resolved_path);
    request.set_filename(filename);
    request.set_size(size);
    request.set_client_id(client_id);

    grpc::Status status = get_data_stub(ds_address)->truncate(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server truncate failed: " << status.error_message() << std::endl;
        return false;
    }

    // 2. Best-effort local cache resize, serialized against concurrent writers
    std::shared_ptr<std::mutex> file_mtx;
    {
        std::lock_guard<std::mutex> global_lock(cache_mutex);
        auto m_it = file_mutexes.find(cache_path);
        if (m_it != file_mutexes.end()) file_mtx = m_it->second;
    }
    if (file_mtx) {
        std::lock_guard<std::mutex> file_lock(*file_mtx);
        std::error_code ec;
        std::filesystem::resize_file(cache_path, size, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            std::cerr << "Warning: failed to resize local cache " << cache_path << ": " << ec.message() << std::endl;
        }
    } else {
        std::error_code ec;
        std::filesystem::resize_file(cache_path, size, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            std::cerr << "Warning: failed to resize local cache " << cache_path << ": " << ec.message() << std::endl;
        }
    }

    // 3. Update cached_attr
    std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto attr_it = cached_attr.find(file_loca_server);
    if (attr_it != cached_attr.end()) {
        attr_it->second.size = static_cast<int64_t>(size);
        attr_it->second.mtime = response.timestamp();
        attr_it->second.ctime = response.timestamp();
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
    
    grpc::Status status = metadata_stub_->mkdir(&context, request, &response);
    if (!status.ok()){
        std::cout << "Directory Creation Failed: " << status.error_message() << std::endl;
        return false;
    }
    return true;
}

bool FileSystemClient::delete_file(const std::string& directory){
    std::string resolved_path = resolve_server_path(directory);
    std::string cache_path = std::string(cache_directory) + (resolved_path[0] == '/'? "" : "/" ) + resolved_path;
    if (cache_path.back() == '/') cache_path.pop_back();
    std::filesystem::path path(directory);
    
    std::unique_lock<std::mutex> global_lock(cache_mutex);
    // Check if file is currently being uploaded/closed by another thread
    if (file_mutexes.count(cache_path)) {
        std::shared_ptr<std::mutex> file_mtx = file_mutexes[cache_path];
        global_lock.unlock();             // Unlock global
        std::lock_guard<std::mutex> lock(*file_mtx); // Wait for the file to be free
        global_lock.lock();               // Re-lock global
    }
    // we need to clean the the three caches we have 
    auto it_op = opened_files.find(cache_path);
    if (it_op != opened_files.end()){
        // we find it in opened_files
        if(it_op->second.read_stream) it_op->second.read_stream->close();
        if(it_op->second.write_stream) it_op->second.write_stream->close();
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
    file_mutexes.erase(cache_path);
    global_lock.unlock();

    // And then we actually delete the files physically
    grpc::ClientContext context;
    afs_operation::Delete_request request;
    afs_operation::Delete_response response;
    request.set_directory(resolved_path);
    request.set_client_id(client_id);
    grpc::Status status = metadata_stub_->unlink(&context, request, &response);
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


// === delete_directory: rmdir RPC with POSIX-strict error semantics ===
int FileSystemClient::delete_directory(const std::string& directory){
    std::string resolved_path = resolve_server_path(directory);
    std::string cache_path = std::string(cache_directory) + (resolved_path[0] == '/' ? "" : "/") + resolved_path;
    if (cache_path.size() > 1 && cache_path.back() == '/') cache_path.pop_back();

    // Sweep cache, opened_files, cached_attr, file_mutexes for any keys at or under the directory prefix.
    auto sweep = [](auto& map, const std::string& prefix) {
        auto it = map.begin();
        while (it != map.end()) {
            const std::string& key = it->first;
            bool exact = (key == prefix);
            bool child = (key.size() > prefix.size()
                          && key.compare(0, prefix.size(), prefix) == 0
                          && key[prefix.size()] == '/');
            if (exact || child) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    };

    {
        std::lock_guard<std::mutex> global_lock(cache_mutex);
        sweep(cache, cache_path);
        sweep(opened_files, cache_path);
        sweep(file_mutexes, cache_path);
        sweep(cached_attr, resolved_path);
    }

    grpc::ClientContext context;
    afs_operation::Rmdir_request request;
    afs_operation::Rmdir_response response;
    request.set_directory(resolved_path);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->rmdir(&context, request, &response);
    if (!status.ok()) {
        switch (status.error_code()) {
            case grpc::StatusCode::FAILED_PRECONDITION: return -ENOTEMPTY;
            case grpc::StatusCode::NOT_FOUND:           return -ENOENT;
            case grpc::StatusCode::PERMISSION_DENIED:   return -EACCES;
            default: return -EIO;
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(cache_path, ec);
    return 0;
}


// === change_mode: chmod RPC ===
bool FileSystemClient::change_mode(const std::string& filename, const std::string& directory, uint32_t mode){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::Chmod_request request;
    afs_operation::Chmod_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);
    request.set_mode(mode);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->chmod(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server chmod failed: " << status.error_message() << std::endl;
        return false;
    }

    std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = cached_attr.find(file_loca_server);
    if (it != cached_attr.end()) {
        // preserve the upper file-type bits
        it->second.mode = (it->second.mode & ~0xFFF) | (mode & 0xFFF);
        it->second.ctime = response.timestamp();
    }
    return true;
}


// === change_owner: chown RPC ===
// NOTE: get_attributes() spoofs uid/gid to the local user (line 115), so client `ls -l`
// won't reflect server-side ownership even after a successful chown. The server is
// updated authoritatively, however.
bool FileSystemClient::change_owner(const std::string& filename, const std::string& directory, uint32_t uid, uint32_t gid){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::Chown_request request;
    afs_operation::Chown_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);
    request.set_uid(uid);
    request.set_gid(gid);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->chown(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server chown failed: " << status.error_message() << std::endl;
        return false;
    }

    std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = cached_attr.find(file_loca_server);
    if (it != cached_attr.end()) {
        // keep the spoofed uid/gid; only refresh ctime
        it->second.ctime = response.timestamp();
    }
    return true;
}


// === set_times: utimens RPC ===
bool FileSystemClient::set_times(const std::string& filename, const std::string& directory, int64_t atime_ns, int64_t mtime_ns){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::Utimens_request request;
    afs_operation::Utimens_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);
    request.set_atime(atime_ns);
    request.set_mtime(mtime_ns);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->utimens(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server utimens failed: " << status.error_message() << std::endl;
        return false;
    }

    std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = cached_attr.find(file_loca_server);
    if (it != cached_attr.end()) {
        it->second.atime = atime_ns;
        it->second.mtime = mtime_ns;
    }
    return true;
}


// === create_symlink ===
// Symlinks are NOT cached locally - the dereferencing semantics make caching dangerous.
bool FileSystemClient::create_symlink(const std::string& target, const std::string& linkpath_dir, const std::string& linkpath_name){
    std::string resolved_dir = resolve_server_path(linkpath_dir);

    grpc::ClientContext context;
    afs_operation::Symlink_request request;
    afs_operation::Symlink_response response;
    request.set_target(target);
    request.set_linkpath_dir(resolved_dir);
    request.set_linkpath_name(linkpath_name);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->symlink(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server symlink failed: " << status.error_message() << std::endl;
        return false;
    }
    return true;
}


// === read_symlink ===
// Always hits the server. Symlink targets are not cached.
bool FileSystemClient::read_symlink(const std::string& filename, const std::string& directory, std::string& out_target){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::Readlink_request request;
    afs_operation::Readlink_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);

    grpc::Status status = metadata_stub_->readlink(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server readlink failed: " << status.error_message() << std::endl;
        return false;
    }
    if (!response.success()) return false;
    out_target = response.target();
    return true;
}


// === fsync_file: streamed flush_file RPC, mirrors close_file but does not erase opened_files ===
bool FileSystemClient::fsync_file(const std::string& filename, const std::string& directory){
    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = std::string(cache_directory) + (resolved_path.front() == '/' ? "" : "/") + resolved_path
                              + (resolved_path.back() == '/' ? "" : "/") + filename;

    std::unique_lock<std::mutex> global_lock(cache_mutex);

    auto opened_file_it = opened_files.find(file_location);
    if (opened_file_it == opened_files.end()) {
        std::cerr << "Error: Cannot fsync '" << filename << "' because it is not open." << std::endl;
        return false;
    }

    auto m_it = file_mutexes.find(file_location);
    if (m_it == file_mutexes.end()) {
        std::cerr << "No mutex for file: " << file_location << " when fsync" << std::endl;
        return false;
    }
    std::shared_ptr<std::mutex> file_mtx = m_it->second;

    auto cache_it = cache.find(file_location);
    if (cache_it == cache.end()) {
        std::cerr << "Error: Inconsistent state. File is open but not in cache." << std::endl;
        return false;
    }

    // shared_ptr copy keeps the stream alive even if the entry is replaced/erased
    // between dropping global_lock and taking file_lock.
    std::shared_ptr<std::ofstream> write_stream = opened_file_it->second.write_stream;

    // Get the data server address and root for this file
    std::string ds_address;
    std::string ds_root;
    {
        auto c_it = cache.find(file_location);
        if (c_it != cache.end()) {
            ds_address = c_it->second.server_address;
            ds_root = c_it->second.server_root;
        }
    }

    global_lock.unlock();
    std::lock_guard<std::mutex> file_lock(*file_mtx);

    if (ds_address.empty()) {
        std::cerr << "Error: No data server address for file during fsync." << std::endl;
        return false;
    }

    write_stream->flush();
    if (write_stream->fail()) {
        std::cerr << "Error: Failed to flush write stream before fsync." << std::endl;
        return false;
    }

    // Re-open a fresh ifstream for upload (do not close the existing streams - file stays open)
    std::ifstream file_stream(file_location, std::ios::binary);
    if (!file_stream.is_open()) {
        std::cerr << "Error: Could not re-open file for fsync: " << file_location << std::endl;
        return false;
    }

    const std::size_t chunk_size = 4096;
    char buffer[chunk_size];
    afs_operation::Fsync_response response;
    int num_of_tries = 0;
    grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");

    auto* ds_stub = get_data_stub(ds_address);
    while (num_of_tries < 3 && !status.ok()) {
        grpc::ClientContext context;
        std::unique_ptr<grpc::ClientWriter<afs_operation::FileRequest>> writer(
            ds_stub->flush_file(&context, &response)
        );

        file_stream.clear();
        file_stream.seekg(0, std::ios::beg);

        bool sent_at_least_once = false;
        while (true) {
            file_stream.read(buffer, chunk_size);
            std::streamsize len = file_stream.gcount();
            if (len <= 0 && sent_at_least_once) break;

            afs_operation::FileRequest request;
            request.set_directory(ds_root + resolved_path);
            request.set_filename(filename);
            request.set_content(buffer, len);
            request.set_client_id(client_id);

            if (!writer->Write(request)) break;
            sent_at_least_once = true;

            if (len <= 0) break;
        }
        writer->WritesDone();
        status = writer->Finish();
        num_of_tries++;
    }

    file_stream.close();

    if (!status.ok()) {
        std::cerr << "RPC failed during fsync: " << status.error_message() << std::endl;
        return false;
    }

    global_lock.lock();
    auto fresh_cache_it = cache.find(file_location);
    if (fresh_cache_it != cache.end()) {
        fresh_cache_it->second.timestamp = response.timestamp();
        fresh_cache_it->second.locally_modified = false;
    }
    std::string file_loca_server = resolved_path + (resolved_path.back() == '/' ? "" : "/") + filename;
    auto attr_it = cached_attr.find(file_loca_server);
    if (attr_it != cached_attr.end()) {
        attr_it->second.mtime = response.timestamp();
        attr_it->second.ctime = response.timestamp();
        try {
            attr_it->second.size = std::filesystem::file_size(file_location);
        } catch (...) {}
    }
    // do NOT erase opened_files / file_mutexes - file stays open
    return true;
}


// === get_filesystem_stats: statfs RPC ===
bool FileSystemClient::get_filesystem_stats(struct statvfs& out){
    grpc::ClientContext context;
    afs_operation::Statfs_request request;
    afs_operation::Statfs_response response;

    grpc::Status status = metadata_stub_->statfs(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server statfs failed: " << status.error_message() << std::endl;
        return false;
    }
    std::memset(&out, 0, sizeof(out));
    out.f_bsize  = response.bsize();
    out.f_frsize = response.frsize();
    out.f_blocks = response.blocks();
    out.f_bfree  = response.bfree();
    out.f_bavail = response.bavail();
    out.f_files  = response.files();
    out.f_ffree  = response.ffree();
    return true;
}


// === xattr quartet ===
bool FileSystemClient::set_xattr(const std::string& filename, const std::string& directory, const std::string& name, const std::string& value, int flags){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::SetXattr_request request;
    afs_operation::SetXattr_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);
    request.set_name(name);
    request.set_value(value);
    request.set_flags(flags);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->setxattr(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server setxattr failed: " << status.error_message() << std::endl;
        return false;
    }
    return true;
}

int FileSystemClient::get_xattr(const std::string& filename, const std::string& directory, const std::string& name, std::string& out){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::GetXattr_request request;
    afs_operation::GetXattr_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);
    request.set_name(name);

    grpc::Status status = metadata_stub_->getxattr(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server getxattr failed: " << status.error_message() << std::endl;
        return -1;
    }
    if (!response.found()) return -1;
    out = response.value();
    return static_cast<int>(out.size());
}

bool FileSystemClient::list_xattr(const std::string& filename, const std::string& directory, std::vector<std::string>& out){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::ListXattr_request request;
    afs_operation::ListXattr_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);

    grpc::Status status = metadata_stub_->listxattr(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server listxattr failed: " << status.error_message() << std::endl;
        return false;
    }
    out.clear();
    for (const auto& n : response.names()) {
        out.push_back(n);
    }
    return true;
}

bool FileSystemClient::remove_xattr(const std::string& filename, const std::string& directory, const std::string& name){
    std::string resolved_path = resolve_server_path(directory);

    grpc::ClientContext context;
    afs_operation::RemoveXattr_request request;
    afs_operation::RemoveXattr_response response;
    request.set_directory(resolved_path);
    request.set_filename(filename);
    request.set_name(name);
    request.set_client_id(client_id);

    grpc::Status status = metadata_stub_->removexattr(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Server removexattr failed: " << status.error_message() << std::endl;
        return false;
    }
    return true;
}




