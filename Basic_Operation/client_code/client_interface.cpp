#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <sstream> 


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

    std::unique_ptr<afs_operation::operators::Stub> stub_;
    std::map<std::string,FileInfo> cache;
    std::map<std::string,std::unique_ptr<std::fstream>> opened_files;
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

            afs_operation::FileResponse response_temp;

            grpc::ClientContext context;

            grpc::Status status = stub_ -> open(&context, request, &response_temp);

            // check if call succeeded
            if (status.ok()) {
                std::cout << "File retrieved successfully!" << std::endl;

                // we initialise FileInfo with is_changed = false, response_time = timestamp recorded on the server
                struct FileInfo file_info{false, response_temp.timestamp(), filename};
                // add this file / FileInfo pair to the map
                cache[filename] = file_info;
                // create the file
                std::string file_path = "/tmp/cache/"+filename;

                std::ofstream outfile(file_path);
                if (!outfile.is_open()){
                    std::cerr << "Failed to create file at " << "file_path" << std::endl;
                    cache.erase(filename);
                    return false ;
                }else{
                    std::cout << "File cached successfully" << std::endl;
                }
                // put the content in the file 
                outfile << response_temp.content();
                if (outfile.fail()){
                    std::cerr << "Can not write data to the local cache" << std::endl;
                    cache.erase(filename);
                    return false;
                }
                outfile.close();

                auto file_descriptor_ptr = std::make_unique<std::fstream>(file_path);
                opened_files[filename] = std::move(file_descriptor_ptr);
                return true;
                
            } else {
                std::cerr << "RPC failed: " << status.error_code() 
                        << ": " << status.error_message() << std::endl;
                return false;
            }

        }else{
            // open locally
            // However when we open locally, we need to check from the server whether our version is the latest version
            if (opened_files.find(filename)!=opened_files.end()){
                std::cerr<< "The file is already open, don't try to open it again please" << std::endl;
                return false;
            }else{

            std::string file_path = "/tmp/cache/"+filename;
            int64_t timestamp = cache[filename].timestamp;
            afs_operation::FileRequest request;
            request.set_filename(filename);
            request.set_timestamp(timestamp);
            request.set_path_select(path); // also send path_select for the server

            afs_operation::FileResponse response;
            grpc::ClientContext context;
            // get the response to check whether we need to update
            grpc::Status status = stub_ -> compare(&context, request, &response);

            if (!status.ok()) {
                std::cerr << "RPC failed during compare: " << status.error_message() << std::endl;
                return false;
            }

            if (response.update_bit() == 1){
                std::cout << "Cache is stale. Overwriting local file..." << std::endl;
                int64_t new_timestamp = response.timestamp();
                std::string new_content = response.content();
                // overwrite the new content to the local file
                std::ofstream outfile(file_path);
                if (!outfile.is_open()) {
                    std::cerr << "Error: Could not open the file at " << file_path << std::endl;
                    return false;
                }
                outfile << new_content;
                
                outfile.close();

                //update the FileInfo object
                cache[filename].timestamp = response.timestamp();
                cache[filename].is_changed = false;
                std::cout << "Successfully overwrote file: " << file_path << std::endl;
            }else{
                std::cout << "File: "<< file_path << " is valid and is now opened." << std::endl;
            }
            // open the local file which is guaranteed to be up-to-date
            auto file_descriptor_ptr = std::make_unique<std::fstream>(file_path);
            if (!file_descriptor_ptr || !file_descriptor_ptr->is_open()) {
                std::cerr << "Failed to open the local file stream." << std::endl;
                return false;
            }
            opened_files[filename] = std::move(file_descriptor_ptr);
            std::cout << "File '" << filename << "' is now open for use." << std::endl;
             return true;
            }
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
                std::fstream& file_stream = *(it->second);
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
                std::fstream& file_stream = *(it->second);

                // Save the current position of the READ cursor.
                std::streampos original_read_pos = file_stream.tellg();

                // Position the cursor at the end of the file to append data
                file_stream.seekp(0, std::ios::end);
                file_stream << data;

                // Restore the READ cursor to its original position.
                file_stream.seekg(original_read_pos);

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
        std::string file_path = "/tmp/cache/" + filename;
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

        // Open the newly created local file with fstream for reading/writing
        // and add its stream pointer to the map of opened files.
        auto file_stream_ptr = std::make_unique<std::fstream>(file_path, std::ios::in | std::ios::out);
        if (!file_stream_ptr || !file_stream_ptr->is_open()) {
            std::cerr << "Failed to open newly created file stream for: " << file_path << std::endl;
            cache.erase(filename); // Clean up metadata on failure.
            return false;
        }

        opened_files[filename] = std::move(file_stream_ptr);

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

            // Read the entire content of the local cached file.
            std::fstream& file_stream = *(opened_file_it->second);
            file_stream.seekg(0, std::ios::beg); // Rewind to the start before reading
            
            // sstream can help reading the entire contents of the file
            std::stringstream buffer;
            buffer << file_stream.rdbuf();
            std::string content = buffer.str();

            // Prepare and send the create RPC to the server.
            afs_operation::FileRequest request;
            request.set_filename(filename);
            request.set_content(content);

            afs_operation::FileResponse response;
            grpc::ClientContext context;
            grpc::Status status = stub_->create(&context, request, &response);

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


int main(){
    std::string address = "localhost:50051";
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
        address,
        grpc::InsecureChannelCredentials()
    );

    FileSystemClient client(channel);
    std::string filename = "test1.txt";
    if (client.open_file(filename,1) == false){
        std::cout << "wtf" <<std::endl;
    }
    auto str = client.read_file_line(filename);
    if (str != std::nullopt){
    std::cout << "the read line is " << *str << std::endl;
    }
    std::string data = "bad boy";
    client.write_file(filename, data);

    client.close_file(filename);



}