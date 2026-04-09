# Implementing Missing FUSE-Supporting APIs

This document records the design, implementation, and post-implementation
cleanup of the work that added 13 new RPCs to the distributed filesystem so
that the FUSE layer no longer lies to the kernel about file metadata,
extended attributes, free space, durability, symlinks, or directory removal.

It is intentionally exhaustive: it captures *what* was changed, *where* it
was changed, and *why* each decision was made the way it was.

---

## 1. Why this work was needed

Before this change, the FUSE handler table in `FUSE/FUSE_integration.cpp`
registered a number of stubs that returned success without doing anything,
and several common handlers were not registered at all. Concretely:

| Symptom | Cause |
| --- | --- |
| `cp -p`, `make`, `rsync -t`, editors all see drift in mode/owner/timestamps | `chmod`, `chown`, `utimens` were stubbed and silently lied |
| macOS quarantine, Finder tags, SELinux labels were dropped | `xattr` ops were no-ops |
| `df` reports a hard-coded ~10 GB | `statfs` returned fake fields |
| Databases got false durability | `fsync` returned `0` without flushing |
| `O_TRUNC` only resized the local cache copy | server-side `truncate` did not exist |
| Symlinks could neither be created nor followed | `symlink`/`readlink` not registered |
| `rmdir` on a non-empty directory reported `ENOENT` instead of `ENOTEMPTY` | `rmdir` shared the `unlink` RPC, which has no notion of directory semantics |
| `getattr` dereferenced symlinks | the server used `stat` instead of `lstat`, so it could never report `S_IFLNK` |

The plan was to add the missing proto messages and RPCs, the matching
server handlers, the matching client wrappers, and the FUSE callbacks —
without introducing any new build dependencies (the existing CMake +
gRPC build already had everything we needed).

---

## 2. Locked-in design decisions (and the reasoning behind each)

These decisions were made up-front and held throughout the implementation.

### 2.1 POSIX-strict `rmdir` (no recursive flag)

`rmdir` removes one directory and refuses if it is non-empty. There is no
`recursive` flag in the RPC.

**Why:** the POSIX `rm -rf` walks the tree itself by issuing `unlink` and
`rmdir` calls *through* the FUSE mount. As long as both of those work
correctly, `rm -rf` works correctly. Adding a recursive variant of the RPC
would (a) duplicate logic that already lives in `rm`, (b) make the error
semantics more confusing for partial failures, and (c) make it harder for
the server to surface `ENOTEMPTY` cleanly when callers genuinely want it.

### 2.2 RPC-first cache update for chmod/chown/utimens/truncate

The client always sends the RPC first, then updates `cached_attr` only if
the RPC succeeded.

**Why:** if you update the cache first and the RPC fails, the client
believes a state that the server does not — and the next `getattr` will
yank it back, producing a flicker that other clients also see. RPC-first
keeps the client and server in lock-step. This also matches the existing
pattern in `write_file` and `make_directory`.

### 2.3 Host-OS xattrs (persistent across server restarts)

`setxattr`/`getxattr`/`listxattr`/`removexattr` on the server call the
host's `setxattr(2)`/`getxattr(2)` family directly on the underlying file.

**Why:** the alternative is an in-memory side-table that has to be
serialized somewhere. The host filesystem already has a perfectly good
xattr store with persistence, locking, and overflow semantics — using it
gives us all of that for free, and means a client can `xattr -w` a file on
one machine and read it back on another even after a server restart. The
small price we pay is dealing with the macOS vs Linux API split, which is
isolated in four static helpers (see §4.3).

### 2.4 Cover all of groups A–I in one pass

Rather than landing rmdir, then chmod, then xattrs as separate diffs, we
shipped all 9 groups together.

**Why:** they all share the same proto file, the same handler scaffolding,
and the same FUSE registration table. Splitting them into 9 PRs would
have meant editing the same six files nine times each, with merge
conflicts at every layer. The implementation is mechanical once the
pattern is established, so the cost of doing them together is much lower
than the cost of doing them separately.

---

## 3. Files touched

