# AFS (Andrew File System) — Project Guide

## What this is
A distributed filesystem inspired by AFS. Clients mount a remote filesystem via FUSE, with local caching and a callback-based invalidation system. The server is the authoritative source of truth; clients use close-to-open consistency (changes are pushed to the server on close).

## Project structure
```
Basic_Operation/
  proto_files/          # gRPC/protobuf definitions (afs_operation.proto)
  server_code/          # Data server (filesystem_server.cpp/hpp, subscriber_handler.hpp)
  client_code/          # Client library (filesystem_client.cpp/hpp, client_subscriber.hpp)
  metadata_server/      # Metadata server (planned, see DISTRIBUTED_PLAN.md)
  test/                 # Test clients (Operation_Test, client_test, ci_test_client_1/2)
FUSE/                   # FUSE integration layer (FUSE_integration.cpp) — thin frontend
Dashboard/              # Python dashboard (dashboard.py + index.html) — uses GetStatus RPC
ansible/                # Deployment (deploy.yml, inventory.ini)
build/                  # CMake build output (gitignored)
```

## Build
```bash
cd build
cmake ..
make -j$(nproc)
```
Produces: `afs_server`, `afs_client`, `client_test`, `register_test`, `ci_test_client_1`, `ci_test_client_2`

Dependencies: gRPC, Protobuf, Boost (uuid), FUSE (macFUSE on macOS / libfuse on Linux), pkg-config

## Run
```bash
# Server — pass the root directory to serve
./afs_server /path/to/server/root

# Client (FUSE mount) — set SERVER_ADDRESS env var, pass FUSE args
SERVER_ADDRESS=localhost:50051 ./afs_client /path/to/mountpoint
```
Server listens on `0.0.0.0:50051` by default.

## Architecture overview
- **FUSE layer** (`FUSE_integration.cpp`): thin translation from POSIX calls to FileSystemClient methods. No business logic here.
- **Client** (`filesystem_client.cpp`): manages local cache (`cache`, `cached_attr`, `opened_files` maps), handles RPCs, runs subscriber thread for server-push notifications.
- **Server** (`filesystem_server.cpp`): performs actual filesystem I/O, maintains `file_map` (which clients have which files cached) and `file_map_open` (which files are currently open), pushes notifications via `NotificationQueue` per subscriber.
- **Subscriber system**: server pushes UPDATE/DELETE/RENAME notifications to clients. Clients invalidate their cache on receipt.

## Key concurrency model
- Server: gRPC thread pool handles RPCs concurrently. Three mutexes: `file_map_mutex`, `file_map_open_mutex`, `subscriber_mutex`. Lock ordering: `file_map_mutex` → `subscriber_mutex`.
- Client: `cache_mutex` protects `cache`, `cached_attr`, and `opened_files`. Per-file mutexes in `file_mutexes` protect individual stream I/O. Pattern: acquire `cache_mutex` to look up per-file mutex, release `cache_mutex`, then acquire per-file mutex.

## Coding conventions
- C++17 (`CMAKE_CXX_STANDARD 17`)
- gRPC for all client-server communication
- Protobuf messages defined in `afs_operation.proto`
- Prefer `std::lock_guard` / `std::unique_lock` over manual lock/unlock
- Cross-platform: `#ifdef __APPLE__` guards for macOS vs Linux differences (xattr, stat timespec)
- `subscriber_handler.hpp` and `client_subscriber.hpp` define member functions in headers — included from exactly one .cpp each (do not include from multiple translation units)

## Platform
- Primary development: macOS (Darwin)
- Also targets Linux (Docker, CI)
- macFUSE on macOS, libfuse on Linux
