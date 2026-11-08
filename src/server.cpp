#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

///////////////////////////////////////////////////////////////////////////////

constexpr int BUF = 1024;
// longest line the server accepts. Protects against a client that sends endless
// data without a newline (the receive buffer would grow forever).
constexpr size_t MAX_LINE = 8192;
constexpr size_t MAX_USERNAME = 8;
constexpr size_t MAX_SUBJECT = 80;

///////////////////////////////////////////////////////////////////////////////

// volatile sig_atomic_t: the only type that is safe to write in a signal
// handler
volatile sig_atomic_t abortRequested = 0;
int create_socket = -1;
int new_socket = -1;

// base folder for the mail storage (2nd command line argument)
std::string spoolDirectory;

///////////////////////////////////////////////////////////////////////////////

enum ReadResult { LINE_OK, LINE_CLOSED, LINE_ERROR };

///////////////////////////////////////////////////////////////////////////////
/// SOCKET HELPERS
bool sendAll(int sock, const std::string& data);
bool sendLine(int sock, const std::string& text);
ReadResult readLine(int sock, std::string& pending, std::string& line);

/// VALIDATION
bool isValidUsername(const std::string& name);
bool isValidSubject(const std::string& subject);
bool isValidMessageNumber(const std::string& text, int& valueOut);

/// STORAGE
bool ensureDir(const std::string& path);
bool ensureMailbox(const std::string& user, const std::string& box,
                   std::string& dirOut);
std::string sanitizeForFilename(const std::string& text);
std::string timestamp();
bool writeUnique(const std::string& dir, const std::string& name,
                 const std::string& content, std::string& pathOut);
bool storeMessage(const std::string& sender, const std::string& receiver,
                  const std::string& subject, const std::string& body);
std::string sortKey(const std::string& fileName);
bool listMessages(const std::string& user, const std::string& box,
                  std::vector<std::string>& files);
bool readFile(const std::string& path, std::string& content);
std::string readSubject(const std::string& path);

/// COMMAND HANDLERS
bool handleSend(int sock, std::string& pending);
bool handleRead(int sock, std::string& pending);
bool handleList(int sock, std::string& pending);
// TODO:
// - handleDel

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

///////////////////////////////////////////////////////////////////////////////
/// SOCKET HELPERS

// Sends all bytes of data. send() may transmit only a part per call, so loop.
bool sendAll(int sock, const std::string& data) {
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

// Sends the text followed by '\n' (the line delimiter of the protocol).
bool sendLine(int sock, const std::string& text) {
  return sendAll(sock, text + "\n");
}

// Returns exactly one line (without '\n' and without a trailing '\r').
// 'pending' holds bytes that were already received but not handed out yet.
// It belongs to ONE connection and must live as long as that connection.
ReadResult readLine(int sock, std::string& pending, std::string& line) {
  while (true) {
    // 1. complete line already in the buffer? -> hand it out, keep the rest
    size_t pos = pending.find('\n');
    if (pos != std::string::npos) {
      line = pending.substr(0, pos);
      pending.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return LINE_OK;
    }

    // 2. no newline yet, but already too much data -> refuse
    if (pending.size() > MAX_LINE) {
      fprintf(stderr, "Line longer than %zu bytes, closing connection\n",
              MAX_LINE);
      return LINE_ERROR;
    }

    // 3. get more data and look again
    char chunk[BUF];
    ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
    if (n == -1) {
      if (abortRequested) {
        perror("recv error after aborted");
      } else {
        perror("recv error");
      }
      return LINE_ERROR;
    }
    if (n == 0) {
      return LINE_CLOSED; // an incomplete line in 'pending' is dropped
    }
    pending.append(chunk, n);
  }
}

///////////////////////////////////////////////////////////////////////////////
/// VALIDATION

// 1-8 characters, only a-z and 0-9
bool isValidUsername(const std::string& name) {
  if (name.empty() || name.size() > MAX_USERNAME) {
    return false;
  }
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  return true;
}

// 1-80 characters
bool isValidSubject(const std::string& subject) {
  return !subject.empty() && subject.size() <= MAX_SUBJECT;
}

// Message number: only digits, 1-9 characters, value at least 1.
// (9 digits always fit into an int)
bool isValidMessageNumber(const std::string& text, int& valueOut) {
  if (text.empty() || text.size() > 9) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  valueOut = std::stoi(text);
  return valueOut >= 1;
}

///////////////////////////////////////////////////////////////////////////////
/// STORAGE
///
/// Layout:
///   <spool>/<user>/sent/<subject>-<timestamp>.txt
///   <spool>/<user>/received/<subject>-<timestamp>.txt
///
/// File content (the message as defined in the protocol):
///   <sender>\n
///   <receiver>\n
///   <subject>\n
///   <body line 1>\n
///   ...

// Creates a directory if it does not exist yet. "Already exists" is no error.
bool ensureDir(const std::string& path) {
  // https://man7.org/linux/man-pages/man2/mkdir.2.html
  if (mkdir(path.c_str(), 0755) == -1 && errno != EEXIST) {
    fprintf(stderr, "Cannot create directory '%s': %s\n", path.c_str(),
            strerror(errno));
    return false;
  }
  return true;
}

// Creates <spool>/<user>/<box> (box = "sent" or "received") and returns its
// path. The user name must be validated BEFORE (it becomes part of a path).
bool ensureMailbox(const std::string& user, const std::string& box,
                   std::string& dirOut) {
  std::string userDir = spoolDirectory + "/" + user;
  dirOut = userDir + "/" + box;
  return ensureDir(spoolDirectory) && ensureDir(userDir) && ensureDir(dirOut);
}

// The subject is typed by the client, so it must never reach the file system
// as it is (e.g. "../../x" would escape the spool folder).
// Everything except letters, digits and '_' becomes '_'.
std::string sanitizeForFilename(const std::string& text) {
  std::string result = text;
  for (char& c : result) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      c = '_';
    }
  }
  return result;
}

