# Required Client Changes for Callback System

## Overview
To complete the callback implementation, you need to add `client_id` to all `FileRequest` messages sent from the client.

## Changes Required

### Change 1: In `open_file()` - First open (downloading from server)

**Location:** [filesystem_client.cpp:146](Basic_Operation/client_code/filesystem_client.cpp#L146)

**Current code:**
```cpp
afs_operation::FileRequest request;
request.set_filename(filename);
request.set_directory(resolved_path);
```

**Modified code:**
```cpp
afs_operation::FileRequest request;
request.set_filename(filename);
request.set_directory(resolved_path);
request.set_client_id(client_id);  // ADD THIS LINE
```

---

### Change 2: In `open_file()` - Cache validation (compare)

**Location:** [filesystem_client.cpp:245](Basic_Operation/client_code/filesystem_client.cpp#L245)

**Current code:**
```cpp
afs_operation::FileRequest request;
request.set_filename(filename);
request.set_timestamp(timestamp);
request.set_directory(resolved_path);
```

**Modified code:**
```cpp
afs_operation::FileRequest request;
request.set_filename(filename);
request.set_timestamp(timestamp);
request.set_directory(resolved_path);
request.set_client_id(client_id);  // ADD THIS LINE
```

---

### Change 3: In `close_file()` - Flushing to server

**Location:** [filesystem_client.cpp:650](Basic_Operation/client_code/filesystem_client.cpp#L650)

**Current code:**
```cpp
afs_operation::FileRequest request;
request.set_directory(resolved_path);
request.set_filename(filename);
request.set_content(buffer, len);
```

**Modified code:**
```cpp
afs_operation::FileRequest request;
request.set_directory(resolved_path);
request.set_filename(filename);
request.set_content(buffer, len);
request.set_client_id(client_id);  // ADD THIS LINE
```

---

## Build Steps

After making the changes above, follow these steps:

### 1. Regenerate Protobuf Files
```bash
cd /Users/ericzhang/Documents/Filesystems/Basic_Operation/proto_files
protoc --cpp_out=../build --grpc_out=../build \
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  afs_operation.proto
```

### 2. Rebuild the Project
```bash
cd /Users/ericzhang/Documents/Filesystems/build
cmake ..
make
```

### 3. Verify the Build
```bash
# Check that executables were created
ls -lh afs_server afs_client
```

---

## Testing the Callback System

### Test 1: Single Client
```bash
# Terminal 1: Start server
./afs_server /path/to/server/root

# Terminal 2: Start client
./afs_client

# In FUSE mount:
# 1. Open a file
# 2. Modify it
# 3. Close it
# Expected: Server logs should show registration and UPDATE notification
```

### Test 2: Multi-Client Notification
```bash
# Terminal 1: Start server
./afs_server /path/to/server/root

# Terminal 2: Client A
./afs_client
# Open file1.txt (registers interest)

# Terminal 3: Client B
./afs_client
# Modify and close file1.txt

# Expected in Client A logs:
# "NOTIFICATION RECEIVED: UPDATE for file1.txt"
# Client A's cache for file1.txt should be invalidated
```

### Test 3: Rename Notification
```bash
# Client A: Opens file1.txt
# Client B: Renames file1.txt to file2.txt
# Expected: Client A receives RENAME notification
```

### Test 4: Delete Notification
```bash
# Client A: Opens file1.txt
# Client B: Deletes file1.txt
# Expected: Client A receives DELETE notification
```

---

## Expected Log Output

### Server Logs
```
Client <UUID> subscribing for notifications...
Registered client <UUID> for file: /server/root/file1.txt
Queued UPDATE notification for client <UUID> (file: /server/root/file1.txt)
Sent notification to client <UUID>: UPDATE for file1.txt
```

### Client Logs
```
Client ID: <UUID>
NOTIFICATION RECEIVED: UPDATE for file1.txt
Cache is stale. Overwriting local file...
Successfully overwrote file: ./tmp/cache/server/root/file1.txt
```

---

## Common Issues & Solutions

### Issue: "class afs_operation::FileRequest has no member 'client_id'"
**Solution:** You forgot to regenerate the protobuf files. Run step 1 above.

### Issue: Client doesn't receive notifications
**Possible causes:**
1. `client_id` not being sent in requests → Check client code changes
2. `Subscribe()` RPC not running → Check client subscriber thread
3. File path mismatch → Verify paths in server logs

### Issue: Compilation errors after changes
**Solution:** Make sure you:
1. Modified the `.proto` file correctly
2. Regenerated the protobuf files
3. Rebuilt using `cmake` and `make`

---

## What Happens Behind the Scenes

### When Client Opens a File:
1. Client sends `FileRequest` with `client_id` to server
2. Server's `open()` handler extracts `client_id`
3. Server adds client to `file_map[path]` list
4. Server sends file contents back to client

### When Another Client Modifies the File:
1. Client B sends modified file via `close()`
2. Server writes file to disk and gets new timestamp
3. Server calls `NotifyClients(path, "UPDATE", ...)`
4. Server looks up `file_map[path]` → finds Client A
5. Server pushes notification to Client A's queue
6. Server signals condition variable to wake up Subscribe() thread
7. Subscribe() thread sends notification to Client A
8. Client A receives notification and invalidates cache

### Notification Delivery:
- Subscribe() RPC is a **server-streaming RPC**
- It stays open indefinitely
- Server can push notifications at any time
- Client blocks in `reader->Read()` waiting for notifications
- When notification arrives, `Read()` returns with the notification
- Client processes it, then blocks again waiting for next one

---

## Architecture Diagram

```
┌─────────────────┐                    ┌─────────────────┐
│   Client A      │                    │   Client B      │
│                 │                    │                 │
│ ┌─────────────┐ │                    │                 │
│ │ Subscriber  │ │                    │                 │
│ │ Thread      │ │                    │                 │
│ └──────┬──────┘ │                    │                 │
│        │        │                    │                 │
│   Subscribe()   │                    │   close()       │
│        │        │                    │      │          │
└────────┼────────┘                    └──────┼──────────┘
         │                                    │
         │ [Blocking]                         │
         │                                    │
         ▼                                    ▼
┌─────────────────────────────────────────────────────────┐
│                        Server                           │
│                                                         │
│  ┌──────────────┐         ┌─────────────────────────┐   │
│  │ file_map     │         │ notification_queues     │   │
│  │              │         │                         │   │
│  │ file1.txt:   │         │ Client A: [queue + cv]  │   │
│  │  - Client A  │◄───────►│                         │   │
│  │  - Client C  │         │ Client B: [queue + cv]  │   │
│  └──────────────┘         └─────────────────────────┘   │
│                                                         │
│  When Client B modifies file1.txt:                      │
│  1. NotifyClients("file1.txt", "UPDATE")                │
│  2. Lookup file_map → finds [Client A, Client C]        │
│  3. Push notification to their queues                   │
│  4. Signal condition variables                          │
│  5. Subscribe() threads wake up and send notifications  │
└─────────────────────────────────────────────────────────┘
```

---

## Summary

After making the 3 code changes above and rebuilding:
- ✅ Clients will register interest when they open files
- ✅ Server will track which clients care about which files
- ✅ Notifications will be sent when files are modified
- ✅ Clients will invalidate their cache and refetch on next access

The callback system is now complete!
