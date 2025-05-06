/*
 * S1.c - Main Server for Distributed File System 
 *
 * Description:
 * ------------
 * This program implements the main server (S1) in a distributed file system setup.
 * It accepts connections from multiple clients and handles file operations like:
 *   - Handling uploads, downloads, deletions, list generation and TAR creation requests of .c files locally
 *   - Forwarding files  (.pdf, .txt, .zip) to secondary servers (S2, S3, S4) based on file type
 * 
 * Key Behaviors:
 * --------------
 * - S1 handles client connections via forked child processes.
 * - Only .c files are stored locally; other types are transferred to:
 *      .pdf → S2, .txt → S3, .zip → S4
 * - Clients are unaware of S2/S3/S4 and interact only with S1.
 * - S1 ensures proper routing, error checking, and communication between components.
 * - Maintains transparency - clients only see ~S1/ paths
 * - Uses 'U'pload, 'D'ownload, 'R'emove , Download 'T'ar, Directory 'L'isting commands
 *
 * Usage:
 * ------
 * Compile: gcc S1.c -o S1
 * Run:     ./S1
 *
 * Port: Default is 6071 (can be changed via macro)
 *
 * Client Command Received:
 * ------------------------
 * 
 * - uploadf: Upload files to distributed storage
 * - downlf: Download files from server
 * - removef: Delete remote files
 * - downltar: Download tar of file type
 * - dispfnames: List directory contents
 * 
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
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <libgen.h>
#include <asm-generic/socket.h>


#define PORT_S1 6071
#define PORT_S2 6072
#define PORT_S3 6073
#define PORT_S4 6074
#define BUFFER_SIZE 1024
#define MAX_PATH_LEN 1024

/**
 * @brief Establishes connection to a target storage server
 * @param target_port Port number of the target server
 * @param client_sock The client socket descriptor to associate with logging
 * @return Socket descriptor on success, -1 on failure
 *
 * Creates TCP socket and connects to specified storage server
 * Logs connection errors to stderr
 * Attempts connection to 127.0.0.1:[target_port]
 * Used for all inter-server communications (S2/S3/S4)
 */
int connect_to_target_server(int target_port, int client_sock) {
    // Connect to target server
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        send(client_sock, "EInternal server error", 22, 0);
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));   // Initialize serv_addr to zero
    serv_addr.sin_family = AF_INET;             // Set address family to IPv4
    serv_addr.sin_port = htons(target_port);    // Set the target port, converting to network byte order

    // Convert IP address from text to binary form and store in serv_addr
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);   

    // Attempt to connect to the target storage server
    if (connect(server_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        close(server_sock);
        long status = -1;
        send(client_sock, &status, sizeof(long), 0);
        // Close the socket and notify the client about the failure
        const char *msg = "EConnection is not reliable";
        int msg_len = strlen(msg);
        send(client_sock, &msg_len, sizeof(int), 0);
        send(client_sock, msg, msg_len, 0);
        return -1;
    }

    return server_sock;  // Success
}

/**
 * @brief Sends a file to a target server via TCP connection
 * 
 * @param ip Target server IP address (S2/S3/S4)
 * @param port Target server port number
 * @param filedata File content buffer to send
 * @param filesize Size of file data in bytes
 * @param relative_dest_path Destination path on server (relative)
 * @return int 1 on success, -1 on failure
 * 
 * @details Implements protocol:
 * 'U' - Upload File
 *   1. S1 → Storage: 'U' + path_len + path + file_size + file_data
 *   2. Storage → S1: Success/Failure response
 */
int send_file_to_server(const char *ip, int port, const char *filedata, long filesize, const char *relative_dest_path) {
    int sock;
    struct sockaddr_in server;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Socket creation failed");
        return -1;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }

    printf("Server connected\n");

    // Send download command ('D') to target server
    char command_type = 'U';
    send(sock, &command_type, 1, 0);

    // Send path length and relative destination path to target server
    int path_len = strlen(relative_dest_path);
    write(sock, &path_len, sizeof(int));
    printf("Path Length is: %d\n", path_len);

    write(sock, relative_dest_path, path_len);
    printf("Relative path is: %s\n", relative_dest_path);

    // Send file size to target server
    write(sock, &filesize, sizeof(long));

    // Send file data in chunks to target server
    long total_sent = 0;
    while (total_sent < filesize) {
        int chunk = write(sock, filedata + total_sent, filesize - total_sent);
        printf("File content sent to client : %s\n", filedata);
        if (chunk <= 0) break;
        total_sent += chunk;
    }

    close(sock);
    return total_sent == filesize ? 1 : -1;  // Return success or failure
}

/**
 * @brief Processes file upload requests from clients
 * @param client_sock Client socket descriptor
 * @param filename Name of file being uploaded
 * @param dest_path Destination path on server (~S1/...)
 *
 * Handles all file types (.c, .pdf, .txt, .zip)
 * Stores .c files locally, routes others to appropriate servers
 * Creates necessary directory structure
 * Validates file extensions and path formats
 * Implements atomic write operation
 */
