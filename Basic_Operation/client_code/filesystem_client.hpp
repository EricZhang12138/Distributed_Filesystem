#pragma once

#include <string>
#include <map>
#include <memory>       // For std::unique_ptr, std::shared_ptr
#include <fstream>     
#include <optional>    
#include <grpcpp/grpcpp.h>
#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"

class FileSystemClient {

private:
    /**
     * @brief Holds local cache metadata for a file.
     */
    struct FileInfo {
        bool is_changed;      // True if the local copy has been modified
        int64_t timestamp;    // The last known timestamp from the server
        std::string filename; // The base name of the file
    };

    /**
     * @brief Holds the file stream handles for an open file.
     */
    struct FileStreams {
        std::unique_ptr<std::ifstream> read_stream;
        std::unique_ptr<std::ofstream> write_stream;
    };

    // The gRPC stub for communicating with the server
    std::unique_ptr<afs_operation::operators::Stub> stub_;
    
    // In-memory cache of file metadata
    std::map<std::string, FileInfo> cache;
    
    // Map of locally open file handles
    std::map<std::string, FileStreams> opened_files;

    std::string server_root_path_;

    std::string resolve_server_path(const std::string& user_path);

public:
    /**
     * @brief Constructs the client and initializes the connection with the server.
     * @param channel The gRPC channel to use for communication.
     */
    FileSystemClient(std::shared_ptr<grpc::Channel> channel);

    /**
     * @brief Opens a file, downloading it from the server if not cached or
     * validating the cache if it is.
     * @param filename The name of the file to open (e.g., "test.txt").
     * @param path The server-side directory path (e.g., "data/inputs").
     * @return true if the file was successfully opened, false otherwise.
     */
    bool open_file(std::string filename, std::string path);

    /**
     * @brief Reads a single line from a previously opened file.
     * @param filename The name of the file.
     * @param directory The server-side path.
     * @return A string containing the line, or std::nullopt if at EOF or on error.
     */
    std::optional<std::string> read_file_line(std::string& filename, std::string& directory);

    /**
     * @brief Writes data to a previously opened file's local cache.
     * @param filename The name of the file.
     * @param data The string data to write.
     * @param directory The server-side path.
     * @return true on success, false on failure.
     */
    bool write_file(std::string& filename, std::string& data, std::string& directory);  
    // currently write_file is append only because I used std::ios::app and it forces all the writes to append to the file and you can't change the existing content

    /**
     * @brief Creates a new, empty file locally and opens it for writing.
     * @param filename The name of the file to create.
     * @param path The server-side path where the file will eventually be stored.
     * @return true on success, false on failure (e.g., file exists).
     */
    bool create_file(const std::string& filename, const std::string& path);

    /**
     * @brief Closes a file. If modified, it flushes the changes to the server.
     * @param filename The name of the file to close.
     * @param directory The server-side path.
     * @return true if the file was successfully closed (and flushed, if needed), false otherwise.
     */
    bool close_file(const std::string& filename, const std::string& directory);


    std::optional<std::map<std::string, std::string>> ls_contents(const std::string& directory); // list the contents in the specified directory
};