// Local time as YYYYMMDD-HHMMSS-mmm (milliseconds keep names unique)
std::string timestamp() {
  timeval tv{};
  gettimeofday(&tv, nullptr);
  tm tmValue{};
  localtime_r(&tv.tv_sec, &tmValue);
  char date[32];
  strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &tmValue);
  char result[48];
  snprintf(result, sizeof(result), "%s-%03d", date,
           static_cast<int>(tv.tv_usec / 1000));
  return result;
}

// Writes content into <dir>/<name>.txt. If that file exists already (same
// subject in the same millisecond) it tries <name>-1.txt, <name>-2.txt, ...
// Mode "wx" fails if the file exists, so nothing is ever overwritten.
bool writeUnique(const std::string& dir, const std::string& name,
                 const std::string& content, std::string& pathOut) {
  for (int i = 0; i < 100; ++i) {
    std::string path = dir + "/" + name;
    if (i > 0) {
      path += "-" + std::to_string(i);
    }
    path += ".txt";

    FILE* file = fopen(path.c_str(), "wx");
    if (file == nullptr) {
      if (errno == EEXIST) {
        continue;
      }
      fprintf(stderr, "Cannot create '%s': %s\n", path.c_str(),
              strerror(errno));
      return false;
    }

    bool ok = fwrite(content.data(), 1, content.size(), file) == content.size();
    if (fclose(file) != 0) {
      ok = false;
    }
    if (!ok) {
      fprintf(stderr, "Cannot write '%s'\n", path.c_str());
      remove(path.c_str());
      return false;
    }
    pathOut = path;
    return true;
  }
  fprintf(stderr, "No free file name in '%s'\n", dir.c_str());
  return false;
}

// Stores the message in the inbox of the receiver and in the sent folder of
// the sender. All or nothing: if the second write fails, the first is removed.
bool storeMessage(const std::string& sender, const std::string& receiver,
                  const std::string& subject, const std::string& body) {
  std::string content = sender + "\n" + receiver + "\n" + subject + "\n" + body;
  std::string name = sanitizeForFilename(subject) + "-" + timestamp();

  std::string receivedDir, sentDir;
  if (!ensureMailbox(receiver, "received", receivedDir) ||
      !ensureMailbox(sender, "sent", sentDir)) {
    return false;
  }

  std::string receivedPath, sentPath;
  if (!writeUnique(receivedDir, name, content, receivedPath)) {
    return false;
  }
  if (!writeUnique(sentDir, name, content, sentPath)) {
    remove(receivedPath.c_str());
    return false;
  }
  printf("Stored message: %s and %s\n", receivedPath.c_str(), sentPath.c_str());
  return true;
}