void handle_upload_request(int client_sock, const char *filename, const char *dest_path){
        // Receive file size
        long size;
        read(client_sock, &size, sizeof(long));
        printf("Size of file received: %d\n", size);

        // Receive file data
        char *filedata = malloc(size);
        long received = 0;
        while (received < size) {
            int chunk = read(client_sock, filedata + received, size - received);
            //printf("File content receive from client : %s\n", filedata);
            if (chunk <= 0) break;
            received += chunk;
        }

        // Handle file type and destination
        char *ext = strrchr(filename, '.');
        char moddest[1024];
        if (strncmp(dest_path, "~S1", 3) == 0)
            snprintf(moddest, sizeof(moddest), "%s/%s", dest_path + 3, filename);
        else
            snprintf(moddest, sizeof(moddest), "%s/%s", dest_path, filename);

        int result = 0;  // Variable to store the result status (success/failure)

        //printf("moddest is: %s\n", moddest);

        // Determine where to send the file based on its extension
        if (ext && strcmp(ext, ".pdf") == 0) {
            // Send to S2 server
            result = send_file_to_server("127.0.0.1", PORT_S2, filedata, size, moddest);
        } else if (ext && strcmp(ext, ".txt") == 0) {
            // Send to S3 server
            result = send_file_to_server("127.0.0.1", PORT_S3, filedata, size, moddest);
        } else if (ext && strcmp(ext, ".zip") == 0) {
            // Send to S4 server 
            result = send_file_to_server("127.0.0.1", PORT_S4, filedata, size, moddest);
        } else if (ext && strcmp(ext, ".c") == 0) {
            // Save locally to ~/S1
            char fullpath[1024];
            snprintf(fullpath, sizeof(fullpath), "%s/S1%s/%s", getenv("HOME"), dest_path + 3, filename);
            char mkdir_cmd[1024];
            printf("Full path is: %s\n", fullpath);

            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dirname(strdup(fullpath)));
            system(mkdir_cmd);
            printf("Command to create directory is: %s\n", mkdir_cmd);

            FILE *fp = fopen(fullpath, "wb");
            if (fp) {
                fwrite(filedata, 1, size, fp);
                fclose(fp);
                result = 1;  // Success
            } else {
                perror("Write error on .c file");
                result = -1;  // Failure
            }
        }

        // Send a response to the client indicating success or failure
        if (result == 1) {
            // Success: File processing was completed successfully
            const char *response = "File uploaded successfully.";
            write(client_sock, response, strlen(response) + 1);
            printf("%s\n",response);
        } else if (result == -1) {
            // Failure: There was an issue with the file handling
            const char *response = "Error processing the file.";
            write(client_sock, response, strlen(response) + 1);
            printf("%s\n",response);
        } else {
            // Failure: Could not connect to the appropriate server
            const char *response = "Error sending file to destination server.";
            write(client_sock, response, strlen(response) + 1);
            printf("%s\n",response);
        }

        free(filedata);
}


/**
 * @brief Processes file download requests
 * @param client_sock The client socket descriptor 
 * @param filepath The full client path (~S1/...)
 *
 * Handles .c files locally, routes others to storage servers
 * Validates paths and file existence
 * Streams files with size prefix protocol
 * 
 * @details Implements protocol:
 * 'D' - Download File  
 *   1. S1 → Storage: 'D' + path_len + path
 *   2. Storage → S1: file_size + file_data OR error
 */
