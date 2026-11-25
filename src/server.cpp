#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
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

// protects the mail spool:
//   SEND, DEL  -> exclusive lock (std::unique_lock): only this one thread
//   LIST, READ -> shared lock (std::shared_lock): many readers at once,
//                 but never while a SEND or DEL is running
// Only file work happens under the lock, never sending to the client: a slow
// client must not block all other threads.
std::shared_mutex mailboxMutex;

///////////////////////////////////////////////////////////////////////////////

enum ReadResult { LINE_OK, LINE_CLOSED, LINE_ERROR };

// all commands of the protocol; UNKNOWN = the line is none of them
enum class Command { SEND, READ, LIST, DEL, QUIT, UNKNOWN };

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
unsigned long messageId(const std::string& fileName);
unsigned long nextMessageId(const std::string& dir);
bool writeNewMessage(const std::string& dir, const std::string& subjectPart,
                     const std::string& content, std::string& pathOut);
bool storeMessage(const std::string& sender, const std::string& receiver,
                  const std::string& subject, const std::string& body);
bool listMessages(const std::string& user, const std::string& box,
                  std::vector<std::string>& files);
bool readFile(const std::string& path, std::string& content);
std::string readSubject(const std::string& path);

/// COMMAND HANDLERS
bool sendError(int sock, const char* command, const std::string& reason);
bool handleSend(int sock, std::string& pending);
bool handleRead(int sock, std::string& pending);
bool handleList(int sock, std::string& pending);
bool handleDel(int sock, std::string& pending);
Command parseCommand(const std::string& line);
bool handleCommand(int sock, const std::string& line, std::string& pending);

void clientCommunication(int sock);
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
    // local variable: every client gets its own socket, the next accept() must
    // not overwrite the one a running thread is still using
    int clientSocket =
        accept(create_socket, reinterpret_cast<sockaddr*>(&cliaddress),
               &addrlen);
    if (clientSocket == -1) {
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
    // inet_ntop instead of inet_ntoa: inet_ntoa returns a static buffer and is
    // therefore not thread safe
    char clientIp[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &cliaddress.sin_addr, clientIp, sizeof(clientIp));
    printf("Client connected from %s:%d...\n", clientIp,
           ntohs(cliaddress.sin_port));

    // one thread per client; the socket is passed BY VALUE.
    // detach: the thread cleans up by itself when the client is done.
    try {
      std::thread(clientCommunication, clientSocket).detach();
    } catch (const std::system_error& e) {
      fprintf(stderr, "Cannot create thread: %s\n", e.what());
      close(clientSocket);
    }
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
///   <spool>/<user>/received/<subject>-<id>.txt
///
/// <id> is a running number per mailbox (1, 2, 3, ...). The next message gets
/// the highest existing id + 1, so it also continues correctly after a
/// server restart.
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

// Id of a file name "<subject>-<id>.txt" (the subject has no '-', see
// sanitizeForFilename). Returns 0 if the name has no valid id
// such files are sorted first and never block a new id.
unsigned long messageId(const std::string& fileName) {
  std::string name = fileName;
  if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".txt") == 0) {
    name.erase(name.size() - 4);
  }
  size_t dash = name.find('-');
  if (dash == std::string::npos) {
    return 0;
  }
  std::string idText = name.substr(dash + 1);
  if (idText.empty() || idText.size() > 9) {
    return 0;
  }
  for (char c : idText) {
    if (c < '0' || c > '9') {
      return 0;
    }
  }
  return std::stoul(idText);
}

// Highest id in 'dir' + 1. Must be called while mailboxMutex is locked exclusively,
// otherwise two threads could get the same id.
unsigned long nextMessageId(const std::string& dir) {
  namespace fs = std::filesystem;
  unsigned long maxId = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    maxId = std::max(maxId, messageId(it->path().filename().string()));
  }
  return maxId + 1;
}

