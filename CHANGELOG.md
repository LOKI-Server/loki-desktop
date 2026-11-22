# Changelog

## Unreleased

- Show who owns each repository, as "owner / name", for LOKI Server 0.9 groups.
- Choose a group to own a new or published repository when the server allows it.
- Mention repository transfers waiting for you; accept or decline them in the LOKI dashboard.

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
