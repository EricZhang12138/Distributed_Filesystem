#include "afs_client.h"
#include <iostream>

AFSClient::AFSClient(const std::string& server_address) 
    : stub_(afs_operation::operators::NewStub(
        grpc::CreateChannel(
            server_address, 
            grpc::InsecureChannelCredentials()))
        ){
    
    // fetch directory info from server
    afs_operation::InitialiseRequest request;
    request.set_code_to_initialise("I want input/output directory");
    afs_operation::InitialiseResponse response;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->request_dir(&context, request, &response);
    if (status.ok()) {
        input_path_ = response.input_path();
        output_path_ = response.output_path();
        std::cout << "[AFSClient] Connected to server. Input: " << input_path_ 
                  << ", Output: " << output_path_ << std::endl;
    } else {
        std::cerr << "[AFSClient] Failed to initialize: " << status.error_message() << std::endl;
    }
}

bool AFSClient::read_file(const std::string& filename, std::string& content, int path_select){
    // create request
    afs_operation::FileRequest request;
    request.set_filename(filename);
    request.set_path_select(path_select);
    afs_operation::FileResponse response;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->read(&context, request, &response);
    
    if (status.ok()) {
        content = response.content();
        std::cout << "[AFSClient] Read file '" << filename << "' (" 
                  << response.length() << " bytes)" << std::endl;
        return true;
    } 
    else {
        std::cerr << "[AFSClient] Failed to read file '" << filename 
                  << "': " << status.error_message() << std::endl;
        return false;
    }
}

bool AFSClient::write_file(const std::string& filename, const std::string& content, int path_select) 
{
    // create file
    afs_operation::FileRequest create_request;
    create_request.set_filename(filename);
    create_request.set_path_select(path_select);
    create_request.set_content(content);
    
    afs_operation::FileResponse response;
    grpc::ClientContext context;
    grpc::Status status = stub_->create(&context, create_request, &response);
    
    if (status.ok()) {
        std::cout << "[AFSClient] Wrote file '" << filename << "' (" 
                  << content.length() << " bytes)" << std::endl;
        return true;
    } 
    else 
    {
        std::cerr << "[AFSClient] Failed to write file '" << filename 
                  << "': " << status.error_message() << std::endl;
        return false;
    }
}

bool AFSClient::append_file(const std::string& filename, const std::string& content, int path_select) {
    // using write RPC for append
    afs_operation::FileRequest request;
    request.set_filename(filename);
    request.set_path_select(path_select);
    request.set_content(content);
    
    afs_operation::FileResponse response;
    grpc::ClientContext context;
    
    grpc::Status status = stub_->write(&context, request, &response);
    
    if (status.ok()) 
    {
        std::cout << "[AFSClient] Appended to file '" << filename << "' (" 
                  << content.length() << " bytes)" << std::endl;
        return true;
    } else {
        std::cerr << "[AFSClient] Failed to append to file '" << filename 
                  << "': " << status.error_message() << std::endl;
        return false;
    }
}