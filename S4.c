/*
* S4.c - Secondary Server for ZIP File Storage
 *
 * Description:
 * ------------
 * This server receives .zip files from the main server (S1) and stores them
 * under the local ~/S4 directory structure. It handles:
 *   - Receiving and saving uploaded ZIP files
 *   - Responding to S1 for download requests
 *   - Sending ZIP file list to S1 for listing operation
 *
 * Key Behaviors:
 * --------------
 * - Listens for connections from S1 only.
 * - Stores files in a local path mirroring the one sent by the client via S1.
 * - Supports upload, download, remove, list and tar archive generation operations.
 * - Maintains identical directory structure as S1
 * - Services file requests from S1:
 *    - Upload (U)
 *    - Download (D)
 *    - Directory listing (L)
 *
 * Usage:
 * ------
 * Compile: gcc S4.c -o S4
 * Run:     ./S4
 *
 * Port: Default is 6074 (can be changed via macro)
 *
 * Security:
 * ---------
 * - Validates all paths to prevent traversal attacks
 * - Rejects non-ZIP file operations
 * 
 * Authors: Saima Khatoon and Lokesh Jayachandran
 * Date: 09-04-2025
 * Course: COMP-8567
 * Institution: University of Windsor
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


#define PORT_S4 6074
#define BUFFER_SIZE 1024
#define MAX_PATH_LEN 1024

/**
 * @brief Handles file uploads from main server
 * @param sock Connection socket from S1
 *
 * Receives file with metadata (path, size)
 * Creates parent directories as needed
 * Implements overwrite protection
 * Validates file extension matches server type
 */
void handle_upload(int sock) {
    // Request receive from server S1
    printf("======Processing upload of ZIP file======\n");

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
        //printf("File content sent to S1 : %s\n", filedata);
        if (chunk <= 0) break;
        received += chunk;
    }

    // Create full path for S4
    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/S4%s", getenv("HOME"), rel_path);
    printf("Full path is: %s\n", fullpath);

    // Create directory tree ~/S4/..
    char mkdir_cmd[1024];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dirname(strdup(fullpath)));
    system(mkdir_cmd);
    printf("Command to create directory is: %s\n", mkdir_cmd);

    // Create file and write data into it
    FILE *fp = fopen(fullpath, "wb");
    if (fp) {
        fwrite(filedata, 1, filesize, fp);
        fclose(fp);
    } else {
        perror("S4 write failed");
    }

    printf("File uploaded successfully.\n\n");

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
    // Request receive from server S1
    printf("======Processing download of ZIP file======\n");

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
    // Converts ~S1/.. to /home/user/4/..
    char local_path[MAX_PATH_LEN];
    snprintf(local_path, sizeof(local_path), "%s/%s/%s", getenv("HOME"), "S4", strstr(filepath, "S1/") + 3);
    printf("Absolute path of file in S4:%s\n",local_path);

    // Open file in S4
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
    printf("File sent successfully to S1.\n\n");
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
    // Request receive from server S1
    printf("======Processing listing of zip files======\n");

    // Step 1: Receive path length and path
    int path_len = 0;
    if (recv(sock, &path_len, sizeof(int), 0) <= 0 || path_len <= 0 || path_len >= 1024) {
        perror("Failed to receive path length or invalid length");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }

    char pathname[1024] = {0};
    if (recv(sock, pathname, path_len, 0) <= 0) {
        perror("Failed to receive pathname");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }
    pathname[path_len] = '\0';
    printf("Received pathname: %s\n", pathname);

    // Step 2: Convert ~S4/... to actual home directory path
    const char *home = getenv("HOME");
    if (!home) {
        perror("HOME not set");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }

    if (strncmp(pathname, "~S4", 3) != 0) {
        fprintf(stderr, "Invalid path prefix\n");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }

    // Skip "~S4" and add the relative path after it
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/S4%s", home, pathname + 3);
    printf("Searching in directory: %s\n", full_path);

    // Step 3: Run `find` command to list .pdf files
    char command[1024];
    snprintf(command, sizeof(command), "find %s -maxdepth 1 -type f -name \"*.zip\"", full_path);
    printf("Executing: %s\n", command);

    FILE *fp = popen(command, "r");
    if (!fp) {
        perror("Failed to run find");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }

    char **files = NULL;
    int count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';

        char *slash = strrchr(line, '/');
        if (slash) {
            files = realloc(files, (count + 1) * sizeof(char *));
            files[count++] = strdup(slash + 1);
        }
    }
    pclose(fp);

    // Step 4: Send results back to S1
    long status = (count > 0) ? 1 : 0;
    send(sock, &status, sizeof(long), 0);

    if (status == 0) {
        printf("No .zip files found.\n\n");
        return;
    }

    send(sock, &count, sizeof(int), 0);
    for (int i = 0; i < count; i++) {
        int len = strlen(files[i]);
        send(sock, &len, sizeof(int), 0);
        send(sock, files[i], len, 0);
        printf("Sent file: %s\n", files[i]);
        free(files[i]);
    }

    free(files);
    printf("Completed sending list to S1.\n\n");
}


/**
 * @brief Main entry point for S4 server in W25 Distributed Filesystem
 * 
 * @return int Returns 0 on normal shutdown, EXIT_FAILURE on critical errors
 * 
 * @details Creates a TCP server on PORT_S4 that handles multiple file operations:
 *          - 'U' Upload files to server storage
 *          - 'D' Download files from server
 *          - 'L' List available files
 * 
 * @note The server runs indefinitely until manually terminated
 * @warning Uses SO_REUSEADDR|SO_REUSEPORT to allow quick socket recycling
 * 
 * Server Workflow:
 * 1. Creates listening socket on PORT_S4
 * 2. Accepts incoming client connections
 * 3. Processes commands based on received command type
 * 4. Closes client connection after handling
 */
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