void handle_download_request(int client_sock, const char *filepath) {

    // Validate input
    if (!filepath || strlen(filepath) == 0) {
        long error = -1;
        send(client_sock, &error, sizeof(long), 0);
        char *err_msg = "EEmpty file path";
        int msg_len = strlen(err_msg);
        send(client_sock, &msg_len, sizeof(int), 0);
        send(client_sock, err_msg, msg_len, 0);
        return;
    }

    // Extract file extension
    const char *ext = strrchr(filepath, '.');
    if (!ext) {
        long error = -1;
        send(client_sock, &error, sizeof(long), 0);
        char *err_msg = "EInvalid file: no extension";
        int msg_len = strlen(err_msg);
        send(client_sock, &msg_len, sizeof(int), 0);
        send(client_sock, err_msg, msg_len, 0);
        return;
    }
    ext++; // Move past the dot
    printf("Extension is: %s\n", ext);

    // If the received file has a ".c" extension, handle it locally
    if (strcmp(ext, "c") == 0) {

        // Handle .c file locally
        char local_path[MAX_PATH_LEN];
        char *home_dir = NULL;
        const char *s1_part = strstr(filepath, "S1/");
        if (!s1_part) {
            long error = -1;
            send(client_sock, &error, sizeof(long), 0);
            char *err_msg = "EInvalid path format";
            int msg_len = strlen(err_msg);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, err_msg, msg_len, 0);
            return;
        }

        // Build absolute path with buffer safety
        // Converts ~S1/.. to /home/user/S1/..
        home_dir = getenv("HOME");
        snprintf(local_path, MAX_PATH_LEN, "%s/S1/%s", home_dir, s1_part + 3);   // Converts ~S1/ to /home/user/S1/
        printf("Absolute path of file in S1: %s\n",local_path);

        // Open file in S1
        FILE *file = fopen(local_path, "rb");
        if (!file) {
            //printf("EFile not found\n");
            // File not found: send a size of -1
            long error = -1;
            send(client_sock, &error, sizeof(long), 0);
            char *err_msg = "EFile not found";
            int msg_len = strlen(err_msg);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, err_msg, msg_len, 0);
            return;
        }
       
        // Get file size
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        
        long status = 1;
        // Send status to client to proceed for download
        send(client_sock, &status, sizeof(long), 0); 
        // Send file size to client   
        send(client_sock, &file_size, sizeof(long), 0);

        // Send file content to client
        char buffer[BUFFER_SIZE];
        while (file_size > 0) {
            int chunk_size = file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size;
            fread(buffer, 1, chunk_size, file);
            send(client_sock, buffer, chunk_size, 0);
            printf("File content sent to client : %s\n", buffer);
            file_size -= chunk_size;
        }

        fclose(file);   // Close the file
        printf("File sent successfully to client.\n");
        return;
    }

    // If the received file has an extension other than ".c", send it to another server
    // Select appropriate target server for non-.c files
    int target_port;
    char server_prefix[10];
    
    // If the received file has a ".pdf" extension
    if (strcmp(ext, "pdf") == 0) {
        target_port = PORT_S2;
    } 
    // If the received file has a ".txt" extension
    else if (strcmp(ext, "txt") == 0) {
        target_port = PORT_S3;
    }
    // If the received file has a ".zip" extension
    else if (strcmp(ext, "zip") == 0) {
        target_port = PORT_S4;
    }
    // If the received file has any other extension
    else {
        send(client_sock, "EUnsupported file type", 22, 0);
        return;
    }

    int server_sock = connect_to_target_server(target_port, client_sock);
    if (server_sock < 0) {
        return;  // Error already handled
    }
    printf("Server Connected.\n");

    // Send download command ('D') to target server
    char command_type = 'D';
    send(server_sock, &command_type, 1, 0);

    // If connection is successful to target server
    // Send status to client to proceed
    long status = 1;
    send(client_sock, &status, sizeof(long), 0);

    // Send original path (unchanged) e.g.: ~S1/1.txt
    int path_len = strlen(filepath);
    send(server_sock, &path_len, sizeof(int), 0);
    send(server_sock, filepath, path_len, 0);

    // Wait for status byte from target server
    // If file is present in target server, only then proceed
    char status1;
    recv(server_sock, &status1, 1, 0);

    if (status1 == -1) {
        //printf("EFile not found\n");
        int msg_len;
        recv(server_sock, &msg_len, sizeof(int), 0);
        char error_msg[BUFFER_SIZE];
        recv(server_sock, error_msg, msg_len, 0);
        error_msg[msg_len] = '\0';
        printf("Error: %s\n", error_msg);
        // File not found: send a size of -1
        long error = -1;
        send(client_sock, &error, sizeof(long), 0);
        //send(client_sock, msg, strlen(msg), 0);
        send(client_sock, &msg_len, sizeof(int), 0);
        send(client_sock, error_msg, msg_len, 0);
        close(server_sock);
        return;
    }

    // Receive file size from target server
    long file_size;
    recv(server_sock, &file_size, sizeof(long), 0);

    // Send file size to client
    send(client_sock, &file_size, sizeof(long), 0);

    // Forward the response to client
    // Receive response from target server and send it to client
    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    while ((bytes = recv(server_sock, buffer, BUFFER_SIZE, 0)) > 0) {
        printf("File content sent to client : %s\n", buffer);
        send(client_sock, buffer, bytes, 0);
    }

    // Close the server socket
    close(server_sock);
    printf("File sent successfully to client.\n");
}

/**
 * @brief Processes file deletion requests
 * @param client_sock The client socket descriptor
 * @param filepath The full client path (~S1/...)
 *
 * Directly deletes .c files on S1
 * For pdf/txt: Forwards delete request to appropriate storage server
 * Validates paths and handles errors
 * 
 * @details Implements protocol:
 * 'R' - Remove File
 *   1. S1 → Storage: 'R' + path_len + path  
 *   2. Storage → S1: Success/Failure response
 */
