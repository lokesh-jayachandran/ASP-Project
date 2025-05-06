/*
 * S2.c - Secondary Server for PDF File Storage
 *
 * Description:
 * ------------
 * This server receives .pdf files from the main server (S1) and stores them
 * under the local ~/S2 directory structure. It handles:
 *   - Receiving and saving uploaded PDF files
 *   - Responding to S1 for download and delete requests
 *   - Creating TAR files of all .pdf files for downltar command
 *   - Sending pdf file list to S1 for listing operation
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
 *    - Delete (R)
 *    - Download tar (T)
 *    - Directory listing (L)
 *
 * Usage:
 * ------
 * Compile: gcc S2.c -o S2
 * Run:     ./S2
 *
 * Port: Default is 6072 (can be changed via macro)
 *
 * Security:
 * ---------
 * - Validates all paths to prevent traversal attacks
 * - Rejects non-PDF file operations
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
#include <errno.h>
#include <libgen.h>
#include <asm-generic/socket.h>


#define PORT_S2 6072
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
    printf("======Processing upload of PDF file======*\n");

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

    // Create full path for S2
    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/S2%s", getenv("HOME"), rel_path);
    printf("Full path is: %s\n", fullpath);

    // Create directory tree ~/S2/..
    char mkdir_cmd[1024];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dirname(strdup(fullpath)));
    system(mkdir_cmd);
    printf("Command to create directory: %s\n", mkdir_cmd);

    // Create file and write data into it
    FILE *fp = fopen(fullpath, "wb");
    if (fp) {
        fwrite(filedata, 1, filesize, fp);
        fclose(fp);
    } else {
        perror("S2 write failed\n\n");
        printf("S3 write failed\n\n");
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
 * Handles PDF files
 * Implements proper error reporting
 */
void handle_download(int sock) {
    // Request receive from server S1
    printf("======Processing download of PDF file======\n");

    // Receive path length from S1
    int path_len;
    if (recv(sock, &path_len, sizeof(int), 0) != sizeof(int)) {
        perror("Failed to receive path length");
        return;
    }
    printf("File length receive from S1: %d\n", path_len);

    // Receive original path (e.g., "~S1/docs/report.pdf") from S1
    char filepath[MAX_PATH_LEN];
    if (recv(sock, filepath, path_len, 0) != path_len) {
        perror("Failed to receive path");
        return;
    }
    filepath[path_len] = '\0';
    printf("File path receive from S1: %s\n", filepath);

    // Build absolute path with buffer safety
    // Converts ~S2/.. to /home/user/S2/..
    char local_path[MAX_PATH_LEN];
    snprintf(local_path, sizeof(local_path), "%s/%s/%s", getenv("HOME"), "S2", strstr(filepath, "S1/") + 3);
    printf("Absolute path of file in S2: %s\n",local_path);

    // Open file in S2
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
    printf("File sent successfully to S1.\n");
}

/**
 * @brief Processes file deletion requests from S1
 * @param sock The connection socket from S1
 *
 * Validates file path exists
 * Verify path starts with ~S1/
 * Returns success/error message to S1
 * Implements secure path validation
 */
