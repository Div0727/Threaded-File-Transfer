#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>
#include <chrono>
#include <filesystem>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

class DownloadClient {
private:
    std::mutex fileMutex;  // Mutex for file access
    std::string ipAddress;
    std::string filename;
    int threadCount;
    std::string outputDir;
    
    void handleConnection(int threadId) {
        try {
            // Initialize socket with timeout
            SOCKET sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sockfd == INVALID_SOCKET) {
                throw std::system_error(WSAGetLastError(), std::system_category(), "Could not create socket");
            }

            // Set socket timeout (5000ms - increased for reliability)
            DWORD timeout = 5000;
            setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

            // Configure server address
            sockaddr_in serv_addr;
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(8000);
            
            if (inet_pton(AF_INET, ipAddress.c_str(), &serv_addr.sin_addr) <= 0) {
                closesocket(sockfd);
                throw std::runtime_error("Invalid address");
            }

            // Connection with retries
            int retries = 3;
            while (retries-- > 0) {
                if (connect(sockfd, (sockaddr*)&serv_addr, sizeof(serv_addr)) != SOCKET_ERROR) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            
            if (retries < 0) {
                int error = WSAGetLastError();
                closesocket(sockfd);
                throw std::system_error(error, std::system_category(), "Connection failed after retries");
            }

            // Send request (threadId,threadCount,filename)
            std::string request = std::to_string(threadId) + "," + 
                                 std::to_string(threadCount) + "," + 
                                 filename;
            
            if (send(sockfd, request.c_str(), request.size(), 0) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                closesocket(sockfd);
                throw std::system_error(error, std::system_category(), "Request failed");
            }

            // Receive response header
            std::vector<char> headerBuffer;
            char buffer[1024];
            bool headerComplete = false;
            size_t expectedSize = 0;
            std::string partFilename = "";
            
            // Read until we find the newline that terminates the header
            while (!headerComplete) {
                int bytesReceived = recv(sockfd, buffer, sizeof(buffer), 0);
                if (bytesReceived <= 0) {
                    closesocket(sockfd);
                    throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to receive header");
                }
                
                // Add to our header buffer
                headerBuffer.insert(headerBuffer.end(), buffer, buffer + bytesReceived);
                
                // Check if we have a complete header
                std::string headerStr(headerBuffer.begin(), headerBuffer.end());
                size_t endOfHeader = headerStr.find('\n');
                if (endOfHeader != std::string::npos) {
                    // We have the complete header
                    headerComplete = true;
                    
                    // Parse the size
                    size_t sizeStart = headerStr.find("SIZE:") + 5;
                    size_t sizeEnd = headerStr.find(":", sizeStart);
                    expectedSize = std::stoull(headerStr.substr(sizeStart, sizeEnd - sizeStart));
                    
                    // Parse the filename
                    size_t filenameStart = headerStr.find("FILENAME:") + 9;
                    partFilename = headerStr.substr(filenameStart, endOfHeader - filenameStart);
                    
                    // Any remaining data is part of the file content
                    headerBuffer.erase(headerBuffer.begin(), headerBuffer.begin() + endOfHeader + 1);
                }
            }
            
            // Create part filename for this thread
            std::string threadFilename = outputDir + "/" + fs::path(partFilename).filename().string() + 
                                       ".part" + std::to_string(threadId);
            
            // Create output directory if it doesn't exist
            {
                std::lock_guard<std::mutex> lock(fileMutex);
                if (!fs::exists(outputDir)) {
                    fs::create_directories(outputDir);
                }
            }
            
            // Open output file for this thread's part
            std::ofstream outputFile(threadFilename, std::ios::binary | std::ios::trunc);
            if (!outputFile) {
                closesocket(sockfd);
                throw std::runtime_error("Cannot create output file: " + threadFilename);
            }

            // Write any data we already received
            if (!headerBuffer.empty()) {
                outputFile.write(headerBuffer.data(), headerBuffer.size());
            }
            
            // Track total received
            size_t totalReceived = headerBuffer.size();
            
            // Receive file data
            bool transferComplete = false;
            
            while (!transferComplete && totalReceived < expectedSize) {
                int bytesToReceive = std::min(sizeof(buffer), expectedSize - totalReceived);
                int bytesReceived = recv(sockfd, buffer, bytesToReceive, 0);
                
                if (bytesReceived > 0) {
                    outputFile.write(buffer, bytesReceived);
                    totalReceived += bytesReceived;
                    
                    // Update progress
                    int progress = static_cast<int>((totalReceived * 100) / expectedSize);
                    std::cout << "\rThread " << threadId << ": " << progress << "%" << std::flush;
                } 
                else if (bytesReceived == 0) {
                    // Normal closure
                    transferComplete = true;
                }
                else {
                    int error = WSAGetLastError();
                    if (error == WSAETIMEDOUT) {
                        std::cerr << "\nThread " << threadId << " timeout, retrying..." << std::endl;
                        continue;
                    }
                    throw std::system_error(error, std::system_category(), "Transfer error");
                }
            }

            // Verify complete transfer
            if (totalReceived != expectedSize) {
                outputFile.close();
                closesocket(sockfd);
                throw std::runtime_error("Incomplete transfer: received " + 
                                       std::to_string(totalReceived) + " of " + 
                                       std::to_string(expectedSize) + " bytes");
            }

            std::cout << "\nThread " << threadId << " completed. Received " 
                      << totalReceived << "/" << expectedSize << " bytes\n";

            // Cleanup
            outputFile.close();
            shutdown(sockfd, SD_SEND);
            closesocket(sockfd);

        } catch (const std::exception& e) {
            std::cerr << "Thread " << threadId << " error: " << e.what() << std::endl;
        }
    }

public:
    DownloadClient(const std::string& ip, int threads, const std::string& file)
        : ipAddress(ip), threadCount(threads), filename(file), outputDir("downloads") {}

    void start() {
        system("cls");
        
        std::vector<std::thread> threads;
        threads.reserve(threadCount);

        // Create threads with staggered startup
        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back(&DownloadClient::handleConnection, this, i);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Join threads
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        
        // After all threads are done, merge the parts
        mergeFiles();
    }
    
    void mergeFiles() {
        std::cout << "\nMerging file parts..." << std::endl;
        
        // Extract base filename from the path
        std::string baseFilename = fs::path(filename).filename().string();
        std::string outputFilePath = outputDir + "/" + baseFilename;
        
        try {
            // Open output file
            std::ofstream output(outputFilePath, std::ios::binary);
            if (!output) {
                throw std::runtime_error("Cannot create output file: " + outputFilePath);
            }
            
            // Read and append each part
            for (int i = 0; i < threadCount; ++i) {
                std::string partFilename = outputDir + "/" + baseFilename + ".part" + std::to_string(i);
                
                if (!fs::exists(partFilename)) {
                    throw std::runtime_error("Missing part file: " + partFilename);
                }
                
                std::ifstream partFile(partFilename, std::ios::binary);
                if (!partFile) {
                    throw std::runtime_error("Cannot open part file: " + partFilename);
                }
                
                output << partFile.rdbuf();
                partFile.close();
                
                // Delete part file
                fs::remove(partFilename);
            }
            
            output.close();
            std::cout << "File successfully downloaded and merged: " << outputFilePath << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "Error merging files: " << e.what() << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
        return 1;
    }

    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <IP> <thread_count> <filename>" << std::endl;
        WSACleanup();
        return 1;
    }

    try {
        DownloadClient client(argv[1], std::stoi(argv[2]), argv[3]);
        client.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        WSACleanup();
        return 1;
    }

    WSACleanup();
    return 0;
}