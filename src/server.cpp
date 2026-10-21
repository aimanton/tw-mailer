#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <port> <mail-spool-directoryname>"
              << "\n";
    return 1;
  }

  // Port logic
  int port;
  try {
    port = std::stoi(argv[1]);

  } catch (std::exception& e) {
    std::cerr << "Exception caught: " << e.what() << "\n";
    return 1;
  }

  if (port < 1023 || port > 65535) {
    std::cerr << "Provided port: " << argv[1] << " is not a valid port number"
              << "\n";
    return 1;
  }

  // Creating Socket
  sockaddr_in serverAddr;
  memset(&serverAddr, 0, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(port);

  int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

  if (serverSocket == -1) {
    std::cerr << "serverSocket creation threw an error: " << strerror(errno)
              << "\n";
    return 1;
  }

  // Initiate binding
  int bindResult =
      bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));

  if (bindResult == -1) {
    std::cerr << "Binding threw an error: " << strerror(errno) << "\n";
    return 1;
  }

  // Listening on port
  int listenResult = listen(serverSocket, 5);

  if (listenResult == -1) {
    std::cerr << "Listening threw an error: " << strerror(errno) << "\n";
    return 1;
  }

  // Accepting clientSocket
  sockaddr_in clientAddr;
  socklen_t clientAddrLen = sizeof(clientAddr);

  int clientSocket =
      accept(serverSocket, (sockaddr*)&clientAddr, &clientAddrLen);

  if (clientSocket == -1) {
    std::cerr << "Accepting client threw an error: " << strerror(errno) << "\n";
    close(serverSocket);
    return 1;
  }

  // Printing connected Client
  std::cout << "Client connected -> IP: " << inet_ntoa(clientAddr.sin_addr)
            << " Port: " << ntohs(clientAddr.sin_port) << "\n";

  std::cout << "serverSocket: " << serverSocket << "\n";

  std::cout << argv[1] << "\n";
  std::cout << argv[2] << "\n";

  // Closing Sockets
  close(serverSocket);
  close(clientSocket);
  return 0;
}