void handle_remove(int sock) {
    // Request receive from server S1
    printf("======Processing remove of PDF file======\n");

    // Receive path from S1 to delete a file
    int path_len;
    if (recv(sock, &path_len, sizeof(int), 0) != sizeof(int)) {
        perror("Failed to receive path length");
        return;
    }
    printf("File length receive from S1: %d\n", path_len);
    
    // Validate path length
    if (path_len <= 0 || path_len >= MAX_PATH_LEN) {
        send(sock, "EInvalid path length", 20, 0);
        return;
    }

    // Receive original path (e.g., "~S1/docs/report.pdf")
    char filepath[MAX_PATH_LEN];
    int bytes_received = recv(sock, filepath, path_len, 0);
    if (bytes_received != path_len) {
        perror("Failed to receive path");
        send(sock, "EPath receive error", 18, 0);
        return;
    }
    filepath[path_len] = '\0';
    printf("File path receive from S1: %s\n", filepath);
    

    /* Security checks */
    // 1. Prevent directory traversal
    if (strstr(filepath, "../") || strstr(filepath, "/..")) {
        send(sock, "EPath traversal not allowed", 26, 0);
        return;
    }

    // 2. Verify path starts with ~S1/
    if (strncmp(filepath, "~S1/", 4) != 0) {
        send(sock, "EPath must start with ~S1/", 25, 0);
        return;
    }
    
    char local_path[MAX_PATH_LEN];
    char *home_dir = NULL;
    const char *s1_part = strstr(filepath, "S1/");

    
    // Build absolute path with buffer safety
    // Converts ~S1/.. to /home/username/S2/..
    home_dir = getenv("HOME");
    snprintf(local_path, MAX_PATH_LEN, "%s/S2/%s", home_dir, s1_part + 3);
    printf("Absolute path of file in S2:%s\n",local_path);

    // Execute deletion
    if (remove(local_path) == 0) {
        send(sock, "SFile deleted successfully", 26, 0);
        printf("SFile deleted successfully.\n\n");
    } else {
        // Provide specific error messages
        switch (errno) {
            case ENOENT:
                printf("EFile not found\n\n");
                send(sock, "EFile not found", 15, 0);
                break;
            case EACCES:
                printf("EPermission denied\n\n");
                send(sock, "EPermission denied", 18, 0);
                break;
            default:
                printf("EFile deletion failed\n\n");
                send(sock, "EFile deletion failed", 21, 0);
        }
    }
}

/**
 * @brief Handles tar archive creation for storage server
 * @param sock The connection socket from S1
 *
 * Receives filetype (pdf)
 * Creates tar of all matching pdf files in server's storage
 * Uses system tar command with grep filtering
 * Streams archive back to S1
 * Cleans up temporary files
 */
