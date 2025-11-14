#include "filesystem_client.hpp"
#include <iostream>
#include <sstream> 
#include <chrono>
#include <thread>
#include <filesystem>
#include <vector> 

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


bool FileSystemClient::open_file(std::string filename, std::string path){
    
    std::string resolved_path = resolve_server_path(path);
    std::cout << "DEBUG: Opening '" << filename << "' at resolved path: " << resolved_path << std::endl;

    std::string cache_dir = "./tmp/cache/" + resolved_path; // Use resolved_path
    std::string file_location = cache_dir + "/" + filename;
    
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

        auto read_stream = std::make_unique<std::ifstream>(file_location);
        auto write_stream = std::make_unique<std::ofstream>(file_location, std::ios::app);

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
            std::cerr<< "The file is already open, don't try to open it again please" << std::endl;
            return false;
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
        
        auto read_stream = std::make_unique<std::ifstream>(file_location);
        auto write_stream = std::make_unique<std::ofstream>(file_location, std::ios::app);

        if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
            std::cerr << "Failed to open the local file stream." << std::endl;
            return false;
        }
        
        opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
        std::cout << "File '" << filename << "' is now open for use." << std::endl;
        return true;
    }
}

std::optional<std::string> FileSystemClient::read_file_line(std::string& filename, std::string& directory){
    
    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = "./tmp/cache/" + resolved_path + "/" + filename;

    if (cache.find(file_location)==cache.end()){
        std::cerr << "File not in cache. Get the file from the server by calling open_file()" << std::endl;
        return std::nullopt;
    } else {
        if (opened_files.find(file_location) == opened_files.end()){
            std::cerr << "File found in cache but it is not opened. Open the file by calling open_file()" << std::endl;
            return std::nullopt;
        } else {
            auto it = opened_files.find(file_location);
            std::ifstream& file_stream = *(it->second.read_stream);
            std::string line;

            if (std::getline(file_stream, line)){
                return line;
            } else {
                return std::nullopt; // EOF or error
            }
        }
    }
}


bool FileSystemClient::write_file(std::string& filename, std::string& data, std::string& directory){

    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = "./tmp/cache/" + resolved_path + "/" + filename;

    if (cache.find(file_location)==cache.end()){
        std::cerr << "File not in cache. Get the file from the server by calling open_file()" << std::endl;
        return false;
    } else {
        if (opened_files.find(file_location) == opened_files.end()){
            std::cerr << "File found in cache but it is not opened. Open the file by calling open_file()" << std::endl;
            return false;
        } else {
            auto it = opened_files.find(file_location);
            std::ofstream& file_stream = *(it->second.write_stream);

            file_stream << data;

            if (file_stream.fail()) {
                std::cerr << "Error: Failed to write data to " << filename << std::endl;
                return false;
            }
            
            cache[file_location].is_changed = true;
            std::cout << "Successfully wrote to " << filename << " and marked as changed." << std::endl;
            return true;
        }
    } 
}


bool FileSystemClient::create_file(const std::string& filename, const std::string& path) {

    std::string resolved_path = resolve_server_path(path);
    std::string file_location = "./tmp/cache/" + resolved_path + "/" + filename;
    std::string cache_dir = "./tmp/cache/" + resolved_path;

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

    auto read_stream = std::make_unique<std::ifstream>(file_location);
    auto write_stream = std::make_unique<std::ofstream>(file_location, std::ios::app);
    
    if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
        std::cerr << "Failed to open newly created file stream for: " << file_location<< std::endl;
        cache.erase(file_location); 
        return false;
    }

    opened_files[file_location] = FileStreams{std::move(read_stream), std::move(write_stream)};
    std::cout << "Successfully created and opened '" << filename << "' for writing." << std::endl;
    return true;
}


bool FileSystemClient::close_file(const std::string& filename, const std::string& directory) {
    std::string resolved_path = resolve_server_path(directory);
    std::string file_location = "./tmp/cache/" + resolved_path + "/" + filename;
    
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
            auto read_s = std::make_unique<std::ifstream>(file_location);
            auto write_s = std::make_unique<std::ofstream>(file_location, std::ios::app);
            opened_files[file_location] = FileStreams{std::move(read_s), std::move(write_s)};
            std::cerr << "File handles have been re-opened. Please try closing again." << std::endl;
            return false; 
        }

        cache_it->second.is_changed = false;
        cache_it->second.timestamp = response.timestamp();
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



int main() {
    // 1. Setup connection
    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        address,
        grpc::InsecureChannelCredentials()
    );

    FileSystemClient client(channel);
    std::cout << "Client connected to " << address << std::endl;

    // --- Part 1: Create, Write, and Close file.txt in folder1 ---
    std::cout << "\n--- Scenario 1: Create, Write, Close ---" << std::endl;
    
    std::string file1_name = "file.txt";
    std::string file1_path = "folder1"; // Path is relative to server root
    
    std::cout << "Attempting to create: " << file1_path << "/" << file1_name << std::endl;
    if (client.create_file(file1_name, file1_path)) {
        std::cout << "Create success." << std::endl;
        
        // Write to it
        std::cout << "Attempting to write 'I love you'..." << std::endl;
        std::string data = "I love you\n";
        if (client.write_file(file1_name, data, file1_path)) {
            std::cout << "Write success." << std::endl;
        } else {
            std::cerr << "Failed to write to file." << std::endl;
        }
        
        // Close it
        std::cout << "Attempting to close file..." << std::endl;
        if (client.close_file(file1_name, file1_path)) {
            std::cout << "Close success. File flushed to server." << std::endl;
        } else {
            std::cerr << "Failed to close file." << std::endl;
        }

    } else {
        std::cerr << "Failed to create file (it might already exist?): " << file1_name << std::endl;
    }

    // --- Part 2: Open test.txt in /ABC ---
    std::cout << "\n--- Scenario 2: Open File ---" << std::endl;
    
    std::string file2_name = "test.txt";
    std::string file2_path = "ABC"; // Note: We send "ABC", not "/ABC"
    
    std::cout << "Attempting to open: " << file2_path << "/" << file2_name << std::endl;
    
    if (client.open_file(file2_name, file2_path)) {
        std::cout << "File 'test.txt' opened successfully." << std::endl;
        // Good practice to close it when done
        client.close_file(file2_name, file2_path);
    } else {
        std::cerr << "Failed to open 'test.txt'. (This is normal if the file doesn't exist on the server)." << std::endl;
    }

    std::cout << "\n--- Scenario 2: directory listing ---" << std::endl;
    client.ls_contents("");

    std::cout << "\n--- Script finished ---" << std::endl;

    return 0;
}