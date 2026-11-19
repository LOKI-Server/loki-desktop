# LOKI Client for desktop

The native desktop client for [LOKI](https://github.com/LOKI-Server/LOKI). Browse files, download and update app builds, work with private Git repositories, and keep local folders in step with **Save Sync**. Typical uses:
RetroArch or other emulator saves shared with a handheld, or PC game saves
without cloud sync. Every earlier version stays on your server.

C++20, wxWidgets for the native UI, libcurl for HTTP, and nlohmann/json, built
with [xmake](https://xmake.io).

Releases are built for **Windows (x64)**: download the ZIP, extract it
anywhere, and run `loki-client.exe`. Nothing needs installing; the Microsoft C++
runtime is included. The executable is not code-signed yet, so Windows may show a
SmartScreen prompt the first time (**More info → Run anyway**). The code is
portable to macOS and Linux, but those builds are not tested or released yet.

## Git on desktop

The Windows ZIP includes `loki-client.exe` for the GUI and `loki-git.exe` for
the terminal. Both use the same C++ core for sign-in and Git. Build only the
Git CLI with `xmake f --gui=n` and `xmake build loki-git`; use
`xmake f --gui=y` to build both frontends.

```sh
loki-git auth login https://loki.example.com
loki-git repo list
loki-git repo create my-game --push
loki-git repo clone my-game
```

Approve the pairing code in the LOKI dashboard, or use
`loki-git auth login SERVER --password-stdin USERNAME`. With `--mfa`, provide
the password and MFA code on separate input lines. Both frontends support
creating, viewing, pushing, and cloning repositories. In the GUI, open
the **Git** page and select a repository or publish a local worktree. The page shows the current branch and changes, and supports commits, fetch, fast-forward pull and push.
If an existing GUI login only grants Save Sync and shared files, use
**More → Reconnect to LOKI** to grant Git access for the same account.

Run `repo create --push` from a Git worktree with a commit. It creates a
private repo, adds the `loki` remote, and pushes the current branch and any
LFS assets. Install Git and run `git lfs install` for projects with LFS files.
Use `--remote NAME` for a different local remote or `--branch NAME` for a
different server branch.

The frontends save separate sign-ins and local state. On Windows the GUI uses
`%APPDATA%\LOKI Sync`, and `loki-git` reuses the previous CLI's
`%APPDATA%\LOKI\CLI` directory. Override them independently with
`LOKI_DESKTOP_CONFIG_DIR` and `LOKI_GIT_CONFIG_DIR`. For Git automation, pass
a paired Git API credential through `LOKI_TOKEN` and its server origin through
`LOKI_SERVER`. Server administration is documented separately in the server's
`docs/management-api.md`.

## Using it

1. In the LOKI dashboard, the owner grants **Save sync** to you.
2. Start LOKI Client, enter your server address, and choose **Pair with a code**.
   Approve the code in the dashboard under **Account → Devices → Pair a device**, or
   sign in with a password.
3. Open **Sync**, select a server folder, and choose **Choose local folder…**.

LOKI Client syncs as soon as anything changes: it holds one request open to the server, which answers the moment another device changes a folder, and watches the mapped folders for local saves (waiting until a file has stopped changing). A 5-minute timer and **Sync now** cover anything else. Closing the
window keeps it running in the notification area. On Windows, tick **More →
Start at sign-in** to have it start hidden there each time you sign in (it runs
with `--background`, which you can also use yourself).

Settings and the per-folder index are kept in the user's application data
folder (`%APPDATA%\LOKI Sync` on Windows, retained so the app rename preserves
existing settings and saved logins).

## Browse shared files

Choose **Browse** in the sidebar. Double-click a share or folder to open it,
use **Up** to go back, and use **Previous / Next** for larger folders. Select
a file and choose **Download…**, then pick its destination in the Save As
dialog. Browse uses the desktop account and works for accounts without Save Sync.

Downloads stream to a temporary file, are checked against the server's SHA-256,
then saved to the chosen location. Existing destination files are replaced only
after verification and after the Save As overwrite confirmation. Cancelling or
closing the popup keeps a partial download; choose the same file again to resume.
If the server sends a full response instead of a range, the partial is replaced.
Downloaded files do not create sync mappings or upload anything to the server.
**Account settings** opens the server account page, where **Devices** handles
pairing, renaming, and revocation. Signing out of the app signs out both features.

## Download and update apps

**Apps** lists the app repositories shared with your account and selects the
build for your OS. Choose **Download** to save a package or **Update** to get
the latest build. **Check for updates** compares package checksums, so replacing
a server file in place still shows an update. Packages are saved under the
desktop profile. **Open download folder** opens the selected package's folder.
LOKI verifies downloaded packages and leaves installation and launch to you.
App catalogs require an updated LOKI server.

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

To build the release ZIP (Windows, from PowerShell or a Developer Command Prompt):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

It writes `dist/LOKI-Client-<version>-windows-x64.zip` containing the
GUI and CLI executables, their runtime DLLs, the Visual C++ runtime DLLs,
this README, and the license.

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

## License

MIT © Aspenini. See [LICENSE](LICENSE).