// Writes content into <dir>/<subjectPart>-<id>.txt with the next free id.
// Mode "wx" fails if the file exists, so nothing is ever overwritten; in that
// case (a foreign file with that id) the next id is tried.
// Must be called while mailboxMutex is locked exclusively.
bool writeNewMessage(const std::string& dir, const std::string& subjectPart,
                     const std::string& content, std::string& pathOut) {
  unsigned long id = nextMessageId(dir);
  for (int attempt = 0; attempt < 100; ++attempt, ++id) {
    std::string path = dir + "/" + subjectPart + "-" + std::to_string(id) +
                       ".txt";

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

  // exclusive: creating the folders, picking the id and creating the file
  // must not overlap with another SEND/DEL, and no LIST/READ may see a half
  // written file. unique_lock unlocks automatically when the function returns.
  std::unique_lock<std::shared_mutex> lock(mailboxMutex);

  std::string receivedDir;
  if (!ensureMailbox(receiver, "received", receivedDir)) {
    return false;
  }

  std::string receivedPath;
  if (!writeNewMessage(receivedDir, sanitizeForFilename(subject), content,
                       receivedPath)) {
    return false;
  }

  printf("Stored message: %s.", receivedPath.c_str());
  return true;
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
              unsigned long idA = messageId(a);
              unsigned long idB = messageId(b);
              return idA != idB ? idA < idB : a < b;
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

// Logs the reason and answers "ERR <reason>" on a single line, so the client
// knows what went wrong. Returns false if the connection is gone.
bool sendError(int sock, const char* command, const std::string& reason) {
  fprintf(stderr, "%s rejected: %s\n", command, reason.c_str());
  return sendLine(sock, "ERR " + reason);
}

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

  if (!isValidUsername(sender)) {
    return sendError(sock, "SEND", "invalid sender username");
  }
  if (!isValidUsername(receiver)) {
    return sendError(sock, "SEND", "invalid receiver username");
  }
  if (!isValidSubject(subject)) {
    return sendError(sock, "SEND", "invalid subject (1-80 characters)");
  }
  if (!storeMessage(sender, receiver, subject, body)) {
    return sendError(sock, "SEND", "could not store message");
  }
  return sendLine(sock, "OK");
}

// READ\n<username>\n<message number>\n
// Answer: OK\n<sender>\n<receiver>\n<subject>\n<body lines>\n.\n  or  ERR <reason>\n
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
  if (!isValidUsername(username)) {
    return sendError(sock, "READ", "invalid username");
  }
  if (!isValidMessageNumber(numberText, number)) {
    return sendError(sock, "READ", "invalid message number");
  }

  // listing and reading must happen under ONE lock, otherwise a DEL in
  // between could shift the numbers or remove the file
  std::string error;
  {
    std::shared_lock<std::shared_mutex> lock(mailboxMutex);
    if (!listMessages(username, "received", files)) {
      error = "could not read mailbox";
    } else if (static_cast<size_t>(number) > files.size()) {
      error = "no such message";
    } else if (!readFile(spoolDirectory + "/" + username + "/received/" +
                             files[number - 1],
                         content)) {
      error = "could not read message";
    }
  } // lock released here, before answering the client
  if (!error.empty()) {
    return sendError(sock, "READ", error);
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
  if (!isValidUsername(username)) {
    return sendError(sock, "LIST", "invalid username");
  }

  bool listed;
  std::string answer;
  {
    std::shared_lock<std::shared_mutex> lock(mailboxMutex);
    listed = listMessages(username, "received", files);
    if (listed) {
      answer = std::to_string(files.size()) + "\n";
      for (const std::string& file : files) {
        answer +=
            readSubject(spoolDirectory + "/" + username + "/received/" + file) +
            "\n";
      }
    }
  } // lock released here, before answering the client
  if (!listed) {
    return sendError(sock, "LIST", "could not read mailbox");
  }
  return sendAll(sock, answer);
}

// DEL\n<username>\n<message number>\n
// Answer: OK\n  or  ERR\n
// Deletes from the inbox only, the copy in the sender's sent folder stays.
// Returns false if the connection is gone (then there is nobody to answer).
bool handleDel(int sock, std::string& pending) {
  std::string username, numberText;

  // ALWAYS read both lines, even if the first one turns out to be invalid.
  // Otherwise the second line would be interpreted as a command.
  if (readLine(sock, pending, username) != LINE_OK ||
      readLine(sock, pending, numberText) != LINE_OK) {
    return false;
  }

  int number = 0;
  std::vector<std::string> files;
  if (!isValidUsername(username)) {
    return sendError(sock, "DEL", "invalid username");
  }
  if (!isValidMessageNumber(numberText, number)) {
    return sendError(sock, "DEL", "invalid message number");
  }

  // exclusive: listing and deleting must happen under ONE lock, otherwise
  // another DEL in between could shift the numbers -> wrong mail deleted
  std::string error;
  {
    std::unique_lock<std::shared_mutex> lock(mailboxMutex);
    if (!listMessages(username, "received", files)) {
      error = "could not read mailbox";
    } else if (static_cast<size_t>(number) > files.size()) {
      error = "no such message";
    } else {
      std::string path =
          spoolDirectory + "/" + username + "/received/" + files[number - 1];
      if (remove(path.c_str()) != 0) {
        fprintf(stderr, "Cannot delete '%s': %s\n", path.c_str(),
                strerror(errno));
        error = "could not delete message";
      }
    }
  } // lock released here, before answering the client
  if (!error.empty()) {
    return sendError(sock, "DEL", error);
  }
  return sendLine(sock, "OK");
}

// Maps the command line typed by the client to its enum value.
Command parseCommand(const std::string& line) {
  if (line == "SEND") return Command::SEND;
  if (line == "READ") return Command::READ;
  if (line == "LIST") return Command::LIST;
  if (line == "DEL") return Command::DEL;
  if (line == "QUIT") return Command::QUIT;
  return Command::UNKNOWN;
}

// Checks which command 'line' is and calls the matching handler.
// Returns false if the connection has to be closed (QUIT or connection gone).
bool handleCommand(int sock, const std::string& line, std::string& pending) {
  switch (parseCommand(line)) {
    case Command::SEND:
      return handleSend(sock, pending);
    case Command::READ:
      return handleRead(sock, pending);
    case Command::LIST:
      return handleList(sock, pending);
    case Command::DEL:
      return handleDel(sock, pending);
    case Command::QUIT:
      return false; // the server does not respond to QUIT
    case Command::UNKNOWN:
      break;
  }
  return sendLine(sock, "ERR"); // unknown command
}

// Runs in its own thread, one per connected client. Owns 'sock' and closes it.
void clientCommunication(int sock) {
  char buffer[BUF];
  // bytes received but not processed yet (belongs to this connection only)
  std::string pending;
  std::string line;

  ////////////////////////////////////////////////////////////////////////////
  // SEND welcome message
  strcpy(buffer, "Welcome to myserver!\r\nPlease enter your commands...\r\n");
  if (send(sock, buffer, strlen(buffer), 0) == -1) {
    perror("send failed");
    close(sock);
    return;
  }

  while (!abortRequested) {
    /////////////////////////////////////////////////////////////////////////
    // RECEIVE ONE COMMAND LINE
    ReadResult result = readLine(sock, pending, line);
    if (result == LINE_CLOSED) {
      printf("Client closed remote socket\n"); // ignore error
      break;
    }
    if (result == LINE_ERROR) {
      break;
    }

    printf("Command received: %s\n", line.c_str()); // ignore error

    if (!handleCommand(sock, line, pending)) {
      break;
    }
  }

  // closes/frees the descriptor
  if (shutdown(sock, SHUT_RDWR) == -1) {
    perror("shutdown client socket");
  }
  if (close(sock) == -1) {
    perror("close client socket");
  }
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
