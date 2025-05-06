/*
 * w25clients.c - Client Interface for Distributed File System
 *
 * Description:
 * ------------
 * This client connects to the main server (S1) and allows users to interact with
 * the distributed file system using custom commands:
 *   - uploadf: Upload files to distributed storage
 *   - downlf: Download files from server
 *   - removef: Delete file on server
 *   - downltar: Downloads .tar archive of all files of given type
 *   - dispfnames: Lists all files in directory
 *
 * Key Behaviors:
 * --------------
 * - Sends user commands to S1.
 * - Receives responses and file data from S1.
 * - Validates filename formats (e.g. no paths for uploadf).
 * - Displays appropriate messages for success/error scenarios.
 *
 * Usage:
 * ------
 * Compile: gcc w25clients.c -o w25clients
 * Run:     ./w25clients 
 * 
 * Port: Always connects to S1 on localhost:6071 (can be changed via macro)
 * 
 * Supported Client Commands:
 * --------------------------
 * 
 * 1. uploadf <filename> <destination_path>
 *    - Example: uploadf report.pdf ~S1/docs/
 *    - Supported extensions: .c, .pdf, .txt, .zip
 * 
 * 2. downlf <filepath>
 *    - Example: downlf ~S1/project/source.c
 * 
 * 3. removef <filepath>
 *    - Example: removef ~S1/old/notes.txt
 * 
 * 4. downltar <filetype>
 *    - Example: downltar .pdf → downloads pdffiles.tar
 *    - Supported types: .c, .pdf, .txt (excludes .zip)
 *    - Filename: cfiles.tar for c, pdf.tar for pdf, txt.tar for txt
 * 
 * 5. dispfnames <directory>
 *    - Example: dispfnames ~S1/project/
 *    - Output format: alphabetized by extension (.c → .pdf → .txt → .zip)
 * 
 * 6. exit
 *    - Terminates client session
 * 
 * Path Specifications:
 * --------------------
 * - All paths must use ~S1/ prefix regardless of actual storage location
 * - Example valid path: ~S1/folder/sub/file.txt
 * - Example invalid path: /absolute/path/file.txt
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
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <asm-generic/socket.h>


#define PORT_S1 6071
#define BUFFER_SIZE 1024

/**
 * @brief Uploads a file to the server
 * @param sock The connected socket to S1 
 * @param filename Local file to upload
 * @param dest_path Destination path on server (~S1/...)
 *
 * Validates file existence locally
 * Sends file with size prefix protocol
 * Handles server responses and errors
 */
void upload_file(int sock, const char *filename, const char *dest_path) {

    // Open the file
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Error opening file");
        return;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    rewind(fp);

    // Read the file into memory
    char *filedata = malloc(filesize);
    if (fread(filedata, 1, filesize, fp) != filesize) {
        perror("Error reading file");
        fclose(fp);
        free(filedata);
        return;
    }
    fclose(fp);

    // Send file size
    write(sock, &filesize, sizeof(long));

    // Send file data in chunks
    long total_sent = 0;
    while (total_sent < filesize) {
        int chunk = write(sock, filedata + total_sent, filesize - total_sent);
        //printf("File content sent to server : %s\n", filedata);
        if (chunk <= 0) break;
        total_sent += chunk;
    }

    // Receive the server's response
    char response[1024];
    read(sock, response, sizeof(response));
    printf("Server response: %s\n", response);

    free(filedata);
}

/**
 * @brief Downloads a file from the server
 * @param sock The connected socket to S1
 * @param filepath Server path to download (~S1/...)
 *
 * Requests file from server
 * Receives and saves file with progress
 * Handles all error cases
 * Preserves original filename
 */
