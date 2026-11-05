# LOKI Client for desktop

Keeps folders on a Windows, macOS, or Linux computer in step with a
[LOKI](https://github.com/LOKI-Server/LOKI) server's **Save sync**. Typical uses:
RetroArch or other emulator saves shared with a handheld, or PC game saves
without cloud sync. Every earlier version stays on your server.

C++20, wxWidgets for the native UI, libcurl for HTTP, and nlohmann/json, built
with [xmake](https://xmake.io).

## Using it

1. In the LOKI dashboard, the owner grants **Save sync** to you.
2. Start LOKI Client, enter your server address, and choose **Pair with a code**.
   Approve the code in the dashboard under **Account → Devices → Pair a device**, or
   sign in with a password.
3. Select a server folder and choose **Choose local folder…**.

LOKI Client syncs as soon as anything changes: it holds one request open to the server, which answers the moment another device changes a folder, and watches the mapped folders for local saves (waiting until a file has stopped changing). A 5-minute timer and **Sync now** cover anything else. Closing the
window keeps it running in the notification area. Start it with `--background`
to begin hidden there, for example at sign-in.

Settings and the per-folder index are kept in the user's application data
folder (`%APPDATA%\LOKI Sync` on Windows, retained so the app rename preserves
existing settings and saved logins).

## Browse shared files

Choose **More → Browse files…** in the app menu (also available in the tray menu).
A native popup lists the file shares you can access. Double-click a share or
folder to open it, use **Up** to go back, and use **Previous / Next** for larger
folders. Select a file and choose **Download…**, then pick its destination in
the Save As dialog.

The popup reuses the app's saved LOKI login, including an existing code-paired
login. There is no second sign-in for file browsing. When not signed in, signing
in from the popup also connects the main app. One account device credential is
used for both sync and file access; passwords are not saved. Existing standalone
file credentials are migrated when no main account is saved. Each share's
permissions still apply, and accounts without Save sync can use file browsing.
The server must support LOKI 0.6.0's shared `/auth/v1` login.

Downloads stream to a temporary file, are checked against the server's SHA-256,
then saved to the chosen location. Existing destination files are replaced only
after verification and after the Save As overwrite confirmation. Cancelling or
closing the popup keeps a partial download; choose the same file again to resume.
If the server sends a full response instead of a range, the partial is replaced.
Downloaded files do not create sync mappings or upload anything to the server.
**Account settings** opens the server account page, where **Devices** handles
pairing, renaming, and revocation. Signing out of the app signs out both features.

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
`build/windows/x64/release/loki-client.exe` (or the selected architecture and
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

The shared-session and legacy-login migration checks also run by default. To run the Files API
transfer checks with a local fixture (Python 3, no external server needed):

```bash
python tests/files_fixture.py
```

This covers file browsing, URL encoding, checksum verification, resume, full
responses to range requests, rejected ranges, interrupted transfers, empty files,
cancellation, and revoked/forbidden credentials.
