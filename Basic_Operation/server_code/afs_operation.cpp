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
    const std::string SERVER_INPUT_FILES_ROOT = "./tmp/input/";
    const std::string SERVER_OUTPUT_FILES_ROOT = "./tmp/output/";

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

    grpc::Status open(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter<afs_operation::FileResponse>* writer)override{
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
        // std::string content_of_file((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        // int size_of_file = content_of_file.size();

        // response->set_content(content_of_file);
        // response->set_length(size_of_file);
        const std::size_t chunk_size = 4096;
        char buffer[chunk_size];
        afs_operation::FileResponse fr;
        auto file_time = std::filesystem::last_write_time(path);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - std::filesystem::file_time_type::clock::now() + 
        std::chrono::system_clock::now());
        while(true){
            file.read(buffer, chunk_size);
            std::streamsize len = file.gcount();
            
            if(len <= 0)break;
            fr.clear_content();
            fr.set_content(buffer, len);
            fr.set_length(static_cast<int32_t>(len));
            fr.set_timestamp(sctp.time_since_epoch().count());

            if(!writer->Write(fr)){
                std::cerr << "Error: Failed write " << std::endl;
                return grpc::Status(grpc::StatusCode::INTERNAL, "Server failed to create the file.");
            }
        }
        // writer->WritesDone();
        std::cout << "File: " << filename << " successfully retrieved with size: " << std::endl;
        return grpc::Status::OK;
    }


    grpc::Status close(grpc::ServerContext* context, grpc::ServerReader<afs_operation::FileRequest>* reader, afs_operation::FileResponse* response) override {
        
        afs_operation::FileRequest request;
        std::string filename;
        std::string path;

        std::ofstream outfile;

        while(reader->Read(&request)){
            if(filename.empty() || path.empty()){
                filename = request.filename();
                if (request.path_select() == 0) {
                    path = SERVER_INPUT_FILES_ROOT + filename;
                }  else {
                    path = SERVER_OUTPUT_FILES_ROOT + filename;
                }
                outfile.open(path, std::ios::binary);
                if(!outfile.is_open()){
                    std::cerr << "failed to open file: " << path << std::endl;
                    return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                        "cant open file to write");
                }
            }
            outfile.write(request.content().data(), request.content().size());
        }

        if(outfile.is_open())outfile.close();
        auto file_time = std::filesystem::last_write_time(path);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        response->set_timestamp(sctp.time_since_epoch().count());
        return grpc::Status::OK;
        // const std::string& filename = request->filename();
        // std::cout << "Client requested to close file: " << filename << std::endl;
    
        // In simple file systems, close doesn't really need to do much on server side
        // since we open/close files per operation anyway. This is mostly for API completeness
        // Return the current timestamp if file exists
    
        
        // if (request->path_select() == 0) {
        //     path = SERVER_INPUT_FILES_ROOT + filename;
        // } else {
        //     path = SERVER_OUTPUT_FILES_ROOT + filename;
        // }
    
        // Check file existence and get timestamp
        // try {
        //     if (std::filesystem::exists(path)) {
        //         auto file_time = std::filesystem::last_write_time(path);
        //         auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        //         response->set_timestamp(sctp.time_since_epoch().count());
        //         std::cout << "File '" << filename << "' closed successfully." << std::endl;
        //     } 
        //     else {
        //         std::cout << "File '" << filename << "' does not exist, but close acknowledged." << std::endl;
        //         response->set_timestamp(0);
        //     }
        // } 
        // catch (const std::filesystem::filesystem_error& e) {
        //     std::cerr << "Warning: Error accessing file: " << e.what() << std::endl;
        //     response->set_timestamp(0);
        // }
    
        // return grpc::Status::OK;
    }


    // compares the timestamp of the client and server
    grpc::Status compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter< ::afs_operation::FileResponse>* writer) override {
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
                std::ifstream new_content(path,std::ios::binary);
                
            
                // sstream can help reading the entire contents of the file
                const std::size_t chunk_size = 4096;
                char buffer[chunk_size];
                while(true){
                    new_content.read(buffer, chunk_size);
                    std::streamsize len = new_content.gcount();
                    int64_t timestamp_server = sctp.time_since_epoch().count(); 
                    if(len <= 0)break;
                    afs_operation::FileResponse response;
                    response.set_content(buffer, len);
                    response.set_timestamp(timestamp_server);
                    response.set_update_bit(1);
                    if (!writer->Write(response)) {
                        std::cerr << "Failed to write chunk " << " to server" << std::endl;
                        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Failed to write chunk");
                    }
                }
                // std::stringstream buffer;
                // buffer << new_content.rdbuf();
                // std::string content = buffer.str();

                // response -> set_timestamp(timestamp_server);
                // response -> set_content(content);
                // response -> set_update_bit(1);
                // if(!status.ok()){
                //     std::cout<<"Failed to send file\n";
                //     return status;
                // }
                std::cout << "Cache for '" << filename << "' is stale. Sent update." << std::endl;
                return grpc::Status::OK;

            }else{
                afs_operation::FileResponse response;

                response.set_update_bit(0);
                response.set_timestamp(timestamp_server); // for consistency, still good to send back
                writer->Write(response);

                // writer->WritesDone();
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
