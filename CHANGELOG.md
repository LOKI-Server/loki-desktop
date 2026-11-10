# Changelog

## 0.1.0 (unreleased)

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