void handle_remove_request(int client_sock, const char *filepath) 
{
    // Validate input
    if (!filepath || strlen(filepath) == 0) {
        send(client_sock, "EEmpty file path", 16, 0);
        return;
    }

    // Extract file extension
    const char *ext = strrchr(filepath, '.');
    if (!ext) {
        send(client_sock, "EInvalid file: no extension", 27, 0);
        return;
    }
    ext++; // Move past the dot
    printf("Extension is: %s\n", ext);

    // If the received file has a ".c" extension
    if (strcmp(ext, "c") == 0) 
    {
        // Handle .c file locally
        char local_path[MAX_PATH_LEN];
        char *home_dir = NULL;
        const char *s1_part = strstr(filepath, "S1/");  // Pointer to S

            // Validate path format
        if (!s1_part || s1_part - filepath !=1 || strncmp(filepath, "~S1/", 4) != 0) 
        {
            send(client_sock, "EPath must be in format: ~S1/...", 33, 0);
            return;
        }

        // Build absolute path with buffer safety
        // Converts ~S1/.. to /home/user/S1/..
        home_dir = getenv("HOME");
        snprintf(local_path, MAX_PATH_LEN, "%s/S1/%s", home_dir, s1_part + 3);
        printf("Absolute path of file in S1:%s\n",local_path);

        // Send status to client to proceed for removing
        long status = 1;
        send(client_sock, &status, sizeof(long), 0);

        // Execute deletion
        if (remove(local_path) == 0) {
            send(client_sock, "SFile deleted successfully", 26, 0);
            printf("SFile deleted successfully\n");
        } 
        else {
            // Provide specific error messages
            switch (errno) {
                case ENOENT:
                    printf("EFile not found\n");
                    send(client_sock, "EFile not found", 15, 0);
                    break;
                case EACCES:
                    printf("EPermission denied\n");
                    send(client_sock, "EPermission denied", 18, 0);
                    break;
                default:
                    printf("EFile deletion failed\n");
                    send(client_sock, "EFile deletion failed", 21, 0);
            }
        }

        return;
    } 

    // If the received file has an extension other than ".c", send it to another server
    // Select appropriate target server for non-.c files
    // Determine target port
    int target_port;
    // If the received file has a ".pdf" extension
    if (strcmp(ext, "pdf") == 0) {
        target_port = PORT_S2;
    }
    // If the received file has a ".txt" extension 
    else if (strcmp(ext, "txt") == 0) {
        target_port = PORT_S3;
    }
    // If the received file has a ".zip" extension 
    else if (strcmp(ext, "zip") == 0) {
        target_port = PORT_S4;
    } 
    // If the received file has any other extension
    else {
        send(client_sock, "EUnsupported file type", 22, 0);
        return;
    }

    int server_sock = connect_to_target_server(target_port, client_sock);
    if (server_sock < 0) {
        return;  // Error already handled
    }
    printf("Server Connected.\n");
    printf("Filepath: %s\n",filepath);

    // Send remove command to storage server
    char command_type = 'R'; // 'R' for remove
    if (send(server_sock, &command_type, 1, 0) != 1) {
        perror("Failed to send command type");
        close(server_sock);
        send(client_sock, "EInternal server error", 22, 0);
        return;
    }

    // If connection is successful to target server
    // Send status to client to proceed
    long status = 1;
    send(client_sock, &status, sizeof(long), 0);

    // Send path length and path to target storage server
    int path_len = strlen(filepath);
    printf("Filepath length: %d\n",path_len);

    if (send(server_sock, &path_len, sizeof(int), 0) != sizeof(int)) 
    {
        perror("Failed to send path length");
        // Close the socket and notify the client about the failure
        close(server_sock);
        send(client_sock, "EInternal server error", 22, 0);
        return;
    }
    if (send(server_sock, filepath, path_len, 0) != path_len) 
    {
        perror("Failed to send path");
        // Close the socket and notify the client about the failure
        close(server_sock);
        send(client_sock, "EInternal server error", 22, 0);
        return;
    }

    // Get response from storage server, store it in the response buffer
    char response[BUFFER_SIZE];
    ssize_t bytes_received = recv(server_sock, response, BUFFER_SIZE, 0);
    response[bytes_received] = '\0';    // Update last character
    printf("Response received from server: %s\n",response);
    printf("Response bytes received : %d\n",bytes_received);
    close(server_sock); // Close the socket 

    // Send response to client
    if (bytes_received <= 0) {
        send(client_sock, "ENo response from storage server", 31, 0);
    } else {
        printf("Response send to client : %s\n",response);
        // Forward the storage server's response to the client
        send(client_sock, response, bytes_received, 0);
    }
}

/**
 * @brief Processes download tar archive requests
 * @param client_sock The client socket descriptor
 * @param filetype The file extension to tar (c/pdf/txt)
 * 
 * For .c files: Creates tar locally from S1 storage
 * For pdf/txt: Forwards request to appropriate storage server
 * Streams the tar file directly to client
 * Cleans up temporary files after transfer
 * 
 * @details Implements protocol:
 * 'T' - Tar Files
 *   1. S1 → Storage: 'T' + filetype_len + filetype (.pdf/.txt)
 *   2. Storage → S1: tar_size + tar_data
 */
