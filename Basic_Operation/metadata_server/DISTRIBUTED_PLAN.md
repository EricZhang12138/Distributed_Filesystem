# Distributed AFS — Implementation Plan

## Overview

Transform the current single-server AFS into a distributed system with a **metadata server** (brain) and multiple **data servers** (storage). From the user's perspective, FUSE still shows one unified directory tree.

---

## Architecture

```
                          ┌──────────────────┐
                          │  Metadata Server  │
                          │  (directory tree,  │
                          │   file→server map, │
                          │   notifications)   │
                          └────┬────┬────┬────┘
                 gRPC stubs    │    │    │   heartbeats
              ┌────────────────┘    │    └────────────────┐
              ▼                     ▼                     ▼
        ┌───────────┐        ┌───────────┐        ┌───────────┐
        │ Data Srv A │        │ Data Srv B │        │ Data Srv C │
        │ (blob I/O) │        │ (blob I/O) │        │ (blob I/O) │
        └───────────┘        └───────────┘        └───────────┘
              ▲                     ▲                     ▲
              │         gRPC (content ops)                │
              └──────────────┬───────────────────────────┘
                             │
                        ┌─────────┐
                        │  Client  │── gRPC (metadata ops) ──▶ Metadata Server
                        │  + FUSE  │
                        └─────────┘
```

**Client connections:**
- One persistent connection to the metadata server (metadata ops + subscriber)
- Multiple persistent connections to data servers (created on demand, kept alive)

**Metadata server connections:**
- Accepts client connections
- Holds gRPC stubs to each registered data server (for forwarding delete/rename/etc.)

**Data server connections:**
- Accepts client connections (content ops)
- Sends heartbeats to metadata server on startup and periodically

---

## API Call Paths

### Group 1: Client → Metadata Server (structure + metadata)

These operations only involve the metadata server. The client never talks to a data server for these.

| Operation   | Current path               | New path                                            |
|-------------|----------------------------|-----------------------------------------------------|
| `readdir`   | Client → Data Server       | Client → Metadata Server                            |
| `getattr`   | Client → Data Server       | Client → Metadata Server                            |
| `mkdir`     | Client → Data Server       | Client → Metadata Server                            |
| `chmod`     | Client → Data Server       | Client → Metadata Server → Data Server              |
| `chown`     | Client → Data Server       | Client → Metadata Server → Data Server              |
| `utimens`   | Client → Data Server       | Client → Metadata Server → Data Server              |
| `symlink`   | Client → Data Server       | Client → Metadata Server                            |
| `readlink`  | Client → Data Server       | Client → Metadata Server                            |
| `statfs`    | Client → Data Server       | Client → Metadata Server (aggregates across servers) |
| `subscribe` | Client → Data Server       | Client → Metadata Server                            |

### Group 2: Client → Metadata Server → Data Server (structural changes)

The client sends the request to the metadata server. The metadata server updates its mapping, then forwards the physical operation to the relevant data server.

| Operation | Flow |
|-----------|------|
| `unlink`  | Client → Metadata Server → Data Server (delete file on disk) → Metadata Server removes mapping |
| `rmdir`   | Client → Metadata Server → Data Server (remove dir on disk) → Metadata Server removes mapping |
| `rename`  | Client → Metadata Server → Data Server(s) (rename on disk) → Metadata Server updates mapping. If source and dest are on different data servers: download from A, upload to B, delete from A. |

### Group 3: Client → Data Server (content operations)

Client checks local cache first. On cache miss, asks metadata server for the data server address, then talks directly to the data server.

| Operation  | Flow |
|------------|------|
| `open`     | Cache hit: use local file, no network. Cache miss: Client → Metadata Server (lookup) → Client → Data Server (download content) |
| `close`    | If modified: Client → Data Server (upload content). Data server address is known from the open step. |
| `truncate` | Client → Data Server (already known or lookup via metadata) |
| `fsync`    | Client → Data Server (upload content, same as close but keeps file open) |

### Group 4: Client → Metadata Server (attribute cache)

