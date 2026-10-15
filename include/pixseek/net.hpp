#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pixseek {
constexpr std::size_t max_image_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_reply_bytes = 4096;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

class FileDescriptor {
public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0)
      ::close(fd_);
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  int get() const { return fd_; }

private:
  int fd_;
};
inline std::runtime_error system_error(const std::string &action) {
  return std::runtime_error(action + ": " + std::strerror(errno));
}
inline void nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    throw system_error("fcntl");
}
inline int number(const std::string &value, int minimum, int maximum,
                  const std::string &option) {
  if (value.empty() ||
      value.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("Invalid value for " + option);
  std::size_t used = 0;
  long parsed;
  try {
    parsed = std::stol(value, &used);
  } catch (const std::exception &) {
    throw std::runtime_error("Invalid value for " + option);
  }
  if (used != value.size() || parsed < minimum || parsed > maximum)
    throw std::runtime_error("Invalid value for " + option);
  return static_cast<int>(parsed);
}
inline sockaddr_in address(const std::string &host, int port) {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(static_cast<uint16_t>(port));
  std::string ip = host == "localhost" ? "127.0.0.1" : host;
  if (::inet_pton(AF_INET, ip.c_str(), &result.sin_addr) != 1)
    throw std::runtime_error("Expected an IPv4 address: " + host);
  return result;
}
inline void wait_for(int fd, short events, Deadline deadline) {
  while (true) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - Clock::now())
                         .count();
    if (remaining <= 0)
      throw std::runtime_error("Connection timed out");
    pollfd p{fd, events, 0};
    int rc = ::poll(&p, 1, static_cast<int>(remaining));
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      throw system_error("poll");
    }
    if (rc == 0)
      throw std::runtime_error("Connection timed out");
    if (p.revents & (events | POLLHUP | POLLERR))
      return;
    if (p.revents & POLLNVAL)
      throw std::runtime_error("Invalid connection");
  }
}
inline void send_all(int fd, const char *data, std::size_t length,
                     Deadline deadline) {
  std::size_t sent = 0;
  while (sent < length) {
    wait_for(fd, POLLOUT, deadline);
    ssize_t count = ::send(fd, data + sent, length - sent, MSG_NOSIGNAL);
    if (count > 0)
      sent += static_cast<std::size_t>(count);
    else if (count < 0 &&
             (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      continue;
    else
      throw std::runtime_error("Server disconnected while sending");
  }
}
inline std::string receive_reply(int fd, Deadline deadline) {
  std::string result;
  while (result.size() < max_reply_bytes) {
    wait_for(fd, POLLIN, deadline);
    char byte;
    ssize_t count = ::recv(fd, &byte, 1, 0);
    if (count == 1) {
      if (byte == '\0')
        return result;
      result += byte;
    } else if (count == 0)
      throw std::runtime_error("Server disconnected before replying");
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      throw system_error("recv");
  }
  throw std::runtime_error("Server reply exceeds 4096 bytes");
}
inline std::vector<char> read_image(const std::string &path) {
  FileDescriptor file(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  struct stat info{};
  if (file.get() < 0 || ::fstat(file.get(), &info) < 0 ||
      !S_ISREG(info.st_mode))
    throw std::runtime_error("Cannot read regular image file: " + path);
  if (info.st_size <= 0 ||
      static_cast<uint64_t>(info.st_size) > max_image_bytes)
    throw std::runtime_error("Image must contain 1 to 4194304 bytes: " + path);
  std::vector<char> data(static_cast<std::size_t>(info.st_size));
  std::size_t used = 0;
  while (used < data.size()) {
    ssize_t count = ::read(file.get(), data.data() + used, data.size() - used);
    if (count > 0)
      used += static_cast<std::size_t>(count);
    else if (count < 0 && errno == EINTR)
      continue;
    else
      throw std::runtime_error("Incomplete image read: " + path);
  }
  return data;
}
} // namespace pixseek