void download_file(int sock, const char *filepath) {

    // Proceed to receive the response from server only if:
    // - Target server is connected properly to storage server
    // Otherwise, the server responds with a status value of -1 to indicate an error.
    long status;
    int r = recv(sock, &status, sizeof(long), 0);
    //printf("Status received from S1: %d\n", status);
    if (status == -1) {
        // Error flow
        int msg_len;
        recv(sock, &msg_len, sizeof(int), 0);
        char error_msg[BUFFER_SIZE];
        recv(sock, error_msg, msg_len, 0);
        error_msg[msg_len] = '\0';
        // Ignore the leading 'E' in the error message before printing.
        printf("%s\n", error_msg+1);
        return;
    }

    // Receive file size or error
    long file_size;
    int bytes_received = recv(sock, &file_size, sizeof(long), 0);
    if (bytes_received <= 0) {
        printf("Connection error\n");
        return;
    }
    //printf("File size received from S1: %d\n", file_size);
    if (file_size == -1) {
        // Error case
        int msg_len;
        recv(sock, &msg_len, sizeof(int), 0);
        char error_msg[BUFFER_SIZE];
        recv(sock, error_msg, msg_len, 0);
        error_msg[msg_len] = '\0';
        // Ignore the leading 'E' in the error message before printing.
        printf("Server response: %s\n", error_msg+1);
        return;
    }
    
    // Extract filename from path
    const char *filename = strrchr(filepath, '/');
    if (!filename) filename = filepath;
    else filename++;

    // Save file
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Failed to create file");
        return;
    }
    //printf("Filename sent to S1: %s\n", filename);

    char buffer[BUFFER_SIZE];
    buffer[0] = '\0';   // Clear all previous values
    long bytes_remaining = file_size;
    while (bytes_remaining > 0) {
        int bytes_to_read = bytes_remaining < BUFFER_SIZE ? bytes_remaining : BUFFER_SIZE;
        int bytes_received = recv(sock, buffer, bytes_to_read, 0);
        //printf("File content received from S1: %s\n", buffer);
        if (bytes_received <= 0) break;
        fwrite(buffer, 1, bytes_received, file);
        bytes_remaining -= bytes_received;
    }

    fclose(file);
    printf("File downloaded successfully: %s\n", filename);
}

/**
 * @brief Initiates file deletion on server
 * @param sock Connected socket to S1
 * @param filepath Server path to delete (~S1/...)
 *
 * Validates path format before sending
 * Displays progress feedback
 * Handles all server response messages
 * Supports deletion of all file types
 */
void remove_file(int sock, const char *filepath) {

    // Proceed to receive the response from server only if:
    // - Target server is connected properly to storage server
    // Otherwise, the server responds with a status value of -1 to indicate an error.
    long status;
    int r = recv(sock, &status, sizeof(long), 0);
    if (r <= 0) {
        printf("Invalid status received.\n");
        return;
    }
    //printf("Status received from S1: %d\n", status);
    if (status == -1) {
        // Error flow
        int msg_len;
        recv(sock, &msg_len, sizeof(int), 0);
        char error_msg[BUFFER_SIZE];
        recv(sock, error_msg, msg_len, 0);
        error_msg[msg_len] = '\0';
        // Ignore the leading 'E' in the error message before printing.
        printf("%s\n", error_msg+1);
        return;
    }
    
    // Receive response
    char response[BUFFER_SIZE] = " ";
    int bytes_received = recv(sock, response, BUFFER_SIZE, 0);
    //printf("Bytes received: %s\n", response);
    if (bytes_received <= 0) {
        printf("Connection lost\n");
        return;
    }
    response[bytes_received] = '\0';

    // Parse response
    if (response[0] == 'E') {
        printf("Server response: %s\n", response + 1);
    } else {
        printf("Server response: %s\n", response + 1);
    }
}

/**
 * @brief Initiates tar archive download from server
 * @param sock The connected socket to S1
 * @param filetype The extension to download (c/pdf/txt)
 *
 * Sends downltar command to server
 * Receives and saves the tar archive
 * Shows progress and handles errors
 * Outputs: <type>files.tar
 * Example: cfiles.tar for c, pdf.tar for pdf, txt.tar for txt
 */