void handle_downloadtar(int sock) {
    // Request receive from server S1
    printf("======Processing creation of tar file======\n");

    // Receive filetype length and filetype
    int type_len;
    recv(sock, &type_len, sizeof(int), 0);
    printf("Filetype length receive from S1: %d\n", type_len);
    
    char filetype[10];
    recv(sock, filetype, type_len, 0);
    filetype[type_len] = '\0';
    printf("Filetype receive from S1: %s\n", filetype);

    // Validate filetype matches server's responsibility
    if (strcmp(filetype, "pdf") != 0) {  // S2 only handles PDFs
        long error = -1;
        send(sock, &error, sizeof(long), 0);
        char *err_msg = "EWrong filetype for this server";
        int msg_len = strlen(err_msg);
        send(sock, &msg_len, sizeof(int), 0);
        send(sock, err_msg, msg_len, 0);
        return;
    }

     // First check if S2 directory exists
    struct stat st;
    char s2_dir[256];
    snprintf(s2_dir, sizeof(s2_dir), "%s/S2", getenv("HOME"));
    if (stat(s2_dir, &st) == -1 || !S_ISDIR(st.st_mode)) {
        long error = -1;
        send(sock, &error, sizeof(long), 0);
        char *err_msg = "ES1 directory not found";
        int msg_len = strlen(err_msg);
        send(sock, &msg_len, sizeof(int), 0);
        send(sock, err_msg, msg_len, 0);
        printf("ES1 directory not found.\n");
        return;
    }

    // Check if there are any .pdf files
    char check_cmd[512];
    snprintf(check_cmd, sizeof(check_cmd), 
            "find %s -type f -name '*.pdf' | head -n 1 | grep -q .", s2_dir);
    
    if (system(check_cmd) != 0) {
        long error = -1;
        send(sock, &error, sizeof(long), 0);
        char *err_msg = "ENo .pdf files found in S1 directory";
        int msg_len = strlen(err_msg);
        send(sock, &msg_len, sizeof(int), 0);
        send(sock, err_msg, msg_len, 0);
        printf("ENo .pdf files found in S1 directory.\n");
        return;
    }

    // Create a temporary directory for server files
    // Tar file will be created inside server_tmp directory
    char temp_dir[] = "server_temp";
    char dir_command[1024];
    snprintf(dir_command, sizeof(dir_command), "mkdir %s", temp_dir);
    if (system(dir_command)) {
        send(sock, "ECould not create temp directory", 31, 0);
        return;
    }

    // Construct tar file name
    char tar_filename[256];     // pdffiles.tar for pdf tar file
    snprintf(tar_filename, sizeof(tar_filename), "%s.tar", filetype);

    // Create path for tar file (relative paths)
    char server_tar_path[512];
    snprintf(server_tar_path, sizeof(server_tar_path), "%s/%s", temp_dir, tar_filename);
    printf("Tar file path is: %s\n", server_tar_path);

    // Create file list (relative paths)
    char list_path[512];
    snprintf(list_path, sizeof(list_path), "%s/pdf_files.list", temp_dir);
    printf("List path is: %s\n", list_path);
    
    // Construct command for tar file creation
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "find ~/S2 -type f -name '*.pdf' | sed 's|^.*/S2/||' > %s && tar -C ~/S2 -cf %s -T %s",
            list_path, server_tar_path, list_path);
    printf("Tar command is: %s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        send(sock, "ETar creation failed", 19, 0);
        // Cleanup temp files
        remove(list_path);
        remove(server_tar_path);
        rmdir(temp_dir);
        return;
    }

    // Open tar file
    FILE *tar_file = fopen(server_tar_path, "rb");
    if (!tar_file) {
        long error = -1;
        send(sock, &error, sizeof(long), 0);
        char *err_msg = "ETar file not found";
        int msg_len = strlen(err_msg);
        send(sock, &msg_len, sizeof(int), 0);
        send(sock, err_msg, msg_len, 0);
        return;
    }

    // Move pointer to the end of file to get the size
    fseek(tar_file, 0, SEEK_END);
    long tar_size = ftell(tar_file);
    fseek(tar_file, 0, SEEK_SET);

    // Send status to S1 to proceed for sharing
    long status = 1;
    send(sock, &status, sizeof(long), 0);

    // Send tar file size to S1
    send(sock, &tar_size, sizeof(long), 0);

    // Send tar file content to S1
    char buffer[BUFFER_SIZE];
    while (tar_size > 0) {
        int chunk = tar_size > BUFFER_SIZE ? BUFFER_SIZE : tar_size;
        fread(buffer, 1, chunk, tar_file);
        send(sock, buffer, chunk, 0);
        printf("File content sent to S1 : %s\n", buffer);
        tar_size -= chunk;
    }

    // Close the tar file
    fclose(tar_file);   

    // Cleanup temp files
    remove(list_path);
    remove(server_tar_path);
    rmdir(temp_dir);

    printf("Tar file sent successfully to S1.\n\n");
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
    printf("======Processing listing of pdf files======\n");

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

    // Step 2: Convert ~S2/... to actual home directory path
    const char *home = getenv("HOME");
    if (!home) {
        perror("HOME not set");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }

    if (strncmp(pathname, "~S2", 3) != 0) {
        fprintf(stderr, "Invalid path prefix\n");
        long status = 0;
        send(sock, &status, sizeof(long), 0);
        return;
    }

    // Skip "~S2" and add the relative path after it
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/S2%s", home, pathname + 3);
    printf("Searching in directory: %s\n", full_path);

    // Step 3: Run `find` command to list .pdf files
    char command[1024];
    snprintf(command, sizeof(command), "find %s -maxdepth 1 -type f -name \"*.pdf\"", full_path);
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
        printf("No .pdf files found.\n\n");
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
 * @brief Main entry point for S2 server in W25 Distributed Filesystem
 * 
 * @return int Returns 0 on normal shutdown, EXIT_FAILURE on critical errors
 * 
 * @details Creates a TCP server on PORT_S2 that handles multiple file operations:
 *          - 'U' Upload files to server storage
 *          - 'D' Download files from server
 *          - 'R' Remove files from server
 *          - 'T' Create and send tar bundles
 *          - 'L' List available files
 * 
 * @note The server runs indefinitely until manually terminated
 * @warning Uses SO_REUSEADDR|SO_REUSEPORT to allow quick socket recycling
 * 
 * Server Workflow:
 * 1. Creates listening socket on PORT_S2
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
    address.sin_port = htons(PORT_S2);

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
    printf("🚀  S2 Server is UP and listening on port %d\n", PORT_S2);
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
            case 'R': // Remove
                handle_remove(new_socket);
                break;
            case 'T': // Tar
                handle_downloadtar(new_socket);
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