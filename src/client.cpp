#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// HELPERS
bool sendLine(int sock, const std::string& text);
bool isValidUsername(const std::string& name);
bool askUsername(const std::string& prompt, std::string& out);
bool askSubject(std::string& out);

enum ReadResult { LINE_OK, LINE_CLOSED, LINE_ERROR };
constexpr size_t MAX_LINE = 8192;

ReadResult readLine(int sock, std::string& pending, std::string& line);
bool isValidMessageNumber(const std::string& number);
bool askMessageNumber(std::string& out);
bool handleRead(int sock, std::string& pending);
// HANDLERS
bool handleSend(int sock);
bool handleList(int sock, std::string& pending);
// TODO:
// - handleSend
// - handleDel

constexpr int BUF = 1024;

enum Commands { SEND, LIST, READ, DEL, QUIT };

std::string commandToString(Commands command);
std::string commandToString(Commands command) {
  switch (command) {
  case SEND:
    return "SEND";
  case LIST:
    return "LIST";
  case READ:
    return "READ";
  case DEL:
    return "DEL";
  case QUIT:
    return "QUIT";
  }
  return "";
}

int main(int argc, char** argv) {
  int create_socket;
  char buffer[BUF];
  sockaddr_in address{};
  int size;
  int isQuit = 0;

  // COMMAND LINE ARGUMENTS
  // ./twmailer-client <ip> <port>
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
    return EXIT_FAILURE;
  }

  // port: whole argument must be a number between 1 and 65535
  char* endPtr = nullptr;
  long portValue = strtol(argv[2], &endPtr, 10);
  if (argv[2][0] == '\0' || *endPtr != '\0' || portValue < 1 ||
      portValue > 65535) {
    fprintf(stderr, "Invalid port '%s' (expected a number from 1 to 65535)\n",
            argv[2]);
    return EXIT_FAILURE;
  }
  int port = static_cast<int>(portValue);

  // CREATE A SOCKET
  // https://man7.org/linux/man-pages/man2/socket.2.html
  // https://man7.org/linux/man-pages/man7/ip.7.html
  // https://man7.org/linux/man-pages/man7/tcp.7.html
  // IPv4, TCP (connection oriented), IP (same as server)
  if ((create_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("Socket error");
    return EXIT_FAILURE;
  }

  // INIT ADDRESS
  // Attention: network byte order => big endian
  memset(&address, 0, sizeof(address)); // init storage with 0
  address.sin_family = AF_INET;         // IPv4
  // https://man7.org/linux/man-pages/man3/htons.3.html
  address.sin_port = htons(port);
  // https://man7.org/linux/man-pages/man3/inet_aton.3.html
  // returns 0 if the string is not a valid IPv4 address
  if (inet_aton(argv[1], &address.sin_addr) == 0) {
    fprintf(stderr, "Invalid IP address '%s'\n", argv[1]);
    close(create_socket);
    return EXIT_FAILURE;
  }

  // CREATE A CONNECTION
  // https://man7.org/linux/man-pages/man2/connect.2.html
  if (connect(create_socket, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) == -1) {
    // https://man7.org/linux/man-pages/man3/perror.3.html
    perror("Connect error - no server available");
    return EXIT_FAILURE;
  }

  // ignore return value of printf
  printf("Connection with server (%s) established\n",
         inet_ntoa(address.sin_addr));

  // RECEIVE DATA
  // https://man7.org/linux/man-pages/man2/recv.2.html
  size = recv(create_socket, buffer, BUF - 1, 0);
  if (size == -1) {
    perror("recv error");
  } else if (size == 0) {
    printf("Server closed remote socket\n"); // ignore error
  } else {
    buffer[size] = '\0';
    printf("%s", buffer); // ignore error
  }

  std::string pending;
  do {
    printf(">> ");
    if (fgets(buffer, BUF - 1, stdin) != nullptr) {
      int size = static_cast<int>(strlen(buffer));
      // remove new-line signs from string at the end
      if (size >= 2 && buffer[size - 2] == '\r' && buffer[size - 1] == '\n') {
        size -= 2;
        buffer[size] = 0;
      } else if (size >= 1 && buffer[size - 1] == '\n') {
        --size;
        buffer[size] = 0;
      }

      std::string command(buffer);

      if (command == commandToString(SEND)) {
        if (!handleSend(create_socket))
          break;
      } else if (command == commandToString(LIST)) {
        if (!handleList(create_socket, pending))
          break;
      } else if (command == commandToString(READ)) {
        if (!handleRead(create_socket, pending))
          break;

      } else if (command == commandToString(DEL)) {

      } else if (command == commandToString(QUIT)) {
        sendLine(create_socket, command);
        isQuit = 1;
      } else {
        std::cerr << "Unknown command\n";
      }
    }
  } while (!isQuit);

  ////////////////////////////////////////////////////////////////////////////
  // CLOSES THE DESCRIPTOR
  if (create_socket != -1) {
    if (shutdown(create_socket, SHUT_RDWR) == -1) {
      // invalid in case the server is gone already
      perror("shutdown create_socket");
    }
    if (close(create_socket) == -1) {
      perror("close create_socket");
    }
    create_socket = -1;
  }

  return EXIT_SUCCESS;
}