| File | Role |
| --- | --- |
| `Basic_Operation/proto_files/afs_operation.proto` | Wire format. 14 new messages + 13 new RPCs. |
| `Basic_Operation/server_code/filesystem_server.hpp` | 13 new private RPC overrides + new `file_change_callback_metadata` declaration. |
| `Basic_Operation/server_code/filesystem_server.cpp` | New RPC bodies + xattr static helpers + `lstat` fix on lines 14 and 249. |
| `Basic_Operation/client_code/filesystem_client.hpp` | 12 new client method declarations + `<sys/statvfs.h>`. |
| `Basic_Operation/client_code/filesystem_client.cpp` | 12 new client method bodies + RPC-first rewrite of `truncate_file`. |
| `FUSE/FUSE_integration.cpp` | Replaced 8 stubbed handlers, added 3 new handlers, updated `afs_oper`. |
| `CMakeLists.txt` | **No edits.** The existing `add_custom_command` at line 33 already regenerates the `.pb.*` and `.grpc.pb.*` files whenever the proto changes. |

---

## 4. Phase-by-phase walk-through

The work was sequenced into seven phases. Each phase compiles on its own,
which is what allows us to verify progress incrementally instead of
landing the whole thing as one un-debuggable blob.

### Phase 1 — `afs_operation.proto`

14 new messages were added before `service operators { ... }`:

- `Rmdir_request`/`Rmdir_response`
- `Chmod_request`/`Chmod_response` (response carries `timestamp` so the
  client can update `ctime`)
- `Chown_request`/`Chown_response` (same)
- `Utimens_request`/`Utimens_response` (atime+mtime in **nanoseconds**)
- `Symlink_request`/`Symlink_response`
- `Readlink_request`/`Readlink_response` (the response's `target` is the
  raw link string, not a resolved path)
- `Truncate_request`/`Truncate_response`
- `Fsync_response` (the request reuses the existing streamed
  `FileRequest` so the upload format is identical to `close`)
- `Statfs_request`/`Statfs_response` (matches `struct statvfs` field-by-field)
- `SetXattr_request`/`SetXattr_response`
- `GetXattr_request`/`GetXattr_response` — note the `bool found` field;
  see §6.2 for why this is not an error path
- `ListXattr_request`/`ListXattr_response` — the names come back as a
  `repeated string` rather than a single null-separated buffer, because
  protobuf has a perfectly good list type
- `RemoveXattr_request`/`RemoveXattr_response`

Then the 13 new RPCs were added inside `service operators { ... }`,
positioned after `unlink` and before `subscribe` so the existing
`compare` comment block stays where it was.

**Why nanoseconds for timestamps?** macOS and Linux both already store
timestamps at nanosecond precision via `struct timespec`, and FUSE passes
us nanosecond values. Going through `int64` ns avoids any
double-conversion loss between `time_t` and `timespec`.

### Phase 2 — `filesystem_server.hpp`

Two changes:

1. 13 new private overrides for the RPCs declared in Phase 1, inserted
   between the existing `unlink` line and the `subscribe` line so the
   ordering matches the proto file's RPC ordering.
2. A new private declaration:
   ```cpp
   bool file_change_callback_metadata(const std::string& path,
                                      const std::string& client_id,
                                      afs_operation::Notification& notif);
   ```

**Why a new callback?** The existing `file_change_callback_unlink` does
two things: notifies subscribers, and erases the path from `file_map`
because the file is gone. For chmod/chown/utimens/truncate/fsync the file
is *not* gone — only its metadata changed — so we need a sibling that
notifies subscribers but leaves `file_map` alone. Reusing
`file_change_callback_unlink` would silently drop the cache-tracking for
files that are still very much alive.

### Phase 3 — `filesystem_server.cpp`

#### 3.1 Includes

Added `<sys/xattr.h>`, `<sys/statvfs.h>`, `<unistd.h>` (for `::chown`,
`::truncate`, `::fsync`, `::open`, `::close`), `<fcntl.h>`, `<cerrno>`,
`<cstring>`, `<vector>`.

#### 3.2 The `lstat` fix

Changed `stat(path.c_str(), &s)` → `lstat(path.c_str(), &s)` in two
places:

- Line 14, `get_file_timestamp`
- Line 249, `getattr`

**Why this matters:** the kernel resolves symlinks before issuing `open`,
so the only time the server is asked about a symlink is from `getattr`.
`stat` would dereference the link and return the *target's* mode and
size. `lstat` returns the link's own `st_mode` (with `S_IFLNK` set) and
the size of the link target string. That is exactly the contract FUSE
expects, and it is what allows symlinks to render correctly in `ls -l`
(the leading `l`) and to be followed correctly via `readlink`.

There is one caveat: for regular files, `lstat` and `stat` return
identical results. So this change cannot break any existing flow that
worked before.

#### 3.3 Cross-platform xattr static helpers

Added four file-static helpers:

```cpp
static int     _set_xattr   (const char* p, const char* n, const void* v, size_t s, int f);
static ssize_t _get_xattr   (const char* p, const char* n, void* v, size_t s);
static ssize_t _list_xattr  (const char* p, char* l, size_t s);
static int     _remove_xattr(const char* p, const char* n);
```

Each one branches on `#ifdef __APPLE__` so the macOS form
(`setxattr(p, n, v, s, position, options)`, with `XATTR_NOFOLLOW`) and
the Linux form (`setxattr(p, n, v, s, flags)`) are both handled. The
RPC bodies just call the helpers — they don't have to know which OS
they're on.

**Why `XATTR_NOFOLLOW`?** The same reason `lstat` is the right call:
when an xattr is set on a symlink, you almost always mean the symlink
itself, not whatever it points to. macOS Finder tags on aliases work
this way and so does macOS quarantine.

#### 3.4 The metadata callback body

```cpp
bool FileSystem::file_change_callback_metadata(...) {
    std::lock_guard<std::mutex> lock_file_map(file_map_mutex);
    auto it = file_map.find(path);
    if (it == file_map.end()) return true;
    auto client_set = it->second;
    std::lock_guard<std::mutex> lock_subscribers(subscriber_mutex);
    for (const auto& client : client_set) {
        if (client == client_id) continue;     // don't notify self
        auto q = subscribers.find(client);
        if (q != subscribers.end()) q->second->push(notif);
    }
    // Intentionally do NOT erase file_map[path] — file still exists.
    return true;
}
```

**Why notify everyone except the originator?** The originator already
knows they made the change (they're about to update their own cache as
part of the RPC return). Sending them a notification would cause an
extra round-trip on their next `getattr` for nothing. This pattern
matches `file_change_callback_unlink`.

#### 3.5 The 13 new RPC handler bodies

All bodies follow the same shape:

1. Construct `path = directory + (trailing slash) + filename`
2. Perform the POSIX call
3. Map errno to a `grpc::StatusCode`
4. Build a `Notification` and push via the metadata callback
5. Set timestamp on the response (where applicable)

A few interesting ones:

- **`rmdir`**: after the cleanup pass (see §6.2), this collapses to a
  single `::rmdir(path.c_str())` call with an `errno` switch. The
  important mapping is `ENOTEMPTY → FAILED_PRECONDITION`, which the
  client then surfaces back to FUSE as `-ENOTEMPTY`. Without that path,
  `rm -rf` cannot tell apart "this directory is gone" from "this
  directory still has files".
- **`chmod`**: POSIX `::chmod(path, mode)`. We do not pre-mask the mode
  because the kernel has already done that for us before issuing the
  FUSE call.
- **`chown`**: POSIX `::lchown` (so symlinks themselves get chowned, not
  their target). Maps `EPERM`/`EACCES` → `PERMISSION_DENIED`.
- **`utimens`**: POSIX `::utimensat(AT_FDCWD, path, times,
  AT_SYMLINK_NOFOLLOW)`. Nanoseconds in, nanoseconds out, with no
  intermediate `time_t` conversion.
- **`symlink`**: `std::filesystem::create_symlink(target, linkpath, ec)`.
  `file_exists` maps to `ALREADY_EXISTS` so the kernel can return
  `-EEXIST` cleanly.
- **`readlink`**: `std::filesystem::read_symlink(path, ec)`. Returns
  the raw target string. We do not resolve it because POSIX `readlink`
  does not resolve it either.
- **`flush_file`**: streamed-upload RPC, structurally similar to
  `close`. The big difference is that `flush_file` *must not* erase the
  file from `file_map_open` afterwards, because the whole point of
  fsync is that the file stays open. After the post-cleanup pass (see
  §6.2 again), the body opens the path with one `::open(O_WRONLY |
  O_CREAT | O_TRUNC, 0644)`, writes each chunk as it arrives, calls
  `::fsync(fd)` once at the end, and `::close(fd)`. There is no
  intermediate `std::ofstream`.
- **`statfs`**: POSIX `::statvfs(root_dir, &buf)` followed by a copy
  into the response message. There is nothing tricky here — `statvfs`
  fields map one-to-one to the proto fields by design.
- **xattr quartet**: each one calls the matching static helper from
  §3.3. `getxattr` does the standard "size query then allocate then
  fetch" pattern: call once with `nullptr, 0` to get the size, allocate
  a buffer of that size, call again. `listxattr` parses the
  null-separated buffer it gets back into the proto's `repeated string`
  field.

### Phase 4 — `filesystem_client.hpp`