// Sort key of a file name "<subject>-<YYYYMMDD-HHMMSS-mmm>[-n].txt":
// the part after the first '-' (the subject has no '-', see
// sanitizeForFilename), without ".txt". So messages are ordered by arrival
// time. Without ".txt" the "-1" copy sorts after the original name.
std::string sortKey(const std::string& fileName) {
  std::string name = fileName;
  if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".txt") == 0) {
    name.erase(name.size() - 4);
  }
  size_t dash = name.find('-');
  return dash == std::string::npos ? name : name.substr(dash + 1);
}

// Fills 'files' with the names of all messages in <spool>/<user>/<box>,
// oldest first. The position in this list (starting at 1) is the message
// number, so LIST, READ and DEL must all use this function.
// A folder that does not exist is no error: the user simply has no messages.
// Returns false only on a real error. The user name must be validated BEFORE.
bool listMessages(const std::string& user, const std::string& box,
                  std::vector<std::string>& files) {
  namespace fs = std::filesystem;
  files.clear();

  std::string dir = spoolDirectory + "/" + user + "/" + box;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) {
      return true;
    }
    fprintf(stderr, "Cannot open directory '%s': %s\n", dir.c_str(),
            ec.message().c_str());
    return false;
  }

  fs::directory_iterator end;
  while (it != end) {
    if (it->is_regular_file(ec) && it->path().extension() == ".txt") {
      files.push_back(it->path().filename().string());
    }
    it.increment(ec);
    if (ec) {
      fprintf(stderr, "Cannot read directory '%s': %s\n", dir.c_str(),
              ec.message().c_str());
      return false;
    }
  }

  std::sort(files.begin(), files.end(),
            [](const std::string& a, const std::string& b) {
              std::string keyA = sortKey(a);
              std::string keyB = sortKey(b);
              return keyA != keyB ? keyA < keyB : a < b;
            });
  return true;
}

// Reads a whole file into 'content'.
bool readFile(const std::string& path, std::string& content) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    fprintf(stderr, "Cannot open '%s': %s\n", path.c_str(), strerror(errno));
    return false;
  }

  content.clear();
  char chunk[BUF];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) {
    content.append(chunk, n);
  }
  bool ok = ferror(file) == 0;
  fclose(file);
  if (!ok) {
    fprintf(stderr, "Cannot read '%s'\n", path.c_str());
  }
  return ok;
}

// Returns the subject of a stored message (3rd line of the file). A damaged
// message still gets a line in LIST, otherwise the numbers would not match
// the ones READ and DEL use.
std::string readSubject(const std::string& path) {
  const std::string unreadable = "(unreadable message)";
  std::string content;
  if (!readFile(path, content)) {
    return unreadable;
  }

  size_t afterSender = content.find('\n');
  if (afterSender == std::string::npos) {
    return unreadable;
  }
  size_t afterReceiver = content.find('\n', afterSender + 1);
  if (afterReceiver == std::string::npos) {
    return unreadable;
  }
  size_t start = afterReceiver + 1;
  size_t end = content.find('\n', start);
  return content.substr(start, end == std::string::npos ? end : end - start);
}

///////////////////////////////////////////////////////////////////////////////
/// COMMAND HANDLERS

// SEND\n<sender>\n<receiver>\n<subject>\n<body lines>\n.\n
// Returns false if the connection is gone (then there is nobody to answer).
bool handleSend(int sock, std::string& pending) {
  std::string sender, receiver, subject, body, line;

  if (readLine(sock, pending, sender) != LINE_OK ||
      readLine(sock, pending, receiver) != LINE_OK ||
      readLine(sock, pending, subject) != LINE_OK) {
    return false;
  }

  // ALWAYS read up to the final '.', even if a field turns out to be invalid.
  // Otherwise the remaining lines would be interpreted as commands.
  while (true) {
    if (readLine(sock, pending, line) != LINE_OK) {
      return false;
    }
    if (line == ".") {
      break;
    }
    body += line + "\n";
  }

  bool ok = isValidUsername(sender) && isValidUsername(receiver) &&
            isValidSubject(subject) &&
            storeMessage(sender, receiver, subject, body);
  if (!ok) {
    fprintf(stderr, "SEND rejected (invalid input or storage error)\n");
  }
  return sendLine(sock, ok ? "OK" : "ERR");
}

