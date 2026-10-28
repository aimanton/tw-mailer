#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

///////////////////////////////////////////////////////////////////////////////

constexpr int BUF = 1024;

///////////////////////////////////////////////////////////////////////////////

// volatile sig_atomic_t: the only type that is safe to write in a signal
// handler
volatile sig_atomic_t abortRequested = 0;
int create_socket = -1;
int new_socket = -1;

// base folder for the mail storage (2nd command line argument)
std::string spoolDirectory;

///////////////////////////////////////////////////////////////////////////////

void* clientCommunication(void* data);
void signalHandler(int sig);

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[]) {
  socklen_t addrlen;
  sockaddr_in address{};
  sockaddr_in cliaddress{};
  int reuseValue = 1;

  ////////////////////////////////////////////////////////////////////////////
  // COMMAND LINE ARGUMENTS
  // ./twmailer-server <port> <mail-spool-directoryname>
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <port> <mail-spool-directoryname>\n", argv[0]);
    return EXIT_FAILURE;
  }

  // port: whole argument must be a number between 1 and 65535
  char* endPtr = nullptr;
  long portValue = strtol(argv[1], &endPtr, 10);
  if (argv[1][0] == '\0' || *endPtr != '\0' || portValue < 1 ||
      portValue > 65535) {
    fprintf(stderr, "Invalid port '%s' (expected a number from 1 to 65535)\n",
            argv[1]);
    return EXIT_FAILURE;
  }
  int port = static_cast<int>(portValue);

  // spool directory: used later to build the paths of the stored messages
  spoolDirectory = argv[2];

  ////////////////////////////////////////////////////////////////////////////
  // SIGNAL HANDLER
  // SIGINT (Interrupt: ctrl+c)
  // https://man7.org/linux/man-pages/man2/signal.2.html
  if (signal(SIGINT, signalHandler) == SIG_ERR) {
    perror("signal can not be registered");
    return EXIT_FAILURE;
  }

  ////////////////////////////////////////////////////////////////////////////
  // CREATE A SOCKET
  // https://man7.org/linux/man-pages/man2/socket.2.html
  // https://man7.org/linux/man-pages/man7/ip.7.html
  // https://man7.org/linux/man-pages/man7/tcp.7.html
  // IPv4, TCP (connection oriented), IP (same as client)
  if ((create_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("Socket error"); // errno set by socket()
    return EXIT_FAILURE;
  }

  ////////////////////////////////////////////////////////////////////////////
  // SET SOCKET OPTIONS
  // https://man7.org/linux/man-pages/man2/setsockopt.2.html
  // https://man7.org/linux/man-pages/man7/socket.7.html
  // socket, level, optname, optvalue, optlen
  if (setsockopt(create_socket, SOL_SOCKET, SO_REUSEADDR, &reuseValue,
                 sizeof(reuseValue)) == -1) {
    perror("set socket options - reuseAddr");
    return EXIT_FAILURE;
  }

  if (setsockopt(create_socket, SOL_SOCKET, SO_REUSEPORT, &reuseValue,
                 sizeof(reuseValue)) == -1) {
    perror("set socket options - reusePort");
    return EXIT_FAILURE;
  }

  ////////////////////////////////////////////////////////////////////////////
  // INIT ADDRESS
  // Attention: network byte order => big endian
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  ////////////////////////////////////////////////////////////////////////////
  // ASSIGN AN ADDRESS WITH PORT TO SOCKET
  if (bind(create_socket, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) == -1) {
    perror("bind error");
    return EXIT_FAILURE;
  }

  ////////////////////////////////////////////////////////////////////////////
  // ALLOW CONNECTION ESTABLISHING
  // Socket, Backlog (= count of waiting connections allowed)
  if (listen(create_socket, 5) == -1) {
    perror("listen error");
    return EXIT_FAILURE;
  }

  while (!abortRequested) {
    /////////////////////////////////////////////////////////////////////////
    // ignore errors here... because only information message
    // https://linux.die.net/man/3/printf
    printf("Waiting for connections...\n");

    /////////////////////////////////////////////////////////////////////////
    // ACCEPTS CONNECTION SETUP
    // blocking, might have an accept-error on ctrl+c
    addrlen = sizeof(sockaddr_in);
    if ((new_socket =
             accept(create_socket, reinterpret_cast<sockaddr*>(&cliaddress),
                    &addrlen)) == -1) {
      if (abortRequested) {
        perror("accept error after aborted");
      } else {
        perror("accept error");
      }
      break;
    }

    /////////////////////////////////////////////////////////////////////////
    // START CLIENT
    // ignore printf error handling
    printf("Client connected from %s:%d...\n", inet_ntoa(cliaddress.sin_addr),
           ntohs(cliaddress.sin_port));
    clientCommunication(&new_socket); // returnValue can be ignored
    new_socket = -1;
  }

  // frees the descriptor
  if (create_socket != -1) {
    if (shutdown(create_socket, SHUT_RDWR) == -1) {
      perror("shutdown create_socket");
    }
    if (close(create_socket) == -1) {
      perror("close create_socket");
    }
    create_socket = -1;
  }

  return EXIT_SUCCESS;
}