Added `<vector>`, `<sys/statvfs.h>` to the includes, then 12 new method
declarations after `delete_file`:

```cpp
int  delete_directory(const std::string& directory);   // returns 0 or -errno
bool change_mode (...);
bool change_owner(...);
bool set_times   (...);
bool create_symlink(...);
bool read_symlink (...);
bool fsync_file  (...);
bool get_filesystem_stats(struct statvfs& out);
bool set_xattr   (...);
int  get_xattr   (...);
bool list_xattr  (...);
bool remove_xattr(...);
```

**Why is `delete_directory` `int` and not `bool`?** Because the FUSE
layer needs to distinguish at minimum `ENOENT`, `ENOTEMPTY`, and
`EACCES` so that `rm -rf` and friends behave correctly. A `bool` would
collapse all of those into "false" and the kernel would have to guess
what went wrong.

**Why is `get_xattr` `int`?** Same reason. The xattr API needs to
return either the value's length (success) or a negative error code so
that FUSE can map `-ERANGE` (caller's buffer too small) vs `-ENOATTR`
(no such attribute).

### Phase 5 — `filesystem_client.cpp`

Added `<sys/statvfs.h>`, `<cstring>`, `<cerrno>` to the includes.

Then 12 new method bodies, each following the existing
`make_directory` pattern: `resolve_server_path → set request fields →
stub call → on success update local state under `cache_mutex`. The
interesting ones:

- **`delete_directory`**: includes a `sweep` lambda that walks `cache`,
  `opened_files`, `cached_attr`, and `file_mutexes`, erasing any keys
  whose path begins with the directory prefix. Then sends the `rmdir`
  RPC and maps the gRPC status to a negative errno. After RPC success,
  also calls `std::filesystem::remove_all(cache_path, ec)` to wipe the
  local cache copy.

  **Why sweep first, then RPC?** Because once the RPC succeeds the
  directory is gone on the server, and any cached state for files
  inside it is now wrong. If we leave it there until the next
  `getattr`, the client will keep telling the kernel about files that
  no longer exist. Sweeping first means the cache is consistent with
  the server's view as soon as the RPC returns.

  **Race window:** if another client opens a file inside the directory
  between our sweep and our RPC, the server's `rmdir` will fail with
  `ENOTEMPTY` and we return that to FUSE. The other client's view is
  unchanged. This is the correct behavior.

- **`change_mode`**: preserves the upper file-type bits when updating
  cached `mode` with `(it->second.mode & ~0xFFF) | (mode & 0xFFF)`.

  **Why mask?** The cached `mode` includes `S_IFREG`/`S_IFDIR` in the
  upper bits. If we just overwrote `mode` with the value the user
  passed, the next `ls -l` would show `?` for the file type, because
  the kernel needs those bits to know what kind of inode this is.

- **`change_owner`**: does the RPC and updates `ctime` in the cache,
  but **does not** update cached `uid`/`gid`. That is because the
  client's existing `get_attributes` (line ~115) deliberately spoofs
  every file's uid/gid to `getuid()`/`getgid()` so that local `ls -l`
  shows the current user. This is a property of the existing design,
  not something we changed; the comment in the body documents that
  ownership changes succeed on the server but won't be visible in
  client `ls -l`.

- **`fsync_file`**: structurally mirrors `close_file` so the upload
  retry loop and the unique_lock/file_mtx unlock-relock dance behave
  identically. The differences are:
  - Calls `stub_->flush_file` instead of `stub_->close`.
  - Does **not** erase from `opened_files` / `file_mutexes` afterwards
    — file stays open.
  - Updates cache `timestamp` and `locally_modified=false` on success
    so the next read won't re-fetch unnecessarily.

- **`truncate_file`** (rewritten): originally it only resized the local
  cache. Now it:
  1. Calls `stub_->truncate` first.
  2. On success, takes the per-file mutex (if any) and resizes the
     local cache copy. The mutex acquisition (added in the simplify
     pass — see §6.2 #5) prevents a concurrent writer from clobbering
     the size mid-resize.
  3. Updates `cached_attr` size + `mtime` + `ctime` from the server's
     response.

  RPC-first ordering means a server-side failure leaves the cache
  untouched and a single error code propagates to FUSE.

- **xattr quartet**: pass-through methods. `get_xattr` returns `-1` on
  RPC failure or `!response.found()`, otherwise the byte length. The
  byte length signals success and lets the caller know how much data
  to copy.

#### Subscriber

`client_subscriber.hpp` was **not** changed. The existing
"erase from cache and cached_attr on notification" logic correctly
handles `UPDATE` and `DELETE` for all of the new metadata-mutating
operations: when one client changes a file's mode, every subscribed
client gets a notification, evicts the path from its cache, and
re-fetches on the next `getattr`. No new notification handling was
needed.

### Phase 6 — `FUSE/FUSE_integration.cpp`

Added `<vector>` and `<sys/statvfs.h>` to the includes.

#### 6.1 `afs_unlink_folder` → `afs_rmdir`

Renamed and rewired to call `delete_directory`:

```cpp
static int afs_rmdir(const char *path) {
    std::string full_path(path);
    int rc = get_client()->delete_directory(full_path);
    if (rc < 0) {
        std::cout << "FUSE: rmdir failed: " << full_path
                  << " errno=" << -rc << std::endl;
        return rc;   // already negative errno
    }
    return 0;
}
```

`afs_unlink` was left unchanged — files still go through `delete_file`.

#### 6.2 Replaced eight stubbed handlers

`afs_chmod`, `afs_utimens`, `afs_setxattr`, `afs_getxattr`, `afs_chown`,
`afs_statfs`, `afs_fsync`, `afs_listxattr`. Each one splits the path,
calls the corresponding client method, and maps boolean failure to a
sensible negative errno (`-EACCES`, `-EPERM`, `-EIO`, etc.). The xattr
handlers also implement the size-query / `ERANGE` pattern: if the
caller passes `size == 0` they want the length back; if they pass too
small a buffer they get `-ERANGE`; otherwise they get the data plus the
length.

#### 6.3 Three new handlers

- **`afs_symlink`** — pass-through to `create_symlink`.
- **`afs_readlink`** — calls `read_symlink`, then copies up to
  `min(target.size(), size - 1)` bytes into the buffer, null-terminates,
  and returns `0` (NOT the length — POSIX `readlink` returns the length
  but FUSE's `readlink` callback returns 0/errno).
- **`afs_removexattr`** — pass-through to `remove_xattr`.

#### 6.4 The `afs_oper` struct

Updated to declaration order matching FUSE 2.6, with all of the new
handlers wired in:

```cpp
.getattr     = afs_getattr,
.readlink    = afs_readlink,
.mkdir       = afs_mkdir,
.unlink      = afs_unlink,
.rmdir       = afs_rmdir,
.symlink     = afs_symlink,
.rename      = afs_rename,
.chmod       = afs_chmod,
.chown       = afs_chown,
.truncate    = afs_truncate,
.open        = afs_open,
.read        = afs_read,
.write       = afs_write,
.statfs      = afs_statfs,
.release     = afs_release,
.fsync       = afs_fsync,
.setxattr    = afs_setxattr,
.getxattr    = afs_getxattr,
.listxattr   = afs_listxattr,
.removexattr = afs_removexattr,
.readdir     = afs_readdir,
.create      = afs_create,
.utimens     = afs_utimens,
```

**Why this exact order?** C++ designated initializers must appear in
the same order as the struct's declaration. Get the order wrong and
the compiler will reject the file. The order above mirrors the
`fuse_operations` declaration in `<fuse.h>` for FUSE 2.6.

### Phase 7 — Build & verification

```bash
cmake --build build -j --target afs_server afs_client \
                              ci_test_client_1 ci_test_client_2
```

CMake's `add_custom_command` (line 33 of `CMakeLists.txt`)
auto-regenerates the `.pb.*` and `.grpc.pb.*` files from the proto
file, so no manual proto regeneration is needed.

All four targets compile cleanly with no warnings. The `client_test`
and `register_test` targets fail to link with Boost-related symbols,
but this is **pre-existing** — verified by stashing our changes and
rebuilding `main`. Their CMake definitions don't link `Boost::boost`
even though they include `filesystem_client.hpp`, which transitively
needs Boost UUID. Out of scope for this work.

End-to-end verification commands (run against a live FUSE mount at `$MNT`):

```bash
mkdir "$MNT/testdir" && echo hello > "$MNT/testdir/file.txt"
chmod 600 "$MNT/testdir/file.txt"        # exercises chmod
touch -d '2020-01-01 00:00:00' "$MNT/testdir/file.txt"   # utimens
chown $USER "$MNT/testdir/file.txt"      # chown (no-op success)
truncate -s 100 "$MNT/testdir/file.txt"  # server-side truncate
ln -s file.txt "$MNT/testdir/link"       # symlink
readlink "$MNT/testdir/link"             # readlink
mkdir "$MNT/testdir/empty" && rmdir "$MNT/testdir/empty"   # rmdir empty
rmdir "$MNT/testdir" 2>&1 | grep 'not empty'               # rmdir non-empty
rm -rf "$MNT/testdir"                                       # rm -rf
df "$MNT"                                                   # statfs
python3 -c "import os; fd=os.open('$MNT/fs.txt',
    os.O_CREAT|os.O_WRONLY); os.write(fd,b'hi'); os.fsync(fd); os.close(fd)"
