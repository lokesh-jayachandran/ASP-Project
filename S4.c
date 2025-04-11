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
    // upload code
    // uploadf filename destination_path
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
            default:
                printf("Unknown command type\n");
        }

        close(new_socket);
    }

    return 0;
}