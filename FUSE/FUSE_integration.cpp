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

    std::filesystem::path s_path(path);
    std::string filename = s_path.filename().string();
    std::string path_1 = s_path.parent_path().string();

     if (fi->flags & O_TRUNC) {
        if (!get_client()->truncate_file(filename, path_1, 0)){
            std::cout << "FUSE: afs_open is called and failed on "<< filename <<std::endl;
            return -ENOENT;
        };
    }

    bool res = get_client() -> open_file(filename, path_1);
    
    if (!res){
        std::cout << "FUSE: afs_open is called and failed on " << filename <<std::endl;
        return -EACCES; //Access denied
    }
    std::cout << "FUSE: afs_open is called on "<< filename <<std::endl;
    return 0;
}


// 3. Read File
static int afs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info *fi){
    std::filesystem::path s_path(path);
    std::string filename = s_path.filename().string();
    std::string directory = s_path.parent_path().string();
    std::vector<char> buffer;
    
    if (!get_client() -> read_file(filename, directory, size, offset,  buffer)){
        std::cout << "FUSE: afs_read is called and failed on "<< filename <<std::endl;
        return -EACCES; // This may not be the precise error
    }
    memcpy(buf, buffer.data(), buffer.size());  // Actually copy the data!
    std::cout << "FUSE: afs_read is called on "<< filename <<std::endl;
    return buffer.size(); // return the size of the actual data read
}

// 4. Write File
static int afs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
    std::filesystem::path s_path(path);
    std::string filename = s_path.filename().string();
    std::string directory = s_path.parent_path().string();

    std::string data(buf, size);
    if (!get_client()->write_file(filename, data, directory, (std::streampos) offset)){
        std::cout << "FUSE: afs_write is called and failed on " << filename <<std::endl;
        return -EACCES;
    }
    std::cout << "FUSE: afs_write is called on " <<filename <<std::endl;
    return size;
}

// 5. Release File (close)

static int afs_release(const char *path, struct fuse_file_info *fi) {
    std::filesystem::path fs_path(path);
    std::string dir = fs_path.parent_path().string();
    std::string filename = fs_path.filename().string();
    if (dir.empty() || dir == "/") dir = "";

    get_client()->close_file(filename, dir);
    std::cout << "FUSE: afs_release is called on "<< filename <<std::endl;
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
        std::cout << "FUSE: afs_create is called and failed on "<< filename <<std::endl;
        return -EACCES;
    }
    std::cout << "FUSE: afs_create is called on "<< filename <<std::endl;
    return 0;
}


static int afs_getattr(const char *path, struct stat *stbuf){
    memset(stbuf, 0, sizeof(struct stat));
    
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


// Rename (Handles both simple renaming and Atomic Saves)
static int afs_rename(const char* from, const char* to) {
    std::cout << "FUSE: Rename request from " << from << " to " << to << std::endl;

    std::filesystem::path from_p(from);
    std::filesystem::path to_p(to);

    std::string from_dir = from_p.parent_path().string();
    std::string to_dir = to_p.parent_path().string();

    std::string from_name = from_p.filename().string();
    std::string to_name = to_p.filename().string();

    // Handling Root Directory edge cases
    if (from_dir == "/") from_dir = "";
    if (to_dir == "/") to_dir = "";
    
    /*
    // LIMITATION:
    // Current Client implementation takes only one 'directory' argument.
    // This implies we can only rename files inside the SAME directory. Basically we can't move file from one directory to the other directory
    if (from_dir != to_dir) {
        std::cerr << "Error: Moving files between directories is not yet supported." << std::endl;
        // Return EXDEV (Cross-device link) is the standard error when a move 
        // cannot be done atomically (forces cp + rm behavior in some shells), 
        // but EACCES is safer here.
        return -EACCES; 
    }
    */

    // We pass 'from_dir' because we established it is the same as 'to_dir'
    bool success = get_client()->rename_file(from_name, to_name, from_dir, to_dir);

    if (!success) {
        // Broad error code. ideally rename_file would return specific errors,
        // but ENOENT (No such file) or EACCES (Permission denied) are standard fallbacks.
        return -ENOENT; 
    }

    return 0;
}

static int afs_truncate(const char *path, off_t size){
    std::filesystem::path fs_path(path);
    std::string dir = fs_path.parent_path().string();
    std::string filename = fs_path.filename().string();

    if (!get_client()->truncate_file(filename, dir, size)){
        return -ENOENT;
    };
    return 0;
}

static int afs_mkdir(const char *path, mode_t mode){
    std::filesystem::path path_f(path);
    std::string fs_path(path_f);
    if (!get_client()->make_directory(fs_path, mode)){
        std::cout << "FUSE: directory creation failed: " << fs_path << std::endl;
        return -ENOENT;
    }
    return 0;
}

static int afs_unlink(const char *path){
    std::string full_path(path);
    if (!get_client() -> delete_file(full_path)){
        std::cout << "FUSE: file deletion failed: " << full_path << std::endl;
        return -ENOENT;
    }
    return 0;
}

static int afs_unlink_folder(const char *path){
    std::string full_path(path);
    if (!get_client() -> delete_file(full_path)){
        std::cout << "FUSE: folder deletion failed: " << full_path << std::endl;
        return -ENOENT;
    }
    return 0;
}



// dummy function, not implemented
// CHMOD (Change Mode/Permissions)
// The OS calls this to ensure the temp file has the same rights as the original.
static int afs_chmod(const char *path, mode_t mode) {
    // In a real filesystem, you would update the 'mode' in your cached_attr map.
    // For now, returning 0 is enough to let the editor proceed.
    std::cout << "FUSE: chmod called for " << path << " (Mock Success)" << std::endl;
    return 0; 
}


// dummy function, not implemented 
// UTIMENS (Update Timestamps)
// The OS calls this to set specific access/modification times.
static int afs_utimens(const char *path, const struct timespec tv[2]) {
    // tv[0] is atime, tv[1] is mtime
    // Again, returning 0 tricks the editor into thinking it succeeded.
    std::cout << "FUSE: utimens called for " << path << " (Mock Success)" << std::endl;
    return 0;
}

#ifdef __APPLE__
static int afs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position){
#else
static int afs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
#endif
    // macOS tries to set 'com.apple.quarantine' etc.
    // We just say "Okay!" without actually saving it.
    // Ideally, you'd save this to a map, but for basic saving, ignoring is fine.
    
    // On macOS, the function signature might differ slightly depending on FUSE version.
    // If you get a compile error, check if your version adds a 'uint32_t position' arg.
    std::cout << "FUSE: setxattr " << name << " (Ignored)" << std::endl;
    return 0;
}