xattr -w com.test hello "$MNT/fs.txt"      # macOS xattr
xattr -p com.test "$MNT/fs.txt"
xattr -d com.test "$MNT/fs.txt"
```

Each step exercises one of the new RPCs end-to-end.

---

## 5. Risks and gotchas (and how each is handled)

1. **`lstat` change.** Identical behavior for regular files, so existing
   tests still pass. Only behavior change is for symlinks, where the new
   semantics are correct.
2. **`chmod` mask preservation in cache.** Without masking, the next
   `ls -l` would show `?` for the file type because the upper bits would
   be lost. Fixed in `change_mode`.
3. **`chown` cache spoofing.** `get_attributes` overrides uid/gid with
   `getuid()/getgid()`. Server-side chown succeeds but client `ls -l`
   still shows the local user. This matches existing design — documented
   in a code comment.
4. **`utimensat AT_SYMLINK_NOFOLLOW`.** Supported on macOS 10.13+ and
   all current Linux. If older macOS becomes a target, fall back to
   `::lutimes`.
5. **`flush_file` must not erase `file_map_open`.** Whole point is the
   file stays open. Uses `file_change_callback_metadata`, never
   `file_change_callback_unlink`.
6. **Truncate ordering.** RPC-first means a failed RPC leaves the
   cache untouched. If the order were flipped, a failed RPC would leave
   the cache wrong with no way to detect it.
7. **`delete_directory` race.** Sweep cache keys under lock, then send
   RPC. If another client opens a file inside the dir between sweep
   and RPC, the server's rmdir fails with ENOTEMPTY — the correct
   behavior.
8. **macOS `XATTR_NOFOLLOW` / `ENOATTR` vs Linux `ENODATA`.** All
   guarded with `#ifdef __APPLE__`.