void downloadtar_file(int sock, const char *filetype) {

    // Proceed to receive the tar file size only if:
    // - The target directory exists on the server, and
    // - It contains at least one .c file.
    // - Target server is connected properly to storage server
    // Otherwise, the server responds with a status value of -1 to indicate an error.
    long status;
    int r = recv(sock, &status, sizeof(long), 0);
    if (r <= 0) {
        printf("Invalid status received.\n");
        return;
    }
    //printf("Status received from S1: %d\n", status);
    if (status == -1) {
        // Error flow
        int msg_len;
        recv(sock, &msg_len, sizeof(int), 0);
        char error_msg[BUFFER_SIZE];
        recv(sock, error_msg, msg_len, 0);
        error_msg[msg_len] = '\0';
        // Ignore the leading 'E' in the error message before printing.
        printf("%s\n", error_msg+1);
        return;
    }

    // Receive response from server
    long tar_size;
    int bytes_received = recv(sock, &tar_size, sizeof(long), 0);
    
    if (bytes_received <= 0) {
        printf("Connection error\n");
        return;
    }
    //printf("Tar file size received from S1: %d\n", tar_size);

    if (tar_size < 0) {
        // Error case
        int msg_len;
        recv(sock, &msg_len, sizeof(int), 0);
        char error_msg[BUFFER_SIZE];
        recv(sock, error_msg, msg_len, 0);
        error_msg[msg_len] = '\0';
        // Ignore the leading 'E' in the error message before printing.
        printf("Server response: %s\n", error_msg+1);   
        return;
    }

    // Find last dot in filename
    const char *ext = strrchr(filetype, '.');
    ext++;      // Contain file type only without dot
    //printf("File extension is: %s\n",ext);

    // Create output filename
    char filename[50];
    // cfiles.tar for c file name
    // txt.tar for txt file name
    // pdf.tar for pdf file name
    if (strcmp(ext, "c") == 0){
        snprintf(filename, sizeof(filename), "%sfiles.tar", ext);  
    }
    else {
        snprintf(filename, sizeof(filename), "%s.tar", ext);  
    }

    // Save tar file
    FILE *tar_file = fopen(filename, "wb");
    if (!tar_file) {
        perror("Failed to create tar file");
        return;
    }
    //printf("Tar file created with name: %s\n", filename);

    char buffer[BUFFER_SIZE];
    long bytes_remaining = tar_size;
    while (bytes_remaining > 0) {
        int chunk = bytes_remaining > BUFFER_SIZE ? BUFFER_SIZE : bytes_remaining;
        bytes_received = recv(sock, buffer, chunk, 0);
        //printf("Bytes received from S1: %s\n", buffer);
        if (bytes_received <= 0) break;
        fwrite(buffer, 1, bytes_received, tar_file);
        bytes_remaining -= bytes_received;
    }

    fclose(tar_file);
    printf("File %s downloaded successfully.\n", filename);
}

/**
 * @brief Requests and displays directory contents
 * @param sock Connected socket to S1
 * @param path Server directory path to list (~S1/...)
 *
 * Shows hierarchical directory structure
 * Handles network errors gracefully
 */
void list_file(int sock, const char *filepath){

    // Prepare and send the command to S1
    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "dispfnames %s", filepath);
    send(sock, command, strlen(command), 0);

    //printf("Command sent to S1: %s\n", command);

    // Receive status
    long status;
    int r = recv(sock, &status, sizeof(long), 0);
    if (r <= 0 || status != 1) {
        printf("Failed to retrieve file list or invalid status received.\n");
        return;
    }

    // Receive the total number of files
    int file_count;
    r = recv(sock, &file_count, sizeof(int), 0);
    if (r <= 0) {
        printf("Failed to receive file count.\n");
        return;
    }

    printf("Number of files received: %d\n", file_count);

    // Receive and display all filenames
    for (int i = 0; i < file_count; i++) {
        int fnlen;
        r = recv(sock, &fnlen, sizeof(int), 0);
        if (r <= 0) {
            printf("Error receiving filename length for file %d.\n", i + 1);
            break;
        }

        char filename[BUFFER_SIZE];
        r = recv(sock, filename, fnlen, 0);
        if (r <= 0) {
            printf("Error receiving filename for file %d.\n", i + 1);
            break;
        }

        filename[fnlen] = '\0';  // Null-terminate string
        printf("File %d: %s\n", i + 1, filename);
    }

    printf("File list retrieval complete.\n");
}

