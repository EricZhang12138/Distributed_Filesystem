#include "filesystem_server.hpp" 
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono> // For timestamp logic

grpc::Status FileSystem::request_dir(grpc::ServerContext* context, const afs_operation::InitialiseRequest* request, afs_operation::InitialiseResponse* response) {
    if (request->code_to_initialise() == "I want input/output directory"){
        std::cout << "Received client request(later I should add the name of the client)" << std::endl;
        response->set_root_path("/");
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
    std::cout << "Client wants " << directory<<"/"<<filename << std::endl;
    std::string path = directory + "/" + filename;

    // have to read the file in binary mode to avoid line ending translation
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()){
        std::cerr << "file: " << path << " not found" << std::endl;
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "File not found on the server.");
    }

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
            path = request.directory() + "/" + filename;
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

    auto file_time = std::filesystem::last_write_time(path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    response->set_timestamp(sctp.time_since_epoch().count());
    return grpc::Status::OK;
}


grpc::Status FileSystem::compare(grpc::ServerContext* context, const afs_operation::FileRequest* request, grpc::ServerWriter< ::afs_operation::FileResponse>* writer) {
    std::string filename = request->filename();
    int64_t timestamp = request -> timestamp();
    std::string path = request->directory() + "/" + request->filename();
            
    try {
        auto file_time = std::filesystem::last_write_time(path); // gets the file's modification time
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        int64_t timestamp_server = sctp.time_since_epoch().count(); //Unix timestamp
        
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


void FileSystem::RunServer(){
    std::string server_address = "0.0.0.0:50051";

    grpc::ServerBuilder builder;
    
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);
    
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    
    server->Wait();
}


// --- Main Application Entry Point ---

int main(){
    FileSystem filesys;
    filesys.RunServer();

    std::cout << "Server Stopped" << std::endl;
    return 0;
}