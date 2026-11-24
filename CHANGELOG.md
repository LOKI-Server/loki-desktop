# Changelog

## 0.5.0

- Download whole folders from the file browser.
- Check for updates to downloaded files and folders on shares that support updates.
- Reuse unchanged bytes in large files, and locate or redownload missing items.

## 0.4.0

- Organize repository controls into **Sync**, **Branches**, and **Working changes**.
- Manage local branches, check out fetched branches, and see whether the current branch is up to date, ahead, behind, or diverged. Publish new branches from the desktop client.
- Browse commit history, preview staged and unstaged changes, and select which files to commit.
- Compare and merge branches, with conflict status and an option to abort a pending merge.

## 0.3.0

- Refresh the window: a native sidebar with the LOKI dashboard's page icons and the signed-in account at the bottom, page headers with each page's main actions, and buttons that only appear when they apply.
- Replace the single **More** menu with **File**, **View**, **Account**, and **Help**. **View** switches pages with Ctrl+1 to Ctrl+4, and **Help → About LOKI Client** shows the version.
- The sidebar's **Account** button opens LOKI in your browser, reconnects, or signs out.
- Center the connect screen, and sign in by pressing Enter in the password or code field.
- Show errors in red and completed downloads in green, wrapped to the window width.
- Open Save sync first when the account can sync.
- Ask before you stop syncing a folder on this computer.
- Show repositories as clone or link actions until they are on this computer, then as commit, pull, push, and fetch.
- Hide the file browser's Previous and Next buttons unless a folder has more than one page.
- Show who owns each repository, as "owner / name", for LOKI Server 0.9 groups.
- Choose a group to own a new or published repository when the server allows it.
- Mention repository transfers waiting for you; accept or decline them in the LOKI dashboard.
- Update Skald to 0.2.0, so a refused push shows the server's reason, such as a repository waiting for a transfer.

## 0.2.1

- Show an animated progress bar while repository operations run.
- Explain server storage errors when a repository push fails and show where to check quotas.

## 0.2.0

- Added the Skald-backed **Repositories** page for use with LOKI Server 0.8.0.
- Added automatic Windows client update checks and **More → Check for client updates**.

## 0.1.0

First release of LOKI Client for desktop (Windows x64).

- Two-way **Save sync** of server folders with folders on this computer.
- Pair with a short code approved in the LOKI dashboard, or sign in with a
  password and optional two-factor code.
- Syncs as soon as anything changes (server long-poll and a file watcher), with a
  5-minute fallback; keeps running in the notification area when closed.
- **More → Start at sign-in** starts it hidden in the notification area.
- Conflicts never lose data: the server keeps every displaced version, and files
  this computer never synced are stashed rather than overwriting newer saves.
- **Browse files**: browse file shares and download files with resume and
  SHA-256 verification, using the same account sign-in.
- Warns before signing in to a plain-HTTP server address.
- Portable ZIP with the Microsoft C++ runtime included; nothing to install.
- Requires LOKI Server 0.6.0 or newer (shared `/auth/v1` sign-in).
