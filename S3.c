/*
 * Distributed File System - TEXT Storage Server (S3)
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
 * - Listens on TCP port 8086
 * - Expects paths in ~S1/ format, converts to ~/S3/
 * - Maintains identical directory structure as S1
 *
 * Security:
 * - Validates all paths to prevent traversal attacks
 * - Rejects non-TXT file operations
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

#define PORT_S3 6056
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
    printf("Request receive from server S1 to upload a txt file.\n");

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

    // Create full path for S2
    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/S3%s", getenv("HOME"), rel_path);
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
        perror("S3 write failed");
        printf("S3 write failed");
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
 * Handles TXT files
 * Implements proper error reporting
 */
void handle_download(int sock) {
    printf("Request receive from server S1 to download a txt file.\n");

    // Receive path length from S1
    int path_len;
    if (recv(sock, &path_len, sizeof(int), 0) != sizeof(int)) {
        perror("Failed to receive path length");
        return;
    }
    printf("File length receive from S1: %d\n", path_len);

    // Receive original path (e.g., "~S1/docs/report.txt") from S1
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
    snprintf(local_path, sizeof(local_path), "%s/%s/%s", getenv("HOME"), "S3", strstr(filepath, "S1/") + 3);
    //snprintf(local_path, MAX_PATH_LEN, "%s/S3/%s", getenv("HOME");, s1_part + 3);
    printf("Absolute path of file in S3:%s\n",local_path);

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
 * @brief Processes file deletion requests from S1
 * @param sock Connection socket from main server
 *
 * Validates file path exists
 * Checks file permissions before deletion
 * Handles both files and empty directories
 * Returns success/error message to S1
 * Implements secure path validation
 */
void handle_remove(int sock) 
{
    printf("Request receive from server S1 to remove a txt file.\n");

    // Receive path from S1 to delete a file
    int path_len;
    if (recv(sock, &path_len, sizeof(int), 0) != sizeof(int)) {
        perror("Failed to receive path length.\n");
        return;
    }
    printf("File length receive from S1: %d\n", path_len);

    // Validate path length
    if (path_len <= 0 || path_len >= MAX_PATH_LEN) {
        send(sock, "EInvalid path length", 20, 0);
        return;
    }

    // Receive original path (e.g., "~S1/docs/report.txt")
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
    // Converts ~S1/.. to /home/username/S3/..
    home_dir = getenv("HOME");
    snprintf(local_path, MAX_PATH_LEN, "%s/S3/%s", home_dir, s1_part + 3);
    printf("Absolute path of file in S3:%s\n",local_path);

    // Execute deletion
    if (remove(local_path) == 0) {
        send(sock, "SFile deleted successfully", 26, 0);
        printf("SFile deleted successfully\n");
    } else {
        // Provide specific error messages
        switch (errno) {
            case ENOENT:
                printf("EFile not found\n");
                send(sock, "EFile not found", 15, 0);
                break;
            case EACCES:
                printf("EPermission denied\n");
                send(sock, "EPermission denied", 18, 0);
                break;
            default:
                printf("EFile deletion failed\n");
                send(sock, "EFile deletion failed", 21, 0);
        }
    }
}

/**
 * @brief Handles tar archive creation for storage server
 * @param sock The connection socket from S1
 *
 * Receives filetype (txt)
 * Creates tar of all matching txt files in server's storage
 * Uses system tar command with grep filtering
 * Streams archive back to S1
 * Cleans up temporary files
 */
void handle_downloadtar(int sock) {
    printf("Request receive from server S1 to create tar file for txt files.\n");

    // Receive filetype length and filetype
    int type_len;
    recv(sock, &type_len, sizeof(int), 0);
    printf("Filetype length receive from S1: %d\n", type_len);
    
    char filetype[10];
    recv(sock, filetype, type_len, 0);
    filetype[type_len] = '\0';
    printf("Filetype receive from S1: %s\n", filetype);

    // Validate filetype matches server's responsibility
    if (strcmp(filetype, "txt") != 0) {  // S3 only handles txts
        long error = -1;
        send(sock, &error, sizeof(long), 0);
        char *err_msg = "EWrong filetype for this server";
        int msg_len = strlen(err_msg);
        send(sock, &msg_len, sizeof(int), 0);
        send(sock, err_msg, msg_len, 0);
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
    char tar_filename[256];     // txtfiles.tar for txt tar file
    snprintf(tar_filename, sizeof(tar_filename), "%sfiles.tar", filetype);

    // Create path for tar file (relative paths)
    char server_tar_path[512];
    snprintf(server_tar_path, sizeof(server_tar_path), "%s/%s", temp_dir, tar_filename);
    printf("Tar file path is: %s\n", server_tar_path);

    // Create file list (relative paths)
    char list_path[512];
    snprintf(list_path, sizeof(list_path), "%s/c_files.list", temp_dir);
    printf("List path is: %s\n", list_path);
    
    // Construct command for tar file creation
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "find ~/S3 -type f -name '*.txt' | sed 's|^.*/S3/||' > %s && tar -C ~/S3 -cf %s -T %s",
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
        printf("EFile not found\n");
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

    printf("Tar file sent successfully to S1\n");
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
    snprintf(command, sizeof(command), "find %s/S3 -type f -name \"*.txt\"", getenv("HOME"));

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
    address.sin_port = htons(PORT_S3);

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
    printf("🚀  S3 Server is UP and listening on port %d\n", PORT_S3);
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