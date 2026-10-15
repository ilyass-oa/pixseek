#include "pixseek/net.hpp"
#include <iostream>
#include <utility>
using namespace pixseek;
namespace {
struct Options {
  std::string host = "127.0.0.1";
  int port = 5555;
  int timeout = 5000;
  std::vector<std::string> files;
};
void help() {
  std::cout
      << "Pixseek client\nUsage: pixseek-client [--host IPv4] [--port N] "
         "[--timeout-ms N] [FILE ...]\n"
         "Without files, read one image path per line from standard input.\n";
}
FileDescriptor connect_to(const Options &options, Deadline deadline) {
  FileDescriptor fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd.get() < 0)
    throw system_error("socket");
  nonblocking(fd.get());
  auto target = address(options.host, options.port);
  if (::connect(fd.get(), reinterpret_cast<const sockaddr *>(&target),
                sizeof(target)) < 0) {
    if (errno != EINPROGRESS)
      throw system_error("connect");
    wait_for(fd.get(), POLLOUT, deadline);
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0)
      throw system_error("getsockopt");
    if (error) {
      errno = error;
      throw system_error("connect");
    }
  }
  return fd;
}
} // namespace
int main(int argc, char **argv) {
  Options options;
  try {
    for (int i = 1; i < argc; ++i) {
      std::string key = argv[i];
      if (key == "--help") {
        help();
        return 0;
      }
      if (key == "--host" || key == "--port" || key == "--timeout-ms") {
        if (++i >= argc)
          throw std::runtime_error("Missing value for " + key);
        if (key == "--host")
          options.host = argv[i];
        else if (key == "--port")
          options.port = number(argv[i], 1, 65535, key);
        else
          options.timeout = number(argv[i], 100, 60000, key);
      } else if (key.rfind("--", 0) == 0)
        throw std::runtime_error("Unknown option: " + key);
      else
        options.files.push_back(key);
    }
    (void)address(options.host, options.port);
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  }
  int status = 0;
  FileDescriptor socket;
  auto process = [&](const std::string &path) {
    std::vector<char> bytes;
    try {
      bytes = read_image(path);
    } catch (const std::exception &error) {
      std::cerr << "Error: " << error.what() << '\n';
      status = 1;
      return;
    }
    auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout);
    if (socket.get() < 0)
      socket = connect_to(options, deadline);
    uint32_t length = htonl(static_cast<uint32_t>(bytes.size()));
    send_all(socket.get(), reinterpret_cast<const char *>(&length),
             sizeof(length), deadline);
    send_all(socket.get(), bytes.data(), bytes.size(), deadline);
    std::cout << receive_reply(socket.get(), deadline) << std::endl;
  };
  try {
    if (!options.files.empty())
      for (const auto &file : options.files)
        process(file);
    else {
      std::string path;
      while (std::getline(std::cin, path)) {
        if (!path.empty() && path.back() == '\r')
          path.pop_back();
        if (!path.empty())
          process(path);
      }
      if (std::cin.bad())
        throw std::runtime_error("Failed to read image paths");
    }
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
  return status;
}