void handle_downloadtar_request(int client_sock, const char *filetype) {
    // Validate filetype
    if (!filetype || strlen(filetype) == 0|| (strcmp(filetype, "c") != 0 && 
                    strcmp(filetype, "pdf") != 0 && 
                    strcmp(filetype, "txt") != 0)){
        send(client_sock, "EInvalid filetype (use: c, pdf, txt)", 35, 0);
        return;
    }

    // Determine target server
    int target_port;

    // If the received file type is "c"
    if (strcmp(filetype, "c") == 0) {
        // Handle .c files locally
  
        // First check if S1 directory exists
        struct stat st;
        char s1_dir[256];
        snprintf(s1_dir, sizeof(s1_dir), "%s/S1", getenv("HOME"));
        if (stat(s1_dir, &st) == -1 || !S_ISDIR(st.st_mode)) {
            long error = -1;
            send(client_sock, &error, sizeof(long), 0);
            char *err_msg = "ES1 directory not found";
            int msg_len = strlen(err_msg);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, err_msg, msg_len, 0);
            printf("ES1 directory not found\n");
            return;
        }

        // Check if there are any .c files
        char check_cmd[512];
        snprintf(check_cmd, sizeof(check_cmd), 
                "find %s -type f -name '*.c' | head -n 1 | grep -q .", s1_dir);
        
        if (system(check_cmd) != 0) {
            long error = -1;
            send(client_sock, &error, sizeof(long), 0);
            char *err_msg = "ENo .c files found in S1 directory";
            int msg_len = strlen(err_msg);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, err_msg, msg_len, 0);
            printf("ENo .c files found in S1 directory\n");
            return;
        }
            
        // Construct tar file name
        char tar_filename[256];     // cfiles.tar for c tar file
        snprintf(tar_filename, sizeof(tar_filename), "%sfiles.tar", filetype);

        // Create a temporary directory for server files
        char temp_dir[] = "server_temp";
        char dir_command[1024];
        snprintf(dir_command, sizeof(dir_command), "mkdir %s", temp_dir);
        if (system(dir_command)) {
            long error = -1;
            send(client_sock, &error, sizeof(long), 0);
            char *err_msg = "ECould not create temp directory";
            int msg_len = strlen(err_msg);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, err_msg, msg_len, 0);
            printf("ECould not create temp directory\n");
            return;
        }

        // Create path for tar file (relative paths)
        char server_tar_path[512];
        snprintf(server_tar_path, sizeof(server_tar_path), "%s/%s", temp_dir, tar_filename);
        printf("Tar file path is: %s\n", server_tar_path);

        // Create file list (relative paths)
        char list_path[512];
        snprintf(list_path, sizeof(list_path), "%s/c_files.list", temp_dir);
        printf("List path is: %s\n", list_path);
        
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "find ~/S1 -type f -name '*.c' | sed 's|^.*/S1/||' > %s && tar -C ~/S1 -cf %s -T %s",
               list_path, server_tar_path, list_path);
        printf("Tar command is: %s\n", cmd);
        int ret = system(cmd);
        if (ret != 0) {
            long error = -1;
            send(client_sock, &error, sizeof(long), 0);
            char *err_msg = "ETar creation failed";
            int msg_len = strlen(err_msg);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, err_msg, msg_len, 0);
            printf("ETar creation failed\n");
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
            send(client_sock, &error, sizeof(long), 0);
            char *err_msg = "ETar creation failed";
            int msg_len = strlen(err_msg);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, err_msg, msg_len, 0);
            printf("ETar creation failed\n");
            return;
        }

        // Move pointer to the end of file to get the size
        fseek(tar_file, 0, SEEK_END);
        long tar_size = ftell(tar_file); 
        fseek(tar_file, 0, SEEK_SET);   // Move pointer back to start of file

        // Send status to client to proceed
        long status = 1;
        send(client_sock, &status, sizeof(long), 0);

        // Send tar file size to client
        send(client_sock, &tar_size, sizeof(long), 0);

        char buffer[BUFFER_SIZE];
        while (tar_size > 0) {
            int chunk = tar_size > BUFFER_SIZE ? BUFFER_SIZE : tar_size;
            fread(buffer, 1, chunk, tar_file);
            send(client_sock, buffer, chunk, 0);
            printf("File content sent to client : %s\n", buffer);
            tar_size -= chunk;
        }

        // Close the tar file
        fclose(tar_file);   

        // Cleanup temp files
        remove(list_path);
        remove(server_tar_path);
        rmdir(temp_dir);
        printf("Tar file sent successfully to client\n");

        return;
    }
    else {
        // Route to appropriate server
        if (strcmp(filetype, "pdf") == 0) {
            target_port = PORT_S2;
        } else if (strcmp(filetype, "txt") == 0) {
            target_port = PORT_S3;
        }

        int server_sock = connect_to_target_server(target_port, client_sock);
        if (server_sock < 0) {
            return;  // Error already handled
        }
        printf("Server Connected.\n");

        // Send tar command ('T') to target server (S2/S3 : pdf/txt)
        char command = 'T';
        send(server_sock, &command, 1, 0);

        // Send filetype length and filetype to target server
        int type_len = strlen(filetype);
        send(server_sock, &type_len, sizeof(int), 0);
        send(server_sock, filetype, type_len, 0);

        // Wait for status byte from target server
        // If directory and file is present in target server, only then proceed
        long status1;
        recv(server_sock, &status1, sizeof(long), 0);
        send(client_sock, &status1, sizeof(long), 0);
        printf("status1: %d\n", status1);

        if (status1 == -1) {
            //printf("ES1 directory or .pdf files not found\n");
            int msg_len;
            recv(server_sock, &msg_len, sizeof(int), 0);
            printf("msg_len: %d\n", msg_len);
            char error_msg[BUFFER_SIZE];
            recv(server_sock, error_msg, msg_len, 0);
            error_msg[msg_len] = '\0';
            printf("Error: %s\n", error_msg);
            //send(client_sock, &status1, sizeof(long), 0);
            send(client_sock, &msg_len, sizeof(int), 0);
            send(client_sock, error_msg, msg_len, 0);
            close(server_sock);
            return;
        }

        // Receive tar file size from target server
        long tar_size;
        recv(server_sock, &tar_size, sizeof(long), 0);

        if (tar_size < 0) {
            char error_msg[100];
            int msg_len;
            recv(server_sock, &msg_len, sizeof(int), 0);
            recv(server_sock, error_msg, msg_len, 0);
            error_msg[msg_len] = '\0';
            send(client_sock, error_msg, msg_len, 0);
            close(server_sock);
            return;
        }

        // Forward tar file size to client
        send(client_sock, &tar_size, sizeof(long), 0);

        // Receive response from target server and send it to client
        char buffer[BUFFER_SIZE];
        long bytes_remaining = tar_size;
        while (bytes_remaining > 0) {
            int chunk = bytes_remaining > BUFFER_SIZE ? BUFFER_SIZE : bytes_remaining;
            int bytes_received = recv(server_sock, buffer, chunk, 0);
            send(client_sock, buffer, bytes_received, 0);
            printf("File content sent to client : %s\n", buffer);
            bytes_remaining -= bytes_received;
        }

        // Close the server socket
        close(server_sock);
        printf("Tar file sent successfully to client\n");
    }
}