// READ\n<username>\n<message number>\n
// Answer: OK\n<sender>\n<receiver>\n<subject>\n<body lines>\n.\n  or  ERR\n
// Returns false if the connection is gone (then there is nobody to answer).
bool handleRead(int sock, std::string& pending) {
  std::string username, numberText, content;

  // ALWAYS read both lines, even if the first one turns out to be invalid.
  // Otherwise the second line would be interpreted as a command.
  if (readLine(sock, pending, username) != LINE_OK ||
      readLine(sock, pending, numberText) != LINE_OK) {
    return false;
  }

  int number = 0;
  std::vector<std::string> files;
  bool ok = isValidUsername(username) &&
            isValidMessageNumber(numberText, number) &&
            listMessages(username, "received", files) &&
            static_cast<size_t>(number) <= files.size() &&
            readFile(spoolDirectory + "/" + username + "/received/" +
                         files[number - 1],
                     content);
  if (!ok) {
    fprintf(stderr, "READ rejected (invalid input or no such message)\n");
    return sendLine(sock, "ERR");
  }

  // every stored line ends with '\n'; make sure the final '.' is on its own
  // line
  if (!content.empty() && content.back() != '\n') {
    content += "\n";
  }
  return sendAll(sock, "OK\n" + content + ".\n");
}

// LIST\n<username>\n
// Answer: <count>\n<subject 1>\n...<subject count>\n
// Count is 0 for an unknown user, an invalid name or a storage error.
// Returns false if the connection is gone (then there is nobody to answer).
bool handleList(int sock, std::string& pending) {
  std::string username;
  if (readLine(sock, pending, username) != LINE_OK) {
    return false;
  }

  std::vector<std::string> files;
  if (!isValidUsername(username) ||
      !listMessages(username, "received", files)) {
    fprintf(stderr, "LIST rejected (invalid input or storage error)\n");
    return sendLine(sock, "0");
  }

  std::string answer = std::to_string(files.size()) + "\n";
  for (const std::string& file : files) {
    answer +=
        readSubject(spoolDirectory + "/" + username + "/received/" + file) +
        "\n";
  }
  return sendAll(sock, answer);
}

void* clientCommunication(void* data) {
  char buffer[BUF];
  int* current_socket = static_cast<int*>(data);
  // bytes received but not processed yet (belongs to this connection only)
  std::string pending;
  std::string line;

  ////////////////////////////////////////////////////////////////////////////
  // SEND welcome message
  strcpy(buffer, "Welcome to myserver!\r\nPlease enter your commands...\r\n");
  if (send(*current_socket, buffer, strlen(buffer), 0) == -1) {
    perror("send failed");
    return nullptr;
  }

  while (!abortRequested) {
    /////////////////////////////////////////////////////////////////////////
    // RECEIVE ONE COMMAND LINE
    ReadResult result = readLine(*current_socket, pending, line);
    if (result == LINE_CLOSED) {
      printf("Client closed remote socket\n"); // ignore error
      break;
    }
    if (result == LINE_ERROR) {
      break;
    }

    printf("Command received: %s\n", line.c_str()); // ignore error

    if (line == "SEND") {
      if (!handleSend(*current_socket, pending)) {
        break;
      }
    } else if (line == "READ") {
      if (!handleRead(*current_socket, pending)) {
        break;
      }
    } else if (line == "LIST") {
      if (!handleList(*current_socket, pending)) {
        break;
      }
    } else if (line == "QUIT") {
      break; // the server does not respond to QUIT
    } else {
      // TODO: DEL
      if (!sendLine(*current_socket, "ERR")) {
        break;
      }
    }
  }

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