9. **Symlinks in `readdir`.** The current `ls()` only emits Directory
   and Regular_File entries; symlinks are silently dropped from
   `readdir`. Adding an `else if (entry.is_symlink())` branch is a
   2-line follow-up — flagged as a known gap, not fixed in this pass.
10. **Stale `.pb.*` files.** If a build fails with "no such symbol
    Rmdir_request" after editing the proto, force a regeneration:
    `rm -rf build && cmake -B build && cmake --build build -j`.
11. **Permission errors on macOS xattr.** macFUSE's default
    `allow_other` and `noapplexattr` mount options can change xattr
    behavior. If `xattr -w` returns "operation not permitted", check
    the mount options.

---

## 6. The simplify pass

After the main implementation, the `simplify` skill was invoked. Three
agents (reuse, quality, efficiency) reviewed the diff in parallel and
flagged 30+ items. Each finding was triaged into "real bug or hot-path
waste" (fixed) vs "out of scope or false positive" (skipped).

### 6.1 Findings that were fixed

#### Fix 1 — `getattr` double `lstat` (H1)

**Problem:** the `getattr` body was calling both `lstat(path, &s)` *and*
`get_file_timestamp(path)`, where `get_file_timestamp` itself does
another `lstat`. That's two stat syscalls per `getattr`, which is the
single hottest path in the entire FUSE workload.

**Fix:** reuse the existing `s` struct for the timestamp:

```cpp
#ifdef __APPLE__
    response->set_mtime(static_cast<int64_t>(s.st_mtimespec.tv_sec) * 1'000'000'000LL
                      + s.st_mtimespec.tv_nsec);
#else
    response->set_mtime(static_cast<int64_t>(s.st_mtim.tv_sec) * 1'000'000'000LL
                      + s.st_mtim.tv_nsec);
#endif
```

**Why it matters:** `getattr` is called by the kernel for *every* path
component on every operation. Halving the syscall count there pays for
itself many times over.

#### Fix 2 — `rmdir` TOCTOU quartet (H2)

**Problem:** the original `rmdir` body did:

```cpp
if (!std::filesystem::exists(path))           return NOT_FOUND;
if (!std::filesystem::is_directory(path))     return ...;
if (!std::filesystem::is_empty(path))         return FAILED_PRECONDITION;
std::filesystem::remove(path);
```

