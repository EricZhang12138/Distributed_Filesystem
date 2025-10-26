#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <sstream> 
#include <chrono>
#include <thread>


#include <grpcpp/grpcpp.h>
#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"


class FileSystemClient{

private:
    struct FileInfo{
        bool is_changed;
        int64_t timestamp;
        std::string filename;
    };

    struct FileStreams {
        std::unique_ptr<std::ifstream> read_stream;
        std::unique_ptr<std::ofstream> write_stream;
    };


    std::unique_ptr<afs_operation::operators::Stub> stub_;
    std::map<std::string,FileInfo> cache;
    std::map<std::string,FileStreams> opened_files;
    std::string output_file_path;
    std::string input_file_path;

public:
    FileSystemClient(std::shared_ptr<grpc::Channel> channel) : stub_(afs_operation::operators::NewStub(channel)){
        afs_operation::InitialiseRequest request;
        request.set_code_to_initialise("I want input/output directory");
        afs_operation::InitialiseResponse response;
        grpc::ClientContext context;
        grpc::Status status = stub_ -> request_dir(&context, request, &response);
        if (!status.ok()){
            std::cerr << "Unable to retrieve the path for input/output files on the server" << std::endl;
        }else{
            //Now we successfully retrieved the data
            output_file_path = response.output_path();
            input_file_path = response.input_path();
        }
    }

    // if path == 0, we go to input files to retrieve data. If path == 1, we go to output files to retrieve data
    bool open_file(std::string filename, int path){

        // the file is not in the local cache
        if (cache.find(filename) == cache.end()){
            afs_operation::FileRequest request;
            request.set_filename(filename);
            request.set_path_select(path);

            
            std::string file_path = "./tmp/cache/"+filename;

            afs_operation::FileResponse response_temp;

            int num_of_retries = 0;
            grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");
            
            while(num_of_retries < 3 && !status.ok()){
                grpc::ClientContext context;
                std::unique_ptr<grpc::ClientReader<afs_operation::FileResponse>> reader(stub_->open(&context, request));

                std::ofstream outfile(file_path, std::ios::binary);
                if (!outfile.is_open()){
                    std::cerr << "Failed to create file at " << "file_path" << std::endl;
                    return false ;
                }else{
                    std::cout << "File cached successfully" << std::endl;
                }

                while(reader->Read(&response_temp)){
                    struct FileInfo file_info{false, response_temp.timestamp(), filename};
                    cache[filename] = file_info;
                    if(response_temp.length() > 0)
                        outfile<<response_temp.content();
                    if (outfile.fail()){
                        std::cerr << "Can not write data to the local cache" << std::endl;
                        cache.erase(filename);
                        outfile.close();
                        return false;
                    }

                }
                outfile.close();
                status = reader->Finish();
                num_of_retries ++;
            }
            if (!status.ok()){
                std::cerr << "After 3 tries, Failed to Open the file from the server" << std::endl;
            }

            auto read_stream = std::make_unique<std::ifstream>(file_path);
            auto write_stream = std::make_unique<std::ofstream>(file_path, std::ios::app);

            if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
                std::cerr << "Failed to open local file streams after download." << std::endl;
                cache.erase(filename);
                return false;
            }
            opened_files[filename] = FileStreams{std::move(read_stream), std::move(write_stream)};
            
            
        
            if (status.ok()) {
                // std::cout << "File retrieved successfully!" << std::endl;

                // // we initialise FileInfo with is_changed = false, response_time = timestamp recorded on the server
                // struct FileInfo file_info{false, response_temp.timestamp(), filename};
                // // add this file / FileInfo pair to the map
                // cache[filename] = file_info;
                // // create the file
                // std::string file_path = "/tmp/cache/"+filename;

                // std::ofstream outfile(file_path);
                // if (!outfile.is_open()){
                //     std::cerr << "Failed to create file at " << "file_path" << std::endl;
                //     cache.erase(filename);
                //     return false ;
                // }else{
                //     std::cout << "File cached successfully" << std::endl;
                // }
                // // put the content in the file 
                // outfile << response_temp.content();
                // if (outfile.fail()){
                //     std::cerr << "Can not write data to the local cache" << std::endl;
                //     cache.erase(filename);
                //     return false;
                // }
                // outfile.close();

                // auto file_descriptor_ptr = std::make_unique<std::fstream>(file_path);
                // opened_files[filename] = std::move(file_descriptor_ptr);
                return true;
                
            } else {
                std::cerr << "RPC failed: " << status.error_code() 
                        << ": " << status.error_message() << std::endl;
                return false;
            }

        }else{
            // update locally
            // Check if file is already open by this client
            if (opened_files.find(filename)!=opened_files.end()){
                std::cerr<< "The file is already open, don't try to open it again please" << std::endl;
                return false;
            }

            // Prepare for RPC request
            std::string file_path = "./tmp/cache/"+filename;
            int64_t timestamp = cache[filename].timestamp;
            
            afs_operation::FileRequest request;
            request.set_filename(filename);
            request.set_timestamp(timestamp);
            request.set_path_select(path); 

            // Prepare for the retry loop
            int num_of_retries = 0;
            grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");
            
            // stringstream as an in-memory buffer.
            std::stringstream content_buffer(std::ios::binary | std::ios::in | std::ios::out);
            int update_bit = 0;
            int64_t new_timestamp = 0;
            
            while(num_of_retries < 3 && !status.ok()){
                
                grpc::ClientContext context;
                afs_operation::FileResponse response_chunk;

                //clear buffers from any previous failed attempt
                content_buffer.str(""); // Clear the stringstream's content
                content_buffer.clear(); // Clear its error flags
                update_bit = 0;
                new_timestamp = 0;
                
                std::unique_ptr<grpc::ClientReader<afs_operation::FileResponse>> reader(stub_->compare(&context, request));

                // Read all chunks into the temporary buffer
                while(reader->Read(&response_chunk)){
                    // server sends the same data on every chunk
                    update_bit = response_chunk.update_bit();
                    new_timestamp = response_chunk.timestamp();
                    
                    if(update_bit == 1) { 
                        // Only save the content if the server says we need to update
                        content_buffer << response_chunk.content();
                    }
                }
                
                status = reader->Finish();
                num_of_retries++;

            }

            if (!status.ok()) {
                std::cerr << "RPC failed during compare after 3 attempts: " << status.error_message() << std::endl;
                return false; 
            }

            // RPC was successful. Now, update local cache and file.
            if (new_timestamp != 0) {
                cache[filename].timestamp = new_timestamp;
                cache[filename].is_changed = false;
            }

            if (update_bit == 1) {
                // we can safely open the local file and overwrite it.
                std::cout << "Cache is stale. Overwriting local file..." << std::endl;
                std::ofstream outfile(file_path, std::ios::binary | std::ios::trunc);
                
                if (!outfile.is_open()) {
                    std::cerr << "Error: Could not open the file at " << file_path << " to overwrite." << std::endl;
                    return false;
                }
                outfile << content_buffer.rdbuf(); 
                if (outfile.fail()) {
                    std::cerr << "Error: Failed to write new content to local file." << std::endl;
                    outfile.close();
                    return false;
                }
                outfile.close();
                std::cout << "Successfully overwrote file: " << file_path << std::endl;
            
            } else {
                // We did not delete it, so we do nothing.
                std::cout << "File: "<< file_path << " is valid and is now opened." << std::endl;
            }
            
            // Add the Filestreams
            auto read_stream = std::make_unique<std::ifstream>(file_path);
            auto write_stream = std::make_unique<std::ofstream>(file_path, std::ios::app);

            if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
                std::cerr << "Failed to open the local file stream." << std::endl;
                return false;
            }
            