| Operation  | Flow |
|------------|------|
| `getxattr` / `setxattr` / `listxattr` / `removexattr` | Client → Metadata Server → Data Server. Xattrs are stored on the physical file, so metadata server forwards to the data server. |

---

## New Proto File: `metadata.proto`

```protobuf
service MetadataService {

    // --- Lookup ---
    // Client asks which data server owns a file
    rpc Lookup(LookupRequest) returns (LookupResponse);

    // --- Directory tree operations (moved from data server) ---
    rpc Readdir(ListDirectoryRequest) returns (ListDirectoryResponse);
    rpc Getattr(GetAttrRequest) returns (GetAttrResponse);
    rpc Mkdir(MakeDir_request) returns (MakeDir_response);
    rpc Rmdir(Rmdir_request) returns (Rmdir_response);
    rpc Unlink(Delete_request) returns (Delete_response);
    rpc Rename(RenameRequest) returns (RenameResponse);
    rpc Chmod(Chmod_request) returns (Chmod_response);
    rpc Chown(Chown_request) returns (Chown_response);
    rpc Utimens(Utimens_request) returns (Utimens_response);
    rpc Symlink(Symlink_request) returns (Symlink_response);
    rpc Readlink(Readlink_request) returns (Readlink_response);
    rpc Statfs(Statfs_request) returns (Statfs_response);

    // --- Xattr (forwarded to data server) ---
    rpc SetXattr(SetXattr_request) returns (SetXattr_response);
    rpc GetXattr(GetXattr_request) returns (GetXattr_response);
    rpc ListXattr(ListXattr_request) returns (ListXattr_response);
    rpc RemoveXattr(RemoveXattr_request) returns (RemoveXattr_response);

    // --- Notifications ---
    rpc Subscribe(SubscribeRequest) returns (stream Notification);

    // --- Data server registration ---
    rpc RegisterDataServer(RegisterRequest) returns (RegisterResponse);
    rpc Heartbeat(HeartbeatRequest) returns (HeartbeatResponse);
}

// New messages
message LookupRequest {
    string path = 1;           // full file path
    string client_id = 2;
}
message LookupResponse {
    string server_address = 1; // e.g., "server-b:50051"
    bool found = 2;
}

message RegisterRequest {
    string server_address = 1; // data server's listen address
    string root_dir = 2;       // data server's root directory
}
message RegisterResponse {
    bool success = 1;
}

message HeartbeatRequest {
    string server_address = 1;
}
message HeartbeatResponse {
    bool acknowledged = 1;
}
```

---

## Metadata Server Internal State

```cpp
class MetadataServer {
    // --- File-to-server mapping ---
    // key: file path (e.g., "/docs/report.txt")
    // value: data server address (e.g., "server-b:50051")
    std::unordered_map<std::string, std::string> file_to_server;
    std::mutex file_map_mutex;

    // --- Directory tree ---
    // The metadata server maintains the directory structure.
    // Option A: in-memory tree (fast, lost on restart)
    // Option B: Postgres via database.cpp (persistent, survives restart)

    // --- Data server registry ---
    // key: server address
    // value: stub + health info
    struct DataServerInfo {
        std::shared_ptr<afs_operation::operators::Stub> stub;
        std::chrono::steady_clock::time_point last_heartbeat;
        bool alive;
    };
    std::unordered_map<std::string, DataServerInfo> data_servers;
    std::mutex server_registry_mutex;

    // --- File metadata cache ---
    // Stores getattr results so we don't hit data servers for every stat call
    std::unordered_map<std::string, FileAttributes> attr_cache;

    // --- Notification system ---
    // Moved from data server — subscribers map and NotificationQueues live here now
    std::unordered_map<std::string, std::shared_ptr<NotificationQueue>> subscribers;
    std::mutex subscriber_mutex;
    std::unordered_map<std::string, std::unordered_set<std::string>> file_map;

    // --- Assignment policy ---
    // Determines which data server gets a new file
    std::string assign_server_for_new_file();  // round-robin, least-files, etc.
};
```

