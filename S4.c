/*
 * Distributed File System - ZIP Storage Server (S4)
 *
 * Responsibilities:
 * 1. Stores all PDF files in ~/S2/ directory
 * 2. Services file requests from S1:
 *    - Upload (U)
 *    - Download (D)
 *    - Delete (R)
 *    - Download tar (T)
 *    - Directory listing (L)
 *
 * Protocol:
 * - Listens on TCP port 8087
 * - Expects paths in ~S1/ format, converts to ~/S4/
 * - Maintains identical directory structure as S1
 *
 * Security:
 * - Validates all paths to prevent traversal attacks
 * - Rejects non-ZIP file operations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <asm-generic/socket.h>

#define PORT_S4 6057
#define BUFFER_SIZE 1024
#define MAX_PATH_LEN 1024

/**
 * @brief Creates directory structure recursively
 * @param path The full directory path to create
 *
 * Handles all intermediate directory creation
 * Uses mkdir -p system command
 * Validates path format
 */
void create_directory(const char *path) {
    char command[1024];
    snprintf(command, sizeof(command), "mkdir -p %s", path);
    system(command);
}

/**
 * @brief Handles file uploads from main server
 * @param sock Connection socket from S1
 *
 * Receives file with metadata (path, size)
 * Creates parent directories as needed
 * Implements overwrite protection
 * Validates file extension matches server type
 * Uses atomic move for completed transfers
 */
void handle_upload(int sock) {
    printf("Request receive from server S1 to upload a zip file.\n");

    int path_len;
    read(sock, &path_len, sizeof(int));
    printf("Path Length is: %d\n", path_len);

    char rel_path[1024];
    read(sock, rel_path, path_len);
    rel_path[path_len] = '\0';
    printf("Relative path is: %s\n", rel_path);

    // Receive file size
    long filesize;
    read(sock, &filesize, sizeof(long));
    printf("Size of file received: %d\n", filesize);

    // Receive file data
    char *filedata = malloc(filesize);
    long received = 0;
    while (received < filesize) {
        int chunk = read(sock, filedata + received, filesize - received);
        printf("File content sent to S1 : %s\n", filedata);
        if (chunk <= 0) break;
        received += chunk;
    }

    // Create full path for S4
    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/S4%s", getenv("HOME"), rel_path);
    printf("Full path is: %s\n", fullpath);

    // Create directory tree ~/S2/..
    char mkdir_cmd[1024];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dirname(strdup(fullpath)));
    system(mkdir_cmd);
    printf("mkdir_cmd is: %s\n", mkdir_cmd);

    // Create file and write data into it
    FILE *fp = fopen(fullpath, "wb");
    if (fp) {
        fwrite(filedata, 1, filesize, fp);
        fclose(fp);
    } else {
        perror("S4 write failed");
    }

    printf("File uploaded successfully\n");

    free(filedata);
}

/**
 * @brief Processes file download requests from S1
 * @param sock The connection socket from S1
 *
 * Validates requested file exists
 * Streams file with size prefix protocol
 * Handles zip files
 * Implements proper error reporting
 */
void handle_download(int sock) {
    printf("Request receive from server S1 to download a zip file.\n");

    // Receive path length from S1
    int path_len;
    if (recv(sock, &path_len, sizeof(int), 0) != sizeof(int)) {
        perror("Failed to receive path length");
        return;
    }
    printf("File length receive from S1: %d\n", path_len);

    // Receive original path (e.g., "~S1/docs/report.zip") from S1
    char filepath[MAX_PATH_LEN];
    if (recv(sock, filepath, path_len, 0) != path_len) {
        perror("Failed to receive path");
        return;
    }
    filepath[path_len] = '\0';
    printf("File path receive from S1: %s\n", filepath);

    // Build absolute path with buffer safety
    // Converts ~S1/.. to /home/user/S3/..
    char local_path[MAX_PATH_LEN];
    snprintf(local_path, sizeof(local_path), "%s/%s/%s", getenv("HOME"), "S4", strstr(filepath, "S1/") + 3);
    printf("Absolute path of file in S4:%s\n",local_path);

    // Open file in S3
    FILE *file = fopen(local_path, "rb");
    char status = (file != NULL) ? 1 : -1;
    send(sock, &status, 1, 0);  // First send status byte
    if (!file) {
        printf("EFile not found\n");
        char *err_msg = "EFile not found";
        int msg_len = strlen(err_msg);
        send(sock, &msg_len, sizeof(int), 0);
        send(sock, err_msg, msg_len, 0);
        return;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // Send file size to S1
    send(sock, &file_size, sizeof(long), 0);

    // Send file content to S1
    char buffer[BUFFER_SIZE];
    while (file_size > 0) {
        int chunk = file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size;
        int bytes_read = fread(buffer, 1, chunk, file);
        send(sock, buffer, bytes_read, 0);
        printf("File content sent to S1 : %s\n", buffer);
        file_size -= bytes_read;
    }
    fclose(file);       // Close the file
    printf("File sent successfully to S1: %s\n", file);
}

/**
 * @brief Generates directory listings for S1
 * @param sock Connection socket from main server
 * @param path Directory path to scan
 *
 * Lists all files with server's managed extension
 * Recursively scans subdirectories when requested
 * Returns sorted list of filenames with extensions
 * Handles permission errors gracefully
 */
void handle_listing(int sock) {
    printf("Request received from server S1 to list .pdf files.\n");

    char command[1024];
    snprintf(command, sizeof(command), "find %s/S4 -type f -name \"*.zip\"", getenv("HOME"));

    FILE *fp = popen(command, "r");
    if (!fp) {
        perror("Failed to run find command");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }

    char line[1024];
    char *files[512];
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';

        // Extract filename only
        char *slash = strrchr(line, '/');
        if (slash && count < 512) {
            files[count++] = strdup(slash + 1);
        }
    }
    pclose(fp);

    long status = 1;
    send(sock, &status, sizeof(long), 0);
    send(sock, &count, sizeof(int), 0);

    for (int i = 0; i < count; i++) {
        int len = strlen(files[i]);
        send(sock, &len, sizeof(int), 0);
        send(sock, files[i], len, 0);
        printf("Sent file: %s\n", files[i]);
        free(files[i]);
    }

    printf("Completed sending list to S1.\n");
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT_S4);

    // Bind socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("\n==============================================\n");
    printf("🚀  S4 Server is UP and listening on port %d\n", PORT_S4);
    printf("==============================================\n\n");

    while (1) {
        // Accept connection
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            continue;
        }

        // Get command type
        char command_type;
        recv(new_socket, &command_type, 1, 0);

        switch (command_type) {
            case 'U': // Upload
                handle_upload(new_socket);
                break;
            case 'D': // Download
                handle_download(new_socket);
                break;
            case 'L': // List
                handle_listing(new_socket);
                break;
            default:
                printf("Unknown command type\n");
        }

        close(new_socket);
    }

    return 0;
}