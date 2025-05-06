# Distributed File Server System

## Description

This project is a distributed file system implemented in C using UNIX sockets. It consists of four server nodes (`S1`, `S2`, `S3`, `S4`) and a client interface (`w25clients`). Clients interact only with `S1`, which acts as a proxy and file router. While users think files are stored and retrieved directly from `S1`, `S1` transparently delegates storage and retrieval based on file type:

- `.c` files are stored on `S1`
- `.pdf` files are routed to `S2`
- `.txt` files go to `S3`
- `.zip` files are sent to `S4`

The system is designed for multiple concurrent clients, each serviced by separate processes forked by `S1`. The client communicates using defined commands such as `uploadf`, `downlf`, `removef`, `downltar`, and `dispfnames`.

## Techniques Used

- **UNIX Socket Programming**: The core communication uses [POSIX sockets](https://man7.org/linux/man-pages/man2/socket.2.html) for TCP communication between clients and servers.
- **Process Forking**: `S1` uses [`fork()`](https://man7.org/linux/man-pages/man2/fork.2.html) to handle concurrent client sessions, ensuring isolation and responsiveness.
- **Command Parsing**: The client parses custom commands and performs syntax validation before interacting with the server.
- **Transparent Backend Routing**: `S1` functions as a router — forwarding files to `S2`, `S3`, or `S4` depending on type. This abstraction layer hides backend complexity from clients.
- **Recursive File Discovery and Archiving**: Implements recursive directory traversal and creates file-type-specific `.tar` archives using system utilities.
- **Signal Handling**: Server processes use [signal handling](https://man7.org/linux/man-pages/man2/signal.2.html) for robustness and graceful termination.

## Project Structure

```
ASP-Project/
├── S1.c
├── S2.c
├── S3.c
├── S4.c
├── w25clients.c
```

### File Overview

- [`S1.c`](./S1.c): Primary server. Handles client connections, routes file operations, and stores `.c` files locally in `~/S1`.
- [`S2.c`](./S2.c): Handles file storage and retrieval for `.pdf` files in `~/S2`. Communicates only with `S1`.
- [`S3.c`](./S3.c): Handles `.txt` files, with all storage under `~/S3`.
- [`S4.c`](./S4.c): Responsible for `.zip` files, stored under `~/S4`.
- [`w25clients.c`](./w25clients.c): Client-side interface. Parses user commands, verifies syntax, and communicates exclusively with `S1`.

## Supported Commands

These are implemented within [`w25clients.c`](./w25clients.c):

- `uploadf <filename> <destination_path>`: Uploads a file to S1, which then stores or delegates based on file type.
- `downlf <filepath>`: Downloads a file from the distributed system. Files are fetched via `S1`, regardless of where they are stored.
- `removef <filepath>`: Deletes a file from the distributed system via `S1`.
- `downltar <filetype>`: Downloads a `.tar` archive of all files of the specified type (`.c`, `.txt`, or `.pdf`) from the appropriate server.
- `dispfnames <directory_path>`: Displays filenames from a specific path in the distributed system. Aggregates results from `S1` through `S4`, sorted by type and name.

## Design Summary

- Clients never know that `S2`, `S3`, and `S4` exist. All commands go through `S1`.
- The system supports multiple concurrent clients using child processes.
- Files are organized in per-server home directories:
  - `~/S1`, `~/S2`, `~/S3`, `~/S4`
- Files are routed based on extension, with internal forwarding implemented in `S1`.