---

## Changes to Existing Code

### Client (`filesystem_client.hpp/cpp`)

1. **Add metadata stub:**
   ```cpp
   std::unique_ptr<MetadataService::Stub> metadata_stub_;
   ```

2. **Add data server stub map:**
   ```cpp
   std::unordered_map<std::string, std::shared_ptr<afs_operation::operators::Stub>> data_stubs_;
   std::mutex stubs_mutex;

   std::shared_ptr<afs_operation::operators::Stub> get_data_stub(const std::string& address);
   ```

3. **Modify `open_file`:**
   - Cache hit: unchanged
   - Cache miss: call `metadata_stub_->Lookup(path)` to get data server address, then use `get_data_stub(address)` instead of `stub_`

4. **Modify structural operations** (`ls_contents`, `get_attributes`, `make_directory`, `delete_file`, `rename_file`, `delete_directory`, `change_mode`, `change_owner`, `set_times`, `create_symlink`, `read_symlink`):
   - Change from `stub_->operation()` to `metadata_stub_->operation()`

5. **Modify content operations** (`close_file`, `fsync_file`, `truncate_file`):
   - Need to know which data server the file is on. Store the data server address alongside `FileInfo` in the cache when `open` resolves it.

6. **Modify subscriber:**
   - Connect to metadata server instead of data server

7. **Constructor:**
   - Takes metadata server address instead of data server address
   - Connects to metadata server, does `request_dir` equivalent

### Data Server (`filesystem_server.hpp/cpp`)

1. **Remove:** `file_map`, `subscribers`, notification callbacks, `subscribe()`, `GetStatus`, `request_dir`, `ls`, `getattr`, `mkdir`
2. **Keep:** `open`, `close`, `flush_file`, `truncate`, and the raw filesystem operations (`chmod`, `chown`, etc. — metadata server forwards these)
3. **Add:** heartbeat thread that pings metadata server on startup and every N seconds
4. **Simplify:** data server becomes a thin wrapper around file I/O — no directory tree awareness

### FUSE (`FUSE_integration.cpp`)

**No changes.** FUSE still calls the same `FileSystemClient` methods. The routing logic (metadata vs data server) is entirely inside the client.

### Proto (`afs_operation.proto`)

**Minimal changes.** Keep the existing messages. The data server proto shrinks to content-only RPCs. Add `metadata.proto` as a new file.

### Build (`CMakeLists.txt`)

- Add `metadata.proto` to protobuf generation
- Add `metadata_server` build target
- Update client to link against both proto libraries

---

## Data Server Assignment Policy

When a new file is created (`create_file`):
1. Client → Metadata Server: "create /docs/new.txt"
2. Metadata Server picks a data server (round-robin is simplest to start)
3. Metadata Server stores `/docs/new.txt → server-b:50051`
4. Metadata Server → Data Server B: "create empty file"
5. Returns server address to client so subsequent writes go directly to B

---

## Startup Sequence

1. Start metadata server: `./metadata_server --port 50050 --db postgres://...`
2. Start data servers: `./afs_server /data/root --metadata localhost:50050`
   - Each data server registers itself with the metadata server via `RegisterDataServer`
   - Starts heartbeat thread
3. Start client/FUSE: `./afs_fuse /mnt/afs --metadata localhost:50050`
   - Client connects to metadata server
   - Data server connections created on demand

---

## Migration Path (incremental)

To avoid a big-bang rewrite:

1. **Phase 1:** Build the metadata server that forwards everything to a single data server. System works identically to today but with an extra hop. Validates the architecture.

2. **Phase 2:** Move directory tree operations (`ls`, `getattr`, `mkdir`) into the metadata server. Data server stops handling them.

3. **Phase 3:** Add multi-server support. Metadata server tracks file→server mapping. Client gets `Lookup` and stub map. `create` assigns files to servers.

4. **Phase 4:** Add heartbeats and failure detection. Mark unavailable servers. Optionally add replication.