/**
 * @brief Structure representing a file entry with name and extension
 * 
 * @var filename The name of the file (including extension)
 * @var ext The file extension (e.g. ".c", ".txt")
 * 
 * @note Both strings are dynamically allocated and must be freed by the caller
 */
typedef struct
{
    char *filename; /**< Full filename string (allocated) */
    char *ext;      /**< File extension string (allocated) */
} FileEntry;

/**
 * @brief Comparator function for sorting FileEntry structures
 * 
 * @param a First FileEntry to compare
 * @param b Second FileEntry to compare
 * @return int Negative if a < b, positive if a > b, zero if equal
 * 
 * @details Sorts files first by extension order (.c → .pdf → .txt → .zip),
 *          then alphabetically by filename within each extension group.
 *          Designed for use with qsort().
 */
int compare_files(const void *a, const void *b)
{
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;

    // First sort by extension order: .c, .pdf, .txt, .zip
    static const char *ext_order[] = {".c", ".pdf", ".txt", ".zip"};
    int ext_a = 0, ext_b = 0;

    for (int i = 0; i < 4; i++)
    {
        if (strcmp(fa->ext, ext_order[i]) == 0)
            ext_a = i;
        if (strcmp(fb->ext, ext_order[i]) == 0)
            ext_b = i;
    }

    if (ext_a != ext_b)
        return ext_a - ext_b;

    // Then sort alphabetically within each extension group
    return strcmp(fa->filename, fb->filename);
}

/**
 * @brief Retrieves files with specific extension from a directory
 * 
 * @param path Directory path to search
 * @param ext File extension to filter by (e.g. ".c")
 * @param files Pointer to array of FileEntry structures (allocated/reallocated)
 * @param count Pointer to count of found files (updated)
 * 
 * @details Scans specified directory for regular files matching given extension.
 *          Allocates/reallocates FileEntry array and populates with filename/ext.
 *          Caller must free allocated memory for both array and string fields.
 */
void get_files_from_dir(const char *path, const char *ext, FileEntry **files, int *count)
{
    DIR *dir;
    struct dirent *ent;

    if ((dir = opendir(path)) != NULL)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            char fullpath[1024];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);

            struct stat st;
            if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode))
            {
                char *dot = strrchr(ent->d_name, '.');
                if (dot && strcmp(dot, ext) == 0)
                {
                    *files = realloc(*files, (*count + 1) * sizeof(FileEntry));
                    (*files)[*count].filename = strdup(ent->d_name);
                    (*files)[*count].ext = strdup(ext);
                    (*count)++;
                }
            }
        }
        closedir(dir);
    }
}

/**
 * @brief Requests a list of files from a remote server based on directory and extension.
 *
 * This function establishes a TCP connection to a secondary server (e.g., S2, S3, S4),
 * sends a request to list files in a specific directory (`pathname`), and retrieves 
 * filenames matching a specific extension. The received filenames are stored in a dynamically 
 * allocated array of `FileEntry` structures.
 *
 * @param ip        IP address of the server to connect to (e.g., "127.0.0.1").
 * @param port      Port number of the target server (e.g., 9002, 9003, 9004).
 * @param pathname  Directory path on the server to search for files (e.g., "~S2/folder1").
 * @param ext       File extension used to tag returned entries (e.g., ".pdf", ".txt").
 * @param files     Pointer to a dynamically allocated array of FileEntry structures (output).
 * @param count     Pointer to an integer tracking the number of files found (incremented).
 *
 * @return 0 on success, -1 on failure (e.g., socket error, connection error, or no valid response).
 *
 * @note Memory allocated for filenames and extensions must be freed by the caller.
 */
int request_files_from_server(const char *ip, int port, const char *pathname, const char *ext, FileEntry **files, int *count) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    serv_addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return -1;
    }

    // send(sock, command, strlen(command), 0);
    char command = 'L';
    send(sock, &command, 1, 0);

    int path_len = strlen(pathname);
    send(sock, &path_len, sizeof(int), 0);
    send(sock, pathname, path_len, 0);

    // Receive status
    long status;
    if (recv(sock, &status, sizeof(long), 0) <= 0 || status != 1) {
        close(sock);
        return -1;
    }

    // Receive file count
    int remote_count;
    if (recv(sock, &remote_count, sizeof(int), 0) <= 0) {
        close(sock);
        return -1;
    }

    for (int i = 0; i < remote_count; i++) {
        int len;
        if (recv(sock, &len, sizeof(int), 0) <= 0) break;

        char *fname = malloc(len + 1);
        if (recv(sock, fname, len, 0) <= 0) {
            free(fname);
            break;
        }
        fname[len] = '\0';

        *files = realloc(*files, (*count + 1) * sizeof(FileEntry));
        (*files)[*count].filename = fname;
        (*files)[*count].ext = strdup(ext);
        (*count)++;
    }

    close(sock);
    return 0;
}

