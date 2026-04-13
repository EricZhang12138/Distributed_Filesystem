#pragma once

#include <string>
#include <map>
#include <memory>       // For std::unique_ptr, std::shared_ptr
#include <fstream>
#include <optional>
#include <grpcpp/grpcpp.h>
#include "afs_operation.grpc.pb.h"
#include "afs_operation.pb.h"
#include "metadata.grpc.pb.h"
#include "metadata.pb.h"
#include <filesystem>
#include <boost/uuid/uuid.hpp>  // universally unique identifier
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "file_attributes.hpp"
#include <thread>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <sys/statvfs.h>

class FileSystemClient {
private:
    struct FileInfo {
        bool locally_modified;      // True if the local copy has been modified and then if locally_modified == true, we push it to the server on close()
        int64_t timestamp;    // The last known timestamp from the server
        std::string filename; // The base name of the file
        std::string server_address; // Which data server holds this file (for Phase 2)
        std::string server_root;    // The data server's root directory (for building data-server-local paths)
    };
    struct FileStreams {
        // shared_ptr (not unique_ptr) so close_file / fsync_file / read_file / write_file
        // can copy the handle and keep the streams alive past a concurrent erase or replace
        // of the opened_files map entry. Without this, capturing raw .get() pointers and
        // then dropping cache_mutex briefly is a use-after-free hazard.
        std::shared_ptr<std::ifstream> read_stream;
        std::shared_ptr<std::ofstream> write_stream;
    };
    // Metadata server stub (structural ops + subscribe + Lookup/CreateFile)
    std::unique_ptr<metadata::MetadataService::Stub> metadata_stub_;
    // Map of data server address -> stub (for Phase 2 multi-server support)
    std::unordered_map<std::string, std::shared_ptr<afs_operation::operators::Stub>> data_stubs_;
    std::mutex stubs_mutex_;
    // Helper: get or create a data server stub
    afs_operation::operators::Stub* get_data_stub(const std::string& address);
    // In-memory cache of file metadata. key is the directory of the file on the client machine (within the cache)
    std::map<std::string, FileInfo> cache;
    // Map of locally open file handles. Key is the directory of the file on the client machine (within the cache)
    std::map<std::string, FileStreams> opened_files;
    std::mutex cache_mutex; // this mutex is for cache, cache_attr and opened_files all together
    std::string server_root_path_;
    std::string client_id;
    std::unique_ptr<grpc::ClientContext> subscriber_context_;
    std::thread subscriber_thread;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> file_mutexes; // Protects file stream access
    std::string cache_directory;
    void RunSubscriber();

public:
    // Map of locally cached FileAttributes. key is the directory of the file on the server
    std::map<std::string, FileAttributes> cached_attr;
    /**
     * @brief Constructs the client and initializes the connection with the server.
     * @param channel The gRPC channel to use for communication.
     */
    FileSystemClient(std::shared_ptr<grpc::Channel> metadata_channel, std::string cache_path = "./tmp/cache");
    ~FileSystemClient();

    std::string resolve_server_path(const std::string& user_path);

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
    bool read_file(const std::string& filename, const std::string& directory, const int size, const int offset, std::vector<char>& buffer);

    /**
     * @brief Writes data to a previously opened file's local cache.
     * @param filename The name of the file.
     * @param data The string data to write.
     * @param directory The server-side path.
     * @return true on success, false on failure.
     */
    bool write_file(const std::string& filename,const std::string& data, const std::string& directory, std::streampos position);  
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
    
    std::optional<FileAttributes> get_attributes(const std::string& filename, const std::string& path);

    bool rename_file(const std::string& from_name, const std::string& to_name, const std::string& old_path, const std::string& new_path);

    bool truncate_file(const std::string& filename, const std::string& path, const int size);

    bool make_directory(const std::string& directory, const uint32_t mode);

    bool delete_file(const std::string& directory);

    int  delete_directory(const std::string& directory);   // returns 0 or -errno
    bool change_mode (const std::string& filename, const std::string& directory, uint32_t mode);
    bool change_owner(const std::string& filename, const std::string& directory, uint32_t uid, uint32_t gid);
    bool set_times   (const std::string& filename, const std::string& directory, int64_t atime_ns, int64_t mtime_ns);
    bool create_symlink(const std::string& target, const std::string& linkpath_dir, const std::string& linkpath_name);
    bool read_symlink (const std::string& filename, const std::string& directory, std::string& out_target);
    bool fsync_file  (const std::string& filename, const std::string& directory);
    bool get_filesystem_stats(struct statvfs& out);
    bool set_xattr   (const std::string& filename, const std::string& directory, const std::string& name, const std::string& value, int flags);
    int  get_xattr   (const std::string& filename, const std::string& directory, const std::string& name, std::string& out);
    bool list_xattr  (const std::string& filename, const std::string& directory, std::vector<std::string>& out);
    bool remove_xattr(const std::string& filename, const std::string& directory, const std::string& name);
};