// Fake getting attributes (Attribute Not Found)
#ifdef __APPLE__
static int afs_getxattr(const char *path, const char *name, char *value, size_t size, uint32_t position) {
#else
static int afs_getxattr(const char *path, const char *name, char *value, size_t size) {
#endif
    // If we say "Success" but return empty, macOS gets confused.
    // It's safer to say "I don't have that attribute" (-ENOATTR is standard on macOS).
    // Note: On Linux this is -ENODATA.
    
    #ifdef __APPLE__
        return -ENOATTR; 
    #else
        return -ENODATA;
    #endif
}



// Fake Disk Space Reporting
static int afs_statfs(const char *path, struct statvfs *stbuf) {
    // 1. Block Size (Standard is 4KB)
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;

    // 2. Total Blocks (Fake a large size, e.g., ~10GB)
    // 2621440 blocks * 4096 bytes = ~10 GB
    stbuf->f_blocks = 2621440;

    // 3. Free Blocks (Pretend it's all free)
    // This is what the editor checks. If this is 0, save fails.
    stbuf->f_bfree = 2621440;
    stbuf->f_bavail = 2621440; 

    // 4. Inodes (File count limits)
    // Some tools check this too. Give plenty.
    stbuf->f_files = 10000;
    stbuf->f_ffree = 10000;

    return 0;
}

// TextEdit calls this on the temp file during atomic save.
// We mock success to keep it happy.
static int afs_chown(const char *path, uid_t uid, gid_t gid) {
    (void) uid; (void) gid; // Mark as unused to silence compiler warnings
    std::cout << "FUSE: chown called for " << path << " (Mock Success)" << std::endl;
    return 0;
}

static int afs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    (void) path; (void) isdatasync; (void) fi;
    // Since we write synchronously in afs_write (or cache in memory until close),
    // we can just say "Yes, it's synced".
    return 0;
}
static int afs_listxattr(const char *path, char *list, size_t size) {
    // If 'size' is 0, the OS is asking "How large is the list?". 
    // We return 0 because we have no attributes.
    // If 'size' > 0, the OS wants the data. We copy nothing and return 0.
    return 0;
}


static fuse_operations afs_oper = {
    .getattr    = afs_getattr,
    .mkdir      = afs_mkdir,
    .unlink     = afs_unlink,
    .rmdir      = afs_unlink_folder, // Maps to 'rmdir' in the struct
    .rename     = afs_rename,
    .chmod      = afs_chmod,
    .chown      = afs_chown,
    .truncate   = afs_truncate,
    .open       = afs_open,
    .read       = afs_read,
    .write      = afs_write,
    .statfs     = afs_statfs,
    .release    = afs_release,
    .fsync      = afs_fsync,
    .setxattr   = afs_setxattr,
    .getxattr   = afs_getxattr,
    .listxattr  = afs_listxattr,
    .readdir    = afs_readdir,
    .create     = afs_create,
    .utimens    = afs_utimens,
};


int main(int argc, char *argv[]){
    const char* env_addr = std::getenv("SERVER_ADDRESS");
    std::string address = env_addr ? std::string(env_addr) : "localhost:50051";
    //std::string address = "192.168.0.31:50051";
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());

    FileSystemClient* client = new FileSystemClient(channel);

    return fuse_main(argc, argv, &afs_oper, client);
}