bool sendLine(int sock, const std::string& text) {
  std::string data = text + "\n";
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = send(sock, data.c_str() + sent, data.size() - sent, 0);
    if (n == -1) {
      perror("send error");
      return false;
    }
    sent += n;
  }
  return true;
}

bool isValidUsername(const std::string& name) {
  if ((name.size() < 1 && name.size() > 8) || name.empty())
    return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
      return false;
  }
  return true;
}

bool askUsername(const std::string& prompt, std::string& out) {
  while (true) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, out))
      return false;
    if (isValidUsername(out))
      return true;
    std::cerr << "Invalid name: 1-8 characters, only a-z and 0-9.\n";
  }
}

bool askSubject(std::string& out) {
  while (true) {
    std::cout << "Subject: " << std::flush;
    if (!std::getline(std::cin, out))
      return false;
    if (!out.empty() && out.size() <= 80)
      return true;
    std::cerr << "Invalid subject: 1-80 characters.\n";
  }
}

bool handleSend(int sock) {
  std::string sender, receiver, subject, bodyLine;

  if (!sendLine(sock, commandToString(SEND)))
    return false;
  if (!askUsername("Sender: ", sender) || !sendLine(sock, sender))
    return false;
  if (!askUsername("Receiver: ", receiver) || !sendLine(sock, receiver))
    return false;
  if (!askSubject(subject) || !sendLine(sock, subject))
    return false;

  std::cout << "Message (end with a single '.' on its own line):\n";
  do {
    if (!std::getline(std::cin, bodyLine))
      return false;
    if (!sendLine(sock, bodyLine))
      return false;
  } while (bodyLine != ".");

  // the server answers OK or ERR once the message is complete
  char reply[BUF];
  ssize_t size = recv(sock, reply, BUF - 1, 0);
  if (size <= 0) {
    std::cerr << "Server closed the connection or recv failed\n";
    return false;
  }
  reply[size] = '\0';
  std::cout << "<< " << reply;
  return true;
}

// Returns exactly one line (without '\n' / '\r'). 'pending' keeps bytes that
// were received but not handed out yet; it lives as long as the connection.
ReadResult readLine(int sock, std::string& pending, std::string& line) {
  while (true) {
    size_t pos = pending.find('\n');
    if (pos != std::string::npos) {
      line = pending.substr(0, pos);
      pending.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return LINE_OK;
    }
    if (pending.size() > MAX_LINE) {
      std::cerr << "Server sent a line that is too long\n";
      return LINE_ERROR;
    }
    char chunk[BUF];
    ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
    if (n == -1) {
      perror("recv error");
      return LINE_ERROR;
    }
    if (n == 0) {
      return LINE_CLOSED;
    }
    pending.append(chunk, n);
  }
}

// only digits, 1-9 characters, value at least 1
bool isValidMessageNumber(const std::string& number) {
  if (number.empty() || number.size() > 9) {
    return false;
  }
  for (char c : number) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return std::stoi(number) >= 1;
}

bool askMessageNumber(std::string& out) {
  while (true) {
    std::cout << "Message-Number: " << std::flush;
    if (!std::getline(std::cin, out)) {
      return false;
    }
    if (isValidMessageNumber(out)) {
      return true;
    }
    std::cerr << "Invalid number: 1-9 digits, at least 1.\n";
  }
}

bool handleRead(int sock, std::string& pending) {
  std::string username, messageNumber, line;

  if (!sendLine(sock, commandToString(READ)))
    return false;
  if (!askUsername("Username: ", username) || !sendLine(sock, username))
    return false;
  if (!askMessageNumber(messageNumber) || !sendLine(sock, messageNumber))
    return false;

  // first line of the answer: OK or ERR
  if (readLine(sock, pending, line) != LINE_OK) {
    std::cerr << "Server closed the connection or recv failed\n";
    return false;
  }
  if (line != "OK") {
    std::cout << "<< " << line << "\n"; // ERR: the session goes on
    return true;
  }

  // message: sender, receiver, subject, body lines, then a line with '.'
  std::cout << "<< OK\n";
  while (true) {
    if (readLine(sock, pending, line) != LINE_OK) {
      std::cerr << "Connection lost while reading the message\n";
      return false;
    }
    if (line == ".") {
      break;
    }
    std::cout << line << "\n";
  }
  return true;
}

bool handleList(int sock, std::string& pending) {
  std::string username, line;

  if (!sendLine(sock, commandToString(LIST)))
    return false;
  if (!askUsername("Username: ", username) || !sendLine(sock, username))
    return false;

  // first line of the answer: number of subjects that follow
  if (readLine(sock, pending, line) != LINE_OK) {
    std::cerr << "Server closed the connection or recv failed\n";
    return false;
  }
  if (line != "0" && !isValidMessageNumber(line)) {
    std::cout << "<< " << line << "\n"; // no count: nothing else will follow
    return true;
  }

  int count = std::stoi(line);
  std::cout << "<< " << count << " message(s)\n";
  for (int i = 1; i <= count; ++i) {
    if (readLine(sock, pending, line) != LINE_OK) {
      std::cerr << "Connection lost while reading the list\n";
      return false;
    }
    // same number that READ expects
    std::cout << i << ": " << line << "\n";
  }
  return true;
}
