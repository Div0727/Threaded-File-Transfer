#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <system_error>
#include <fstream>
#include <filesystem>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

class DownloadServer {
private:
    std::mutex transferMutex;
    SOCKET listenSocket;
    sockaddr_in serverAddr;
    
    struct ThreadParams {
        SOCKET connectionFd;
        int threadId;
        int totalThreads;
        std::string filename;
        std::streampos position;
        size_t bytesToSend;
    };

    void sendFile(const ThreadParams& params) {
        try {
            // Get file information
            fs::path filePath(params.filename);
            if (!fs::exists(filePath)) {
                throw std::runtime_error("File does not exist");
            }
    
            auto fileSize = fs::file_size(filePath);
            
            // Calculate portion to send
            size_t bytesToSend;
            std::streampos position;
            
            // Calculate chunk size and position
            position = params.position;
            if (params.threadId == params.totalThreads - 1) {
                bytesToSend = static_cast<size_t>(fileSize) - position;
            } else {
                bytesToSend = static_cast<size_t>(fileSize) / params.totalThreads;
            }
            
            // Open file (without mutex - each thread has its own file handle)
            std::ifstream file(filePath, std::ios::binary);
            if (!file) {
                throw std::runtime_error("Failed to open file");
            }
    
            // Set file position
            file.seekg(position);
            
            // Send combined header (size + filename)
            std::string header = "SIZE:" + std::to_string(bytesToSend) + 
                               ":FILENAME:" + params.filename + "\n";
            
            if (send(params.connectionFd, header.c_str(), header.size(), 0) == SOCKET_ERROR) {
                throw std::system_error(WSAGetLastError(), std::system_category(), "Header send failed");
            }
    
            // Send file data
            char buffer[8192]; // Increased buffer size for better performance
            size_t remaining = bytesToSend;
            size_t totalSent = 0;
            
            while (remaining > 0) {
                size_t chunkSize = std::min(sizeof(buffer), remaining);
                file.read(buffer, chunkSize);
                size_t bytesRead = file.gcount();
                
                if (bytesRead == 0) {
                    break; // End of file reached unexpectedly
                }
                
                size_t bytesWritten = 0;
                while (bytesWritten < bytesRead) {
                    int bytesSent = send(params.connectionFd, buffer + bytesWritten, bytesRead - bytesWritten, 0);
                    if (bytesSent <= 0) {
                        if (WSAGetLastError() == WSAEWOULDBLOCK) {
                            // Socket buffer is full, wait a bit
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            continue;
                        }
                        throw std::system_error(WSAGetLastError(), std::system_category(), "Data send failed");
                    }
                    
                    bytesWritten += bytesSent;
                }
                
                totalSent += bytesRead;
                remaining -= bytesRead;
                
                int progress = static_cast<int>((totalSent * 100) / bytesToSend);
                std::cout << "\rThread " << params.threadId << ": " << progress << "%" << std::flush;
            }
    
            std::cout << "\nThread " << params.threadId << " completed. Sent "
                      << totalSent << "/" << bytesToSend << " bytes" << std::endl;

            // Proper connection teardown
            shutdown(params.connectionFd, SD_SEND);
            file.close();
        } catch (const std::exception& e) {
            std::cerr << "Thread " << params.threadId << " error: " << e.what() << std::endl;
        }

        closesocket(params.connectionFd);
    }

public:
    DownloadServer(int port) {
        // Initialize Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::system_error(WSAGetLastError(), std::system_category(), "WSAStartup failed");
        }