That's four separate stat-family syscalls plus the actual unlink, and
in between any two of them another process can change the directory's
state. The classic TOCTOU window.

**Fix:** call `::rmdir(path.c_str())` exactly once and switch on
errno:

```cpp
if (::rmdir(path.c_str()) == 0) { /* success */ }
else {
    switch (errno) {
        case ENOENT:    return Status(NOT_FOUND, "directory does not exist");
        case ENOTDIR:   return Status(FAILED_PRECONDITION, "not a directory");
        case ENOTEMPTY: return Status(FAILED_PRECONDITION, "directory not empty");
        case EACCES:
        case EPERM:     return Status(PERMISSION_DENIED, ...);
        default:        return Status(INTERNAL, ::strerror(errno));
    }
}
```

**Why it matters:** atomic, fewer syscalls, no TOCTOU window. Also
matches the way every other UNIX tool implements `rmdir`.

#### Fix 3 — `flush_file` double open/close (H3)

**Problem:** the original `flush_file` body wrote each chunk to a
`std::ofstream`, then *after* the stream loop opened the same path
again with `::open(O_WRONLY)` just to call `::fsync`. That's two
opens, two closes, and an unnecessary intermediate buffer flush.

**Fix:** open once with raw POSIX, write each chunk directly, fsync,
close:

```cpp
int fd = -1;
while (reader->Read(&request)) {
    if (fd < 0) {
        // first chunk: parse path, ensure parent exists, open
        filename = request.filename();
        path = request.directory()
             + (request.directory().back() == '/' ? "" : "/") + filename;
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return Status(INTERNAL, ::strerror(errno));
    }
    const std::string& content = request.content();
    if (!content.empty()) ::write(fd, content.data(), content.size());
    client_id = request.client_id();
}
::fsync(fd);
::close(fd);
```

**Why it matters:** halves the syscall count on the durability path,
which is the entire point of `fsync`.

#### Fix 4 — Unused `read_stream_ptr` in `fsync_file`

**Problem:** `fsync_file` captured a `std::ifstream*` from the opened
file's `read_stream`, then never used it, then silenced the unused
warning with `(void) read_stream_ptr;`. Dead code masking itself with
a void cast.

**Fix:** removed both the local and the silencer. The read stream is
not needed because `fsync_file` re-opens its own ifstream for the
upload anyway.

**Why it matters:** dead code that has been deliberately silenced is
worse than dead code with a warning, because it lies to the next
reader about whether it's intentional.

#### Fix 5 — `truncate_file` race condition

**Problem:** the rewritten `truncate_file` was calling
`std::filesystem::resize_file` on the local cache copy with no per-file
mutex held. A concurrent writer thread (going through `write_file`)
could be in the middle of an append, and the resize would clobber its
work — or worse, race with the OS file descriptor.

**Fix:** look up the per-file mutex from `file_mutexes` (under
`cache_mutex`), then take it before calling `resize_file`. Same
pattern that `write_file` uses.

```cpp
std::shared_ptr<std::mutex> file_mtx;
{
    std::lock_guard<std::mutex> global_lock(cache_mutex);
    auto m_it = file_mutexes.find(cache_path);
    if (m_it != file_mutexes.end()) file_mtx = m_it->second;
}
if (file_mtx) {
    std::lock_guard<std::mutex> file_lock(*file_mtx);
    std::error_code ec;
    std::filesystem::resize_file(cache_path, size, ec);
    ...
} else {
    std::error_code ec;
    std::filesystem::resize_file(cache_path, size, ec);
    ...
}
```

**Why it matters:** truncate is rare but it's destructive. A race here
silently corrupts a file under load, which is the worst class of bug
because it's hard to reproduce and hard to attribute.

#### Fix 6 — `truncate_file` TOCTOU pre-check (M3)

**Problem:** the original `truncate_file` did:

```cpp
if (std::filesystem::exists(cache_path, ec)) {
    std::filesystem::resize_file(cache_path, size, ec);
}
```

That's the classic "check then act" TOCTOU pattern: another thread can
delete the file between the `exists` check and the `resize_file` call,
and we get a confusing error from the wrong syscall.

**Fix:** drop the pre-check, call `resize_file` directly, and ignore
`std::errc::no_such_file_or_directory` as a benign outcome:

```cpp
std::error_code ec;
std::filesystem::resize_file(cache_path, size, ec);
if (ec && ec != std::errc::no_such_file_or_directory) {
    std::cerr << "Warning: failed to resize local cache "
              << cache_path << ": " << ec.message() << std::endl;
}
```