void* clientCommunication(void* data) {
  char buffer[BUF];
  int size;
  int* current_socket = static_cast<int*>(data);

  ////////////////////////////////////////////////////////////////////////////
  // SEND welcome message
  strcpy(buffer, "Welcome to myserver!\r\nPlease enter your commands...\r\n");
  if (send(*current_socket, buffer, strlen(buffer), 0) == -1) {
    perror("send failed");
    return nullptr;
  }

  do {
    /////////////////////////////////////////////////////////////////////////
    // RECEIVE
    size = recv(*current_socket, buffer, BUF - 1, 0);
    if (size == -1) {
      if (abortRequested) {
        perror("recv error after aborted");
      } else {
        perror("recv error");
      }
      break;
    }

    if (size == 0) {
      printf("Client closed remote socket\n"); // ignore error
      break;
    }

    // remove ugly debug message, because of the sent newline of client
    if (size >= 2 && buffer[size - 2] == '\r' && buffer[size - 1] == '\n') {
      size -= 2;
    } else if (buffer[size - 1] == '\n') {
      --size;
    }

    buffer[size] = '\0';
    printf("Message received: %s\n", buffer); // ignore error

    if (send(*current_socket, "OK", 3, 0) == -1) {
      perror("send answer failed");
      return nullptr;
    }
  } while (strcmp(buffer, "quit") != 0 && !abortRequested);

  // closes/frees the descriptor if not already
  if (*current_socket != -1) {
    if (shutdown(*current_socket, SHUT_RDWR) == -1) {
      perror("shutdown new_socket");
    }
    if (close(*current_socket) == -1) {
      perror("close new_socket");
    }
    *current_socket = -1;
  }

  return nullptr;
}

void signalHandler(int sig) {
  if (sig == SIGINT) {
    printf("abort Requested... "); // ignore error
    abortRequested = 1;
    /////////////////////////////////////////////////////////////////////////
    // With shutdown() one can initiate normal TCP close sequence ignoring
    // the reference count.
    // https://beej.us/guide/bgnet/html/#close-and-shutdownget-outta-my-face
    // https://linux.die.net/man/3/shutdown
    if (new_socket != -1) {
      if (shutdown(new_socket, SHUT_RDWR) == -1) {
        perror("shutdown new_socket");
      }
      if (close(new_socket) == -1) {
        perror("close new_socket");
      }
      new_socket = -1;
    }

    if (create_socket != -1) {
      if (shutdown(create_socket, SHUT_RDWR) == -1) {
        perror("shutdown create_socket");
      }
      if (close(create_socket) == -1) {
        perror("close create_socket");
      }
      create_socket = -1;
    }
  } else {
    exit(sig);
  }
}