        // Create socket with reuse option
        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            WSACleanup();
            throw std::system_error(WSAGetLastError(), std::system_category(), "Socket creation failed");
        }

        // Allow port reuse
        int optval = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));

        // Configure server address
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        serverAddr.sin_port = htons(port);

        // Bind socket
        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(listenSocket);
            WSACleanup();
            throw std::system_error(WSAGetLastError(), std::system_category(), "Bind failed");
        }

        // Start listening with a backlog of SOMAXCONN
        if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listenSocket);
            WSACleanup();
            throw std::system_error(WSAGetLastError(), std::system_category(), "Listen failed");
        }
    }

    ~DownloadServer() {
        if (listenSocket != INVALID_SOCKET) {
            closesocket(listenSocket);
        }
        WSACleanup();
    }

    void run() {
        std::cout << "Server started. Waiting for connections..." << std::endl;

        while (true) {
            sockaddr_in clientAddr;
            int clientLen = sizeof(clientAddr);
            
            SOCKET connectionFd = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (connectionFd == INVALID_SOCKET) {
                std::cerr << "Accept failed: " << WSAGetLastError() << std::endl;
                continue;
            }

            // Set non-blocking socket
            u_long mode = 1;
            ioctlsocket(connectionFd, FIONBIO, &mode);

            // Set send timeout (5000ms)
            DWORD timeout = 5000;
            setsockopt(connectionFd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
            
            // Set receive timeout (5000ms)
            setsockopt(connectionFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

            // Receive client message
            char clientMessage[4096] = {0}; // Increased buffer size
            int bytesReceived = 0;
            
            // Receive with timeout handling
            auto startTime = std::chrono::steady_clock::now();
            bool receivedComplete = false;
            
            while (!receivedComplete) {
                int result = recv(connectionFd, clientMessage + bytesReceived, 
                              sizeof(clientMessage) - bytesReceived - 1, 0);
                
                if (result > 0) {
                    bytesReceived += result;
                    receivedComplete = true; // We got data, assume it's complete
                } else if (result == 0) {
                    // Connection closed
                    break;
                } else {
                    int error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK) {
                        // Not ready yet, check timeout
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count() > 3000) {
                            std::cerr << "Timeout receiving client message" << std::endl;
                            closesocket(connectionFd);
                            continue;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    } else {
                        // Real error
                        std::cerr << "Receive error: " << error << std::endl;
                        closesocket(connectionFd);
                        continue;
                    }
                }
            }

            if (bytesReceived <= 0) {
                closesocket(connectionFd);
                std::cerr << "Failed to receive client message" << std::endl;
                continue;
            }

            // Ensure null termination
            clientMessage[bytesReceived] = '\0';

            // Parse client message (threadId,threadCount,filename)
            std::string message(clientMessage, bytesReceived);
            size_t pos1 = message.find(',');
            size_t pos2 = message.find(',', pos1 + 1);

            if (pos1 == std::string::npos || pos2 == std::string::npos) {
                closesocket(connectionFd);
                std::cerr << "Invalid client message format" << std::endl;
                continue;
            }

            ThreadParams params;
            params.connectionFd = connectionFd;
            
            try {
                params.threadId = std::stoi(message.substr(0, pos1));
                params.totalThreads = std::stoi(message.substr(pos1 + 1, pos2 - pos1 - 1));
                params.filename = message.substr(pos2 + 1);
            } catch (const std::exception& e) {
                closesocket(connectionFd);
                std::cerr << "Failed to parse client message: " << e.what() << std::endl;
                continue;
            }

            // Verify file exists before proceeding
            if (!fs::exists(params.filename)) {
                closesocket(connectionFd);
                std::cerr << "Requested file not found: " << params.filename << std::endl;
                continue;
            }

            auto fileSize = fs::file_size(params.filename);
            params.position = (params.threadId * (fileSize / params.totalThreads));
            
            // Compute bytes to send for this thread
            if (params.threadId == params.totalThreads - 1) {
                params.bytesToSend = fileSize - params.position;
            } else {
                params.bytesToSend = fileSize / params.totalThreads;
            }

            // Start thread to handle this connection
            std::thread(&DownloadServer::sendFile, this, params).detach();
        }
    }
};

int main() {
    try {
        DownloadServer server(8000);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}