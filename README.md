# ⚡ Threaded-File-Transfer in C++

This projects demonstrates a simple threaded file transfer using C++ and raw Winsock sockets on windows.

💡 Features

- File downloading via TCP sockets
- Multithreaded parallel downloads (configurable number of threads)
- File size splitting and partial download logic
- Merging downloaded parts into a final file
- Cross-verification via server-side logs
- Built with pure C++ and Windows Sockets API (Winsock2)

File Structure
downloads/         # Where downloaded file will be merged
test_files/        # Sample test files
client.cpp         # Client-side logic
server.cpp         # Server-side logic


Server Side Output showing the thread download process from the server using 5 threads.
![Screenshot 2025-05-05 023750](https://github.com/user-attachments/assets/7a130384-a85a-4ed1-8742-01f674d855ff)

Client Side Output showing the simultaneous client connections each connection sends a portion of the file, showing efficient transfer.

![Screenshot 2025-05-05 023722](https://github.com/user-attachments/assets/ae5acde8-aa45-43ee-b10a-cacc79075ba2)