**Why it matters:** atomic (one syscall), no TOCTOU, and the error
handling is more honest about what "the file isn't there" actually
means.

### 6.2 Findings that were skipped (and why)

- **Helper extraction for the "directory + (slash) + filename" pattern.**
  This pattern appears ~17 times in the new code and ~30 times in the
  existing code. Extracting it would mean touching files we did not
  otherwise change, which is exactly what the over-engineering rules
  in CLAUDE.md tell us not to do. The boilerplate is also five
  characters per call, which is below the threshold where a helper
  pays for its own indirection.
- **Helper extraction for client unary-RPC boilerplate.** Same reason.
  Each RPC body is ~10 lines, half of which is application logic that
  cannot be factored out cleanly. A "RpcWrapper" template would hide
  the per-RPC error mapping that is the most useful part of each body.
- **Helper extraction for the FUSE wrapper path-splitter pattern.**
  Same reason. The split is two lines and the variation between
  handlers is meaningful (some handlers want the parent directory,
  some want the filename, some want both).
- **`fsync_file` is 80% a clone of `close_file`.** True but
  intentional. The 20% that differs is the part that matters (don't
  erase from `opened_files`, use `flush_file` instead of `close`),
  and unifying the bodies would mean one of them growing a flag
  parameter that branches at every interesting point. That is more
  fragile than two parallel implementations.
- **Pre-existing `client_test` / `register_test` Boost link failure.**
  Not introduced by this work and not fixable without editing
  CMakeLists.txt to add `Boost::boost` to those targets, which is out
  of scope.
- **`flush_file` server silently creates parent dirs.** Reported as a
  quality issue. Decided to keep it because the upload retry loop
  needs to handle the case where a directory was deleted between the
  client cache being written and the upload happening, and silently
  recreating the parent dir is the only behavior that lets retries
  succeed.
- **`chmod` server not masking mode with `& 07777`.** Reported as a
  quality issue. Decided to skip because the kernel already masks the
  mode before issuing the FUSE call, and double-masking would only
  hide bugs in callers that send invalid modes.

### 6.3 Final state after the simplify pass

All four main targets (`afs_server`, `afs_client`, `ci_test_client_1`,
`ci_test_client_2`) build cleanly with no warnings. Six bug fixes
landed on top of the original implementation. The test mount still
behaves correctly under all of the verification commands listed in §4
Phase 7.

---

## 7. What was deliberately *not* changed

It's worth recording the things we considered changing and decided not
to, so future work doesn't have to re-derive the reasoning.

- **`client_subscriber.hpp`** — the existing
  "evict-on-notification" handler covers all metadata-mutating ops
  correctly. Adding op-specific handling would over-engineer it.
- **`get_attributes` uid/gid spoofing** (line ~115) — this is an
  existing design choice that allows local `ls -l` to show the
  current user even when the server stores something else. Not in
  scope to revisit.
- **`ls()` symlink emission** in the server — the current code only
  emits Directory and Regular_File. Adding `Symlink` is a 2-line
  follow-up but `lstat` already lets the kernel see symlinks via
  `getattr`, so functionally everything works without it. Flagged as
  a known gap, not fixed.
- **Recursive `rmdir` flag** — explicitly rejected; see §2.1.
- **In-memory xattr table** — explicitly rejected; see §2.3.
- **`CMakeLists.txt`** — auto-regeneration via `add_custom_command`
  already covers proto changes; nothing else needed editing.

---

## 8. Closing notes

The total diff after both the implementation and the simplify pass:

- 1 proto file: 14 messages + 13 RPCs added
- 1 server header: 13 overrides + 1 callback decl
- 1 server cpp: 7 includes, 2 lstat fixes, 4 xattr helpers, 1 callback
  body, 13 RPC bodies, 3 simplify-pass fixes
- 1 client header: 2 includes + 12 method decls
- 1 client cpp: 3 includes, 12 new method bodies, 1 rewritten method,
  3 simplify-pass fixes
- 1 FUSE cpp: 2 includes, 1 rename, 8 stub replacements, 3 new
  handlers, 1 `afs_oper` rewrite

No new build dependencies. No `CMakeLists.txt` changes. No changes to
the subscriber. No changes to the existing close/open/read/write
fast paths. The work is additive everywhere it can be additive, and
where it had to modify existing code (the two `lstat` lines and the
`truncate_file` body) the change is small and well-justified.
