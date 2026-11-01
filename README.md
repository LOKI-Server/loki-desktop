# LOKI Sync for desktop

Keeps folders on a Windows, macOS, or Linux computer in step with a
[LOKI](https://github.com/Aspenini/LOKI) server's **Save sync**. Typical uses:
RetroArch or other emulator saves shared with a handheld, or PC game saves
without cloud sync. Every earlier version stays on your server.

C++20, wxWidgets for the native UI, libcurl for HTTP, and nlohmann/json, built
with [xmake](https://xmake.io).

## Using it

1. In the LOKI dashboard, the owner grants **Save sync** to you.
2. Start LOKI Sync, enter your server address, and choose **Pair with a code**.
   Approve the code in the dashboard under **Save sync → Pair a device**, or
   sign in with a password.
3. Select a server folder and choose **Choose local folder…**.

LOKI Sync syncs as soon as anything changes: it holds one request open to the server, which answers the moment another device changes a folder, and watches the mapped folders for local saves (waiting until a file has stopped changing). A 5-minute timer and **Sync now** cover anything else. Closing the
window keeps it running in the notification area. Start it with `--background`
to begin hidden there, for example at sign-in.

Settings and the per-folder index are kept in the user's application data
folder (`%APPDATA%\LOKI Sync` on Windows).

## How syncing works

The engine (`src/engine.cpp`) is shared with no GUI code. It runs in three
steps:

- **Scan.** Only files whose size or modification time changed are hashed
  (SHA-256). Files changed in the last 3 seconds wait for the next pass.
- **Pull.** The server's change feed is applied. Downloads land in a temporary
  file, are verified against their hash, and replace the local file with one
  rename.
- **Push.** New content is uploaded and committed against the last-seen
  version.
  - When both sides changed a file this computer has synced before, the local
    edit wins. The server keeps the other version as a conflict.
  - Files this computer never synced are *stashed*, so a stale folder never
    replaces newer saves.

## Building

```bash
xmake
```

```bash
xmake run loki-sync
```

On Windows this uses MSVC and prebuilt wxWidgets. Run it from PowerShell or a
Developer Command Prompt rather than an MSYS shell, which mangles CMake paths
while libcurl builds.

On Windows the build copies the wxWidgets runtime DLLs beside
`build/windows/x64/release/loki-sync.exe` (or the selected architecture and
mode). You can launch it directly from Explorer. When moving the application
to another folder, keep those DLLs alongside the executable.

The Windows application supports per-monitor DPI scaling, including moving
the window between displays with different scale settings.

## Tests

```bash
xmake build loki-sync-tests
```

```bash
xmake run loki-sync-tests
```

The hashing and index tests always run. The two-computer test syncs real files
through a LOKI server, covering conflicts, stashing, deletions, and settling. It
runs only when `LOKI_TEST_SERVER`, `LOKI_TEST_USER`, and `LOKI_TEST_PASSWORD`
are set, and it creates a test folder on that server.