// Utility function to replace occurrences of a substring in a string
char* str_replace(char *str, const char *old, const char *new) {
    static char buffer[4096];
    char *p;

    if (!(p = strstr(str, old))) {
        return str;
    }

    strncpy(buffer, str, p - str); // Copy the part before 'old'
    buffer[p - str] = '\0';

    sprintf(buffer + (p - str), "%s%s", new, p + strlen(old)); // Append new part

    return buffer;
}


/**
 * @brief Processes directory listing requests
 * @param client_sock Client socket descriptor
 * @param pathname Directory path to list (~S1/...)
 *
 * Aggregates file lists from all storage servers
 * Combines .c (local), .pdf (S2), .txt (S3), .zip (S4)
 * Sorts results alphabetically by filename
 * Returns only filenames with extensions
 * Handles permission errors and invalid paths
 * 
 * @details Implements protocol:
 * 'L' - List Files
 *   1. S1 → Storage: 'L' + path_len + path
 *   2. Storage → S1: file_count + [filename1, filename2...]
 */
void handle_pathname_request(int client_sock, const char *pathname) {
    // Validate input
    if (!pathname || strlen(pathname) == 0) {
        send(client_sock, "EEmpty pathname", 15, 0);
        return;
    }

    printf("Received pathname: %s\n", pathname);

    FileEntry *files = NULL;
    int count = 0;
    char base_path[MAX_PATH_LEN];
    char *home_dir = getenv("HOME");

    if (!home_dir) {
        send(client_sock, "EHome directory not found", 25, 0);
        return;
    }

    // Helper macro for collecting files from a directory
    #define COLLECT_FILES(SERVER, EXT) \
        snprintf(base_path, sizeof(base_path), "%s/" SERVER "%s", home_dir, pathname + 3); \
        get_files_from_dir(base_path, EXT, &files, &count);

    // Get local .c files from S1 directory
    COLLECT_FILES("S1", ".c");

    // Request file lists from remote servers
    // Modify the path by replacing S1 with the respective server's directory
    char new_path[1024];

    // For S2 (.pdf files)
    snprintf(new_path, sizeof(new_path), "%s", pathname);
    char *path_s2 = str_replace(new_path, "S1", "S2");
    request_files_from_server("127.0.0.1", PORT_S2, path_s2, ".pdf", &files, &count); 

    // For S3 (.txt files)
    snprintf(new_path, sizeof(new_path), "%s", pathname);
    char *path_s3 = str_replace(new_path, "S1", "S3");
    request_files_from_server("127.0.0.1", PORT_S3, path_s3, ".txt", &files, &count); 

    // For S4 (.zip files)
    snprintf(new_path, sizeof(new_path), "%s", pathname);
    char *path_s4 = str_replace(new_path, "S1", "S4");
    request_files_from_server("127.0.0.1", PORT_S4, path_s4, ".zip", &files, &count);

    // Sort files
    qsort(files, count, sizeof(FileEntry), compare_files);

    // Send status first
    long status = 1;
    send(client_sock, &status, sizeof(long), 0);

    // Send file count
    send(client_sock, &count, sizeof(int), 0);

    // Send filenames one by one
    for (int i = 0; i < count; i++) {
        int fnlen = strlen(files[i].filename);
        send(client_sock, &fnlen, sizeof(int), 0);
        send(client_sock, files[i].filename, fnlen, 0);
        printf("Sent file Name: %s\n", files[i].filename);

        // Free memory for each entry
        free(files[i].filename);
        free(files[i].ext);
    }

    // Free the file list
    free(files);

    printf("Completed sending file list.\n");
}

/**
 * @brief Handles a client connection in a dedicated process
 * @param client_sock The client socket descriptor
 * 
 * Processes all client commands in an infinite loop:
 * - uploadf: Receives and routes files to appropriate servers
 * - downlf: Retrieves files from storage servers
 * - removef: Deletes files across the system
 * - downltar: Creates and sends tar archives
 * - dispfnames: Lists directory contents
 * - exit: Terminates connection
 */
