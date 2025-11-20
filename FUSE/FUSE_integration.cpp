#define FUSE_USE_VERSION 26
#include <fuse.h>
// /usr/local/lib/*fuse*.dylib, where the libraries are installed 
#include <string.h>
#include <errno.h>
#include <iostream>
#include <memory>
#include "filesystem_client.hpp"

static FileSystemClient* get_client(){
    return static_cast<FileSystemClient*>(fuse_get_context()->private_data);
    // static type is compile time check and it tells the compiler that "I know 
    //more than you and I am sure variable of type A can be treated as a variable of type B"
}


// 1. Read Directory
static int afs_readdir(const char* path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
    (void) offset; (void) fi; // avoid "unused parameter" warnings
    std::string s_path(path); // std::string is type safe, meaning it checks type matching at compile time

    auto contents = get_client() -> ls_contents(s_path);
    if (!contents){
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for (const auto& [name, type] : *contents) {   
        //*contents is dereferencing the optional! going from std::optional<std::map<std::string, std::string>> to std::map<std::string, std::string>
        filler(buf, name.c_str(), NULL, 0);
    }
    return 0;
}

// 2. Open File
static int afs_open(const char* path, struct fuse_file_info *fi){
    (void*) fi;
    std::filesystem::path s_path(path);
    std::string filename = s_path.filename().string();
    std::string path_1 = s_path.parent_path().string();

    bool res = get_client() -> open_file(filename, path_1);
    if (!res){
        return -EACCES; //Access denied
    }
    return 0;
}


// 3. Read File
static int afs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info *fi){
    std::filesystem::path s_path(path);
    std::string filename = s_path.filename().string();
    std::string directory = s_path.parent_path().string();
    std::vector<char> buffer;
    
    if (!get_client() -> read_file(filename, directory, size, offset,  buffer)){
        return -EACCES; // This may not be the precise error
    }
    memcpy(buf, buffer.data(), buffer.size());  // Actually copy the data!
    return buffer.size(); // return the size of the actual data read
}

// 4. Write File
static int afs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
    std::filesystem::path s_path(path);
    std::string filename = s_path.filename().string();
    std::string directory = s_path.parent_path().string();

    std::string data(buf, size);
    if (!get_client()->write_file(filename,data, directory, (std::streampos) offset)){
        return -EACCES;
    }
    return size;
}

// 5. Release File (close)

static int afs_release(const char *path, struct fuse_file_info *fi) {
    std::filesystem::path fs_path(path);
    std::string dir = fs_path.parent_path().string();
    std::string filename = fs_path.filename().string();
    if (dir.empty() || dir == "/") dir = "";

    get_client()->close_file(filename, dir);
    return 0;
}


// 6. Create File (create)
static int afs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void) mode; (void) fi; // Unused
    
    std::filesystem::path fs_path(path);
    std::string dir = fs_path.parent_path().string();
    std::string filename = fs_path.filename().string();
    if (dir.empty() || dir == "/") dir = "";

    if (!get_client()->create_file(filename, dir)) {
        return -EACCES;
    }
    return 0;
}


static int afs_getattr(const char *path, struct stat *stbuf){
    memset(stbuf, 0, sizeof(struct stat));
    
    // Handle Root Directory Special Case
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;   // owner permission: 7, group permission: 5, other permission: 5 
        stbuf->st_nlink = 2;
        return 0;
    }
    
    std::filesystem::path fs_path(path);
    std::string dir = fs_path.parent_path().string();
    std::string filename = fs_path.filename().string();
    
    if (dir.empty() || dir =="/") dir = "";
    
    // call existing client logic
    auto attrs_opt = get_client()->get_attributes(filename, dir);
    
    if (!attrs_opt){
        return -ENOENT;
    }
    
    auto attrs = *attrs_opt;
    
    stbuf->st_mode = attrs.mode;
    stbuf->st_nlink = attrs.nlink;
    stbuf->st_size = attrs.size;
    stbuf->st_uid = attrs.uid;
    stbuf->st_gid = attrs.gid;
    
    // Convert int64 nanoseconds back to timespec (seconds and nanoseconds)
    // Note: This depends on your OS. Linux uses st_mtim, macOS uses st_mtimespec
    #ifdef __APPLE__
        stbuf->st_mtimespec.tv_sec = attrs.mtime / 1000000000;
        stbuf->st_mtimespec.tv_nsec = attrs.mtime % 1000000000;
        
        stbuf->st_atimespec.tv_sec = attrs.atime / 1000000000;
        stbuf->st_atimespec.tv_nsec = attrs.atime % 1000000000;
        
        stbuf->st_ctimespec.tv_sec = attrs.ctime / 1000000000;
        stbuf->st_ctimespec.tv_nsec = attrs.ctime % 1000000000;
    #else
        stbuf->st_mtim.tv_sec = attrs.mtime / 1000000000;
        stbuf->st_mtim.tv_nsec = attrs.mtime % 1000000000;
        
        stbuf->st_atim.tv_sec = attrs.atime / 1000000000;
        stbuf->st_atim.tv_nsec = attrs.atime % 1000000000;
        
        stbuf->st_ctim.tv_sec = attrs.ctime / 1000000000;
        stbuf->st_ctim.tv_nsec = attrs.ctime % 1000000000;
    #endif
    return 0;
}



static fuse_operations afs_oper = {
    .getattr = afs_getattr,
    .readdir = afs_readdir,
    .open = afs_open,
    .read = afs_read,
    .write = afs_write,
    .release = afs_release,
    .create = afs_create, 
};


int main(int argc, char *argv[]){
    std::string address = "localhost:50051";
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());

    FileSystemClient* client = new FileSystemClient(channel);

    return fuse_main(argc, argv, &afs_oper, client);
}


