#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem> 

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"

struct FileInfo{
    std::string filename;
    std::string server_path;
    int64_t timestamp;
};

class FileSystem final : public afs_operation::operators::Service{

    // a constant for the directory where the files are stored on the server
    const std::string SERVER_INPUT_FILES_ROOT = "/tmp/input/";
    const std::string SERVER_OUTPUT_FILES_ROOT = "/tmp/output/";

    // when client connects, it request directory for input/output paths 
    grpc::Status request_dir(grpc::ServerContext* context, const afs_operation::InitialiseRequest* request, afs_operation::InitialiseResponse* response)override{
        if (request->code_to_initialise() == "I want input/output directory"){
            response->set_input_path(SERVER_INPUT_FILES_ROOT);
            response->set_output_path(SERVER_OUTPUT_FILES_ROOT);
            std::cout << "input/output directory successfully obtained" << std::endl;
            return grpc::Status::OK;
        }else{
            std::cerr << "There is an error while passing the input/output files directory"<<std::endl;
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "You need the correct code to retrieve requested data.");
        }
    }

    grpc::Status open(grpc::ServerContext* context, const afs_operation::FileRequest* request, afs_operation::FileResponse* response)override{
        //get the file name
        std::string filename = request -> filename(); // gRPC generates getter methods
        std::cout << "Client wants " << filename << std::endl;

        //construct the full path
        std::string path;
        if (request->path_select() == 0){
            path = SERVER_INPUT_FILES_ROOT + filename;
        }else{
            path = SERVER_OUTPUT_FILES_ROOT + filename;
        }
        // have to read the file in binary mode to avoid line ending translation
        // linux uses /n but windows uses /r/n. Without binary mode the file content will be 
        // automatically changed if opened on different machines
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()){
            std::cerr << "file: " << path << " not found" << std::endl;
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found on the server.");
        }

        //parse the entire file into a string
        std::string content_of_file((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        int size_of_file = content_of_file.size();

        response->set_content(content_of_file);
        response->set_length(size_of_file);
        std::cout << "File: " << filename << " successfully retrieved with size: " << size_of_file << std::endl;
        return grpc::Status::OK;
    }



    grpc::Status create(grpc::ServerContext* context, const afs_operation::FileRequest* request, afs_operation::FileResponse* response) override {
        const std::string& filename = request->filename();
        const std::string& content = request->content();

        // only output files are created
        std::string path = SERVER_OUTPUT_FILES_ROOT + filename;

        std::cout << "Client requested to create file: " << path << std::endl;

        // Open the file for writing. Will create the file if its not there
        std::ofstream outfile(path, std::ios::binary);
        if (!outfile.is_open()) {
            std::cerr << "Error: Failed to create or open file at " << path << std::endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, "Server failed to create the file.");
        }

        // Write the content received from the client into the file.
        outfile << content;
        if (outfile.fail()) {
            std::cerr << "Error: Failed to write content to " << path << std::endl;
            outfile.close();
            return grpc::Status(grpc::StatusCode::INTERNAL, "Server failed to write content to the file.");
        }

        outfile.close();

        // Get the timestamp of the new file.
        try {
            auto file_time = std::filesystem::last_write_time(path); // gets the file's modification time
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            response->set_timestamp(sctp.time_since_epoch().count()); //Unix timestamp
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Warning: Could not get timestamp for new file: " << e.what() << std::endl;
            response->set_timestamp(0);
        }
        
        // Set the response and return success.
        response->set_length(content.length());
        std::cout << "Successfully created file '" << filename << "' with size " << content.length() << std::endl;
        
        return grpc::Status::OK;
    }

    grpc::Status compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, afs_operation::FileResponse* response) override {
        std::string filename = request->filename();
        int64_t timestamp = request -> timestamp();
        // since only output files can be created -> only output files have timestamps -> we only have to implement the timestamp compare logic for output files

        // Determine path based on path_select
        std::string path;
        if (request->path_select() == 0) {
            path = SERVER_INPUT_FILES_ROOT + filename;
        } else {
            path = SERVER_OUTPUT_FILES_ROOT + filename;
        }

                // Get the timestamp of the file.
        try {
            auto file_time = std::filesystem::last_write_time(path); // gets the file's modification time
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            int64_t timestamp_server = sctp.time_since_epoch().count(); //Unix timestamp
            // compare
            if (timestamp_server > timestamp){
                // this means the client version was an old version
                // then we get the content of the file
                std::fstream new_content(path);
                
            
                // sstream can help reading the entire contents of the file
                std::stringstream buffer;
                buffer << new_content.rdbuf();
                std::string content = buffer.str();

                response -> set_timestamp(timestamp_server);
                response -> set_content(content);
                response -> set_update_bit(1);
                std::cout << "Cache for '" << filename << "' is stale. Sent update." << std::endl;
                return grpc::Status::OK;

            }else{
                response -> set_update_bit(0);
                response->set_timestamp(timestamp_server); // for consistency, still good to send back
                std::cout << "Cache for '" << filename << "' is valid." << std::endl;
                return grpc::Status::OK;
            }
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Warning: Could not get timestamp for the file: " << e.what() << std::endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, "Server failed to get the timestamp of the file on the server.");
        }

        
        





    }




public:
    void RunServer(){
        std::string server_address = "0.0.0.0:50051";

        grpc::ServerBuilder builder;
        
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(this);
        
        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        std::cout << "Server listening on " << server_address << std::endl;
        
        server->Wait();
    }




};

int main(){
    FileSystem filesys;
    filesys.RunServer();

    std::cout << "Server Stopped" << std::endl;
    return 0;
}