void prcclient(int client_sock) {
    char buffer[BUFFER_SIZE];
    int bytes_received;

    while (1) {
        // Receive command from client and store in buffer
        bytes_received = recv(client_sock, buffer, BUFFER_SIZE, 0);
        if (bytes_received <= 0) break;
        buffer[bytes_received] = '\0'; //Add \0 at the end
        printf("Bytes received from client:%s\n",buffer);

        // Parse command
        // Get the first token (i.e; command)
        // Command = uploadf, downlf, removef, downltar and dispfnames
        char *command = strtok(buffer, " ");    
        if (!command) continue;

        // If the command is equal to "uploadf"
        if (strcmp(command, "uploadf") == 0) {
            printf("\n======Command uploadf received======\n");
            // Get the second token (i.e; filename)
            char *filename = strtok(NULL, " ");
            // Get the third token (i.e; destination path)
            char *dest_path = strtok(NULL, " ");
            if (!filename || !dest_path) {
                send(client_sock, "EUsage: uploadf <filename> <destination_path>", 44, 0);
                continue;
            }
            printf("Filename:%s\n",filename);
            printf("Destination path:%s\n",dest_path);

            // For all file types
            handle_upload_request(client_sock, filename, dest_path);
        }
        // If the command is equal to "downlf"
        else if (strcmp(command, "downlf") == 0) {
            printf("\n======Command downlf received======\n");

            // Get the second token (i.e; filepath)
            char *filepath = strtok(NULL, " ");
            if (!filepath) 
            {
                send(client_sock, "EUsage: downlf <filepath>", 24, 0);
                continue;
            }
            printf("Filepath:%s\n",filepath);

            // For all file types
            handle_download_request(client_sock, filepath);
        }
        // If the command is equal to "removef"
        else if (strcmp(command, "removef") == 0) {
            printf("\n======Command removef received======\n");

            // Get the second token (i.e; filepath)
            char *filepath = strtok(NULL, " ");
            if (!filepath) 
            {
                send(client_sock, "EUsage: removef <filepath>", 25, 0);
                continue;
            }
            printf("Filepath:%s\n",filepath);

            // For all file types
            handle_remove_request(client_sock, filepath);
        }
        // If the command is equal to "downltar"
        else if (strcmp(command, "downltar") == 0) {
            printf("\n======Command downltar received======\n");

            // Get the second token (i.e; filetype)
            // Supported file types: .c, .pdf, .txt 
            char *filetype = strtok(NULL, " ");
            if (!filetype) {
                send(client_sock, "EUsage: downltar <filetype>", 26, 0);
                continue;
            }
            filetype++;     // After dot content
            printf("Filetype:%s\n",filetype);

            // For all file types
            handle_downloadtar_request(client_sock, filetype);
        }
        // If the command is equal to "dispfnames"
        else if (strcmp(command, "dispfnames") == 0) {
            printf("\n======Command dispfnames received======\n");
            // Get the second token (i.e; pathname)
            char *pathname = strtok(NULL, " ");
            if (!pathname) 
            {
                send(client_sock, "EUsage: dispfnames <pathname>", 28, 0);
                continue;
            }
            printf("Pathname:%s\n",pathname);

            // For all file types
            handle_pathname_request(client_sock, pathname);
        }
        else if (strcmp(command, "exit") == 0) {
            break;
        }
        else{
            send(client_sock, "EUnknown command", 16, 0);
        }
    }
    close(client_sock);
}

/*
 * Signal handler for SIGCHLD
 *
 * @brief function is triggered automatically whenever a child process terminates.
 * It uses a non-blocking loop (`WNOHANG`) with `waitpid()` to reap all exited
 * child processes and prevent zombie processes from accumulating.
 *
 * This ensures that the server remains clean and does not leave defunct
 * (zombie) child processes in the process table.
 * */
void handle_sigchld(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/**
 * @brief Entry point for the main distributed file system server (S1)
 * 
 * Initializes and runs the primary server that:
 * 1. Creates a listening socket on configured port (PORT_S1)
 * 2. Handles multiple concurrent client connections using fork()
 * 3. Routes requests to appropriate handlers:
 *    - Local processing for .c files
 *    - Forwarding to S2 (PDF), S3 (TXT), S4 (ZIP) servers
 * 4. Maintains system resources and cleans up on termination
 *
 *
 * @return int Returns EXIT_SUCCESS (0) on normal shutdown, 
 *             EXIT_FAILURE (1) on critical errors
 * 
 * Server-to-Server Command Protocol (S1 ↔ S2/S3/S4):
 * Single-character commands followed by path/data:
 * 
 * 'U' - Upload File
 *   1. S1 → Storage: 'U' + path_len + path + file_size + file_data
 *   2. Storage → S1: Success/Failure response
 * 
 * 'D' - Download File  
 *   1. S1 → Storage: 'D' + path_len + path
 *   2. Storage → S1: file_size + file_data OR error
 * 
 * 'R' - Remove File
 *   1. S1 → Storage: 'R' + path_len + path  
 *   2. Storage → S1: Success/Failure response
 * 
 * 'T' - Tar Files
 *   1. S1 → Storage: 'T' + filetype_len + filetype (.pdf/.txt)
 *   2. Storage → S1: tar_size + tar_data
 * 
 * 'L' - List Files
 *   1. S1 → Storage: 'L' + path_len + path
 *   2. Storage → S1: file_count + [filename1, filename2...]
 */
int main() {

    // Read children automatically
    signal(SIGCHLD, handle_sigchld); 

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
    address.sin_port = htons(PORT_S1);

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
    printf("🚀  S1 Server is UP and listening on port %d\n", PORT_S1);
    printf("==============================================\n\n");

    while (1) {
        // Accept connection
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            continue;
        }

        printf("New Client Connected with id: %d.\n",new_socket );

        // Fork a child process to handle the client
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(new_socket);
            continue;
        }

        if (pid == 0) { // Child process
            close(server_fd); // Close listening socket in child
            prcclient(new_socket);
            exit(0);
        } else { // Parent process
            close(new_socket); // Close connected socket in parent
            waitpid(-1, NULL, WNOHANG); // Clean up zombie processes
        }
    }

    return 0;
}