/**
 * @brief Main entry point for W25 Distributed Filesystem Client
 * 
 * @return int Returns 0 on normal exit, -1 on socket/connection errors
 * 
 * @details Establishes connection to S1 server (localhost:PORT_S1) and provides
 *          an interactive command-line interface for file operations including:
 *          - uploadf: Upload files to server (supports .c, .pdf, .txt, .zip)
 *          - downlf: Download files from server
 *          - removef: Delete files from server
 *          - downltar: Download tar bundles by file type
 *          - exit: Terminate the client session
 * 
 * @note The client maintains persistent connection until 'exit' command
 * @warning All file operations are restricted to ~S1/ paths
 */
int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT_S1);
    
    // Convert IPv4 address from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        return -1;
    }

    // Connect to server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }

    printf("Connected to S1 server\n");
    printf("====================================\n");
    printf("🖥️    W25 Client - Distributed FS     \n");
    printf("     Connected to: %d\n", PORT_S1);
    printf("====================================\n\n");

    char input[BUFFER_SIZE];
    while (1) {
        printf("w25clients$ ");
        fgets(input, BUFFER_SIZE, stdin);
        input[strcspn(input, "\n")] = '\0'; // Remove newline

        if (strcmp(input, "exit") == 0) {
            send(sock, "exit", 4, 0);
            break;
        }

        // Parse command
        char *command = strtok(input, " ");
        if (!command) continue;

        //************************************/
        //*************Upload file************/
        //************************************/
        if (strcmp(command, "uploadf") == 0) {
            // Get the second token (i.e; filename)
            char *filename = strtok(NULL, " ");
            // Get the third token (i.e; destination path)
            char *dest_path = strtok(NULL, " ");
            if (!filename || !dest_path) {
                printf("Invalid command syntax. Usage: uploadf filename ~S1/..\n");
                continue;
            }

            // Check if filename contains any '/' — disallow paths
            if (strchr(filename, '/')) {
                printf("Invalid command syntax. Usage: uploadf filename ~S1/..\n");
                continue;
            }

            // Check file type
            char *ext = strrchr(filename, '.');
            if (!ext || (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
                         strcmp(ext, ".txt") != 0 && strcmp(ext, ".zip") != 0)) {
                printf("Unsupported file type. Allowed: .c, .pdf, .txt, .zip\n");
                continue;
            }

            // Check if file exists
            if (access(filename, F_OK) != 0) {
                printf("File does not exist in the current directory.\n");
                continue;
            }

            // Check destination prefix
            if (strncmp(dest_path, "~S1", 3) != 0) {
                printf("Destination must start with ~S1\n");
                continue;
            }

            // Send command to server S1
            char command[BUFFER_SIZE];
            snprintf(command, BUFFER_SIZE, "uploadf %s %s", filename, dest_path);
            send(sock, command, strlen(command), 0);
            //printf("Command send to S1: %s\n", command);

            // Client server communication to upload file from PWD to server
            upload_file(sock, filename, dest_path);
        } 
        //************************************/
        //************Download file***********/
        //************************************/
        else if (strcmp(command, "downlf") == 0) {
            // Get the second token (i.e; filepath)
            char *filepath = strtok(NULL, " ");
            if (!filepath) {
                printf("Invalid command syntax. Usage: downlf ~S1/path/to/file\n");
                continue;
            }

            // Check filepath prefix
            if (strncmp(filepath, "~S1", 3) != 0) {
                printf("Filepath must start with ~S1. Usage: downlf ~S1/path/to/file\n");
                continue;
            }

            // Get last '/'
            const char *filename = strrchr(filepath, '/');
            if (!filename){
                printf("Invalid command syntax. Usage: downlf ~S1/path/to/file\n");
                continue;
            }
            else
                filename++; // Move past '/'

            // Check file type
            char *ext = strrchr(filename, '.');
            if (!ext || (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
                         strcmp(ext, ".txt") != 0 && strcmp(ext, ".zip") != 0)) {
                printf("Unsupported file type. Allowed: .c, .pdf, .txt, .zip\n");
                continue;
            }

             // Send command to server S1
            char command[BUFFER_SIZE];
            snprintf(command, BUFFER_SIZE, "downlf %s", filepath);
            send(sock, command, strlen(command), 0);
            //printf("Command send to S1: %s\n", command);

            // Client server communication to download file from server
            download_file(sock, filepath);
        }
        //************************************/
        //*************Remove file************/
        //************************************/
        else if (strcmp(command, "removef") == 0) {
            // Get the second token (i.e; filepath)
            char *filepath = strtok(NULL, " ");
            if (!filepath) {
                printf("Invalid command syntax. Usage: removef ~S1/path/to/file\n");
                continue;
            }

            // Check filepath prefix
            if (strncmp(filepath, "~S1", 3) != 0) {
                printf("Filepath must start with ~S1. Usage: removef ~S1/path/to/file\n");
                continue;
            }

            // Get last '/'
            const char *filename = strrchr(filepath, '/');
            if (!filename){
                printf("Invalid command syntax. Usage: removef ~S1/path/to/file\n");
                continue;
            }
            else
                filename++; // Move past '/'

            // Check file type
            char *ext = strrchr(filename, '.');
            if (!ext || (strcmp(ext, ".c") != 0 && strcmp(ext, ".pdf") != 0 &&
                         strcmp(ext, ".txt") != 0)) {
                printf("Unsupported file type. Allowed: .c, .pdf, .txt\n");
                continue;
            }

            // Send entire command to server S1
            char command[BUFFER_SIZE];
            snprintf(command, BUFFER_SIZE, "removef %s", filepath);
            //printf("Command sent to S1: %s\n", command);
            send(sock, command, strlen(command), 0);

            // Client server communication to remove a file from server
            remove_file(sock, filepath);
        } 
        //************************************/
        //**********Downlaod tar file*********/
        //************************************/
        else if (strcmp(command, "downltar") == 0) {
            // Get the second token (i.e; filetype)
            // Supported file types: .c, .pdf, .txt 
            char *filetype = strtok(NULL, " ");
            if (!filetype) {
                printf("Invalid command syntax. Usage: downltar <.c|.pdf|.txt>\n");
                continue;
            }

            // Check file type
            if ((strcmp(filetype, ".c") != 0 && strcmp(filetype, ".pdf") != 0 &&
                         strcmp(filetype, ".txt") != 0)) {
                printf("Unsupported file type. Allowed: .c, .pdf, .txt\n");
                continue;
            }

            // Send entire command to server S1
            char command[BUFFER_SIZE];
            snprintf(command, BUFFER_SIZE, "downltar %s", filetype);
            send(sock, command, strlen(command), 0);
            //printf("Command send to S1: %s\n", command);

            // Client server communication to download all files in tar format from server
            downloadtar_file(sock, filetype);
        }
        //************************************/
        //**************List file*************/
        //************************************/
        else if (strcmp(command, "dispfnames") == 0) {
            // Get the second token (i.e; pathname)
            char *filepath = strtok(NULL, " ");
            if (!filepath) {
                printf("Invalid command syntax. Usage: dispfnames ~S1/..\n");
                continue;
            }

            // Check filepath prefix
            if (strncmp(filepath, "~S1", 3) != 0) {
                printf("Filepath must start with ~S1. dispfnames ~S1/..\n");
                continue;
            }

            // Prepare and send the command to S1
            char command[BUFFER_SIZE];
            snprintf(command, BUFFER_SIZE, "dispfnames %s", filepath);
            send(sock, command, strlen(command), 0);
            //printf("Command sent to S1: %s\n", command);

            // Client server communication to list all files from server
            list_file(sock, filepath);
        } else {
            printf("Invalid command.\n");
            printf("Supported: uploadf, downlf, removef, downltar, dispfnames\n");
        }
    }

    close(sock);
    return 0;
}