            opened_files[filename] = FileStreams{std::move(read_stream), std::move(write_stream)};
            std::cout << "File '" << filename << "' is now open for use." << std::endl;
            return true;
        }
    }

    std::optional<std::string> read_file_line(std::string& filename){
        if (cache.find(filename)==cache.end()){
            std::cerr << "File not in cache. Get the file from the server by calling file_open()" << std::endl;
            return std::nullopt;
        }else{
            if (opened_files.find(filename) == opened_files.end()){
                std::cerr << "File found in cache but it is not opened. Open the file by calling file_open()" << std::endl;
                return std::nullopt;
            }else{
                // now we can read the file
                auto it = opened_files.find(filename);
                std::ifstream& file_stream = *(it->second.read_stream);
                std::string line;

                if (std::getline(file_stream, line)){
                    return line;
                }else{
                    // if getline fails such as reaching the end of file
                    return std::nullopt;
                }

            }
        }
    }


    bool write_file(std::string& filename, std::string& data){
        if (cache.find(filename)==cache.end()){
            std::cerr << "File not in cache. Get the file from the server by calling file_open()" << std::endl;
            return false;
        }else{
            if (opened_files.find(filename) == opened_files.end()){
                std::cerr << "File found in cache but it is not opened. Open the file by calling file_open()" << std::endl;
                return false;
            }else{
                // now we write to the file
                auto it = opened_files.find(filename);
                std::ofstream& file_stream = *(it->second.write_stream);

                file_stream << data;

                if (file_stream.fail()) {
                    std::cerr << "Error: Failed to write data to " << filename << std::endl;
                    return false;
                }
                
                // mark it as changed
                cache[filename].is_changed = true;
                std::cout << "Successfully wrote to " << filename << " and marked as changed." << std::endl;
                return true;
            }
        } 
       
    }


    bool create_file(const std::string& filename) {
        // Check if the file already exists in the cache or is already open.
        // You cannot create a file that the client already knows about.
        if (cache.count(filename) || opened_files.count(filename)) {
            std::cerr << "Error: File '" << filename << "' already exists." << std::endl;
            return false;
        }

        // Create an empty file in the local cache directory.
        std::string file_path = "./tmp/cache/" + filename;
        std::ofstream outfile(file_path); // Creates the file on construction.
        if (!outfile.is_open()) {
            std::cerr << "Error: Failed to create local file at " << file_path << std::endl;
            return false;
        }
        outfile.close(); // Immediately close the ofstream handle.

        // Add metadata to the cache.
        // Timestamp is 0, as it doesn't have a server-side version yet.
        struct FileInfo file_info{true, 0, filename};
        cache[filename] = file_info;

        auto read_stream = std::make_unique<std::ifstream>(file_path);
        auto write_stream = std::make_unique<std::ofstream>(file_path, std::ios::app);
        
        if (!read_stream || !read_stream->is_open() || !write_stream || !write_stream->is_open()) {
            std::cerr << "Failed to open newly created file stream for: " << file_path << std::endl;
            cache.erase(filename); // Clean up metadata on failure.
            return false;
        }

        opened_files[filename] = FileStreams{std::move(read_stream), std::move(write_stream)};
        std::cout << "Successfully created and opened '" << filename << "' for writing." << std::endl;
        return true;
    }


    bool close_file(const std::string& filename) {
        // Check file actually open
        auto opened_file_it = opened_files.find(filename);
        if (opened_file_it == opened_files.end()) {
            std::cerr << "Error: Cannot close '" << filename << "' because it is not open." << std::endl;
            return false;
        }

        // Check the cache metadata to see if the file has changed.
        auto cache_it = cache.find(filename);
        if (cache_it == cache.end()) {
            // Ideally not going to happen if a file is open.
            std::cerr << "Error: Inconsistent state. File is open but not in cache." << std::endl;
            opened_files.erase(opened_file_it); // Clean up the open file entry anyway.
            return false;
        }

        // If the file was changed, flush it to the server.
        if (cache_it->second.is_changed) {
            std::cout << "File '" << filename << "' was modified. Flushing to server..." << std::endl;

            // Flush the write stream to ensure all data is on disk
            std::ofstream& write_stream = *(opened_file_it->second.write_stream);
            write_stream.flush();
            if(write_stream.fail()) {
                std::cerr << "Error: Failed to flush write stream before closing." << std::endl;
                return false;
            }

            // Read the entire content of the local cached file.
            // std::fstream& file_stream = *(opened_file_it->second);
            std::ifstream& file_stream = *(opened_file_it->second.read_stream);
            file_stream.clear(); // Clear any error flags (like EOF)
            file_stream.seekg(0, std::ios::beg); // Rewind to the start before reading
            
            // sstream can help reading the entire contents of the file
            // std::stringstream buffer;
            // buffer << file_stream.rdbuf();
            // std::string content = buffer.str();
            const std::size_t chunk_size = 4096;
            char buffer[chunk_size];


            afs_operation::FileResponse response; // response can be reused 
            int num_of_tries = 0;
            grpc::Status status(grpc::StatusCode::UNKNOWN, "Initial state for retry loop");
            while (num_of_tries < 3 && !status.ok()){
                grpc::ClientContext context;     // context can't be reused, so each try needs new context 
                std::unique_ptr<grpc::ClientWriter<afs_operation::FileRequest>> writer(   // writer can't be reused 
                    stub_->close(&context, &response)
                );
                file_stream.clear(); // Clear EOF flag
                file_stream.seekg(0, std::ios::beg); // Rewind to start
                while(true){
                    file_stream.read(buffer, chunk_size);
                    std::streamsize len = file_stream.gcount();

                    if(len<=0)break;
                    afs_operation::FileRequest request;
                    request.set_path_select(1);
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

            if (!status.ok()) {
                std::cerr << "RPC failed while flushing file to server: " << status.error_message() << std::endl;
                // NOTE: We do not close the file locally, so the client can try again.
                return false; 
            }

            // If succeeds, update local cache metadata.
            cache_it->second.is_changed = false;
            cache_it->second.timestamp = response.timestamp();
            std::cout << "File flushed successfully." << std::endl;
        } else {
            std::cout << "File '" << filename << "' was not modified. No flush needed." << std::endl;
        }

        // Remove the file from the map of opened files.
        // The unique_ptr's destructor will automatically close the fstream.
        opened_files.erase(opened_file_it);
        
        std::cout << "File '" << filename << "' is now closed." << std::endl;
        return true;
    }



};


void opt_test(std::string filename){
    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        address,
        grpc::InsecureChannelCredentials()
    );
    FileSystemClient client(channel);

    client.open_file(filename,1);

    std::string data = "Haha, I am last to close";

    client.write_file(filename,data);
    client.close_file(filename);

}




int main(int argc, char *argv[]){
    if(argc < 2){
        std::cerr<<"Usage: binFile <File To Op>";
        exit(1);
    }

    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        address,
        grpc::InsecureChannelCredentials()
    );

    FileSystemClient client(channel);
    std::string filename = argv[1];
    if (client.open_file(filename,1) == false){
        std::cout << "gg" <<std::endl;
    }

    std::string data = "Hello again";
    client.write_file(filename, data);
    
    client.close_file(filename);
    std::thread thread(opt_test,"test1.txt");
    thread.detach();

    std::this_thread::sleep_for(std::chrono::seconds(5));

    client.open_file(filename,1);

    return 0;

}
