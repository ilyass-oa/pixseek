#include "pixseek/catalog.hpp"
#include "pixseek/net.hpp"
#include <array>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

using namespace pixseek;
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

struct Options {
  std::string images = "img", bind = "127.0.0.1";
  int port = 5555, workers = 4, max_clients = 32, timeout = 5000;
};
void help() {
  std::cout << "Pixseek server\nUsage: pixseek-server [OPTIONS]\n"
               "  --images PATH      Catalog directory (default: img)\n"
               "  --bind IPv4        Listen address (default: 127.0.0.1)\n"
               "  --port N           Listen port; 0 chooses a free port "
               "(default: 5555)\n"
               "  --workers N        Search threads, 1-16 (default: 4)\n"
               "  --max-clients N    Connection/pending job limit, 1-64 "
               "(default: 32)\n"
               "  --timeout-ms N     Request/idle timeout, 100-60000 (default: "
               "5000)\n";
}

struct Job {
  uint64_t id;
  std::vector<char> image;
};
struct Result {
  uint64_t id;
  std::string reply;
};

class Workers {
public:
  Workers(const std::vector<CatalogEntry> &catalog, int count,
          std::size_t capacity, int wake)
      : catalog_(catalog), capacity_(capacity), wake_(wake) {
    try {
      for (int i = 0; i < count; ++i)
        threads_.emplace_back([this] { run(); });
    } catch (...) {
      shutdown();
      throw;
    }
  }
  ~Workers() { shutdown(); }
  bool submit(uint64_t id, std::vector<char> &image) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (outstanding_ >= capacity_)
        return false;
      jobs_.push_back({id, std::move(image)});
      ++outstanding_;
    }
    ready_.notify_one();
    return true;
  }
  std::deque<Result> take_results() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::deque<Result> results;
    results.swap(results_);
    outstanding_ -= results.size();
    return results;
  }

private:
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      jobs_.clear();
    }
    ready_.notify_all();
    for (auto &thread : threads_)
      if (thread.joinable())
        thread.join();
  }
  void run() {
    while (true) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopped_ || !jobs_.empty(); });
        if (stopped_)
          return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      std::string reply;
      try {
        reply = search(catalog_, job.image);
      } catch (const std::exception &) {
        reply = "Error: image search failed.";
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
          return;
        results_.push_back({job.id, std::move(reply)});
      }
      // A full pipe already has a wake-up waiting for the reactor.
      char byte = 1;
      (void)::write(wake_, &byte, 1);
    }
  }
  const std::vector<CatalogEntry> &catalog_;
  const std::size_t capacity_;
  const int wake_;
  std::size_t outstanding_ = 0;
  bool stopped_ = false;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  std::deque<Result> results_;
  std::vector<std::thread> threads_;
};

struct Connection {
  FileDescriptor socket;
  std::array<char, 4> header{};
  std::size_t header_used = 0, received = 0, sent = 0;
  std::vector<char> image;
  std::string reply;
  bool busy = false;
  Deadline deadline;
  Connection(FileDescriptor fd, Deadline until)
      : socket(std::move(fd)), deadline(until) {}
};

bool receive(Connection &client, uint64_t id, Workers &workers) {
  while (!client.busy) {
    const bool header = client.header_used < client.header.size();
    char *target = header ? client.header.data() + client.header_used
                          : client.image.data() + client.received;
    std::size_t remaining = header ? client.header.size() - client.header_used
                                   : client.image.size() - client.received;
    ssize_t count = ::recv(client.socket.get(), target, remaining, 0);
    if (count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    if (header) {
      client.header_used += static_cast<std::size_t>(count);
      if (client.header_used == client.header.size()) {
        uint32_t length;
        std::memcpy(&length, client.header.data(), sizeof(length));
        length = ntohl(length);
        if (length == 0 || length > max_image_bytes)
          return false;
        client.image.resize(length);
      }
    } else {
      client.received += static_cast<std::size_t>(count);
      if (client.received == client.image.size()) {
        if (!workers.submit(id, client.image))
          return false;
        client.busy = true;
      }
    }
  }
  return true;
}

bool send_reply(Connection &client, int timeout) {
  while (client.sent < client.reply.size()) {
    ssize_t count =
        ::send(client.socket.get(), client.reply.data() + client.sent,
               client.reply.size() - client.sent, MSG_NOSIGNAL);
    if (count > 0)
      client.sent += static_cast<std::size_t>(count);
    else if (count < 0 && errno == EINTR)
      continue;
    else
      return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
  }
  client.reply.clear();
  client.sent = client.header_used = client.received = 0;
  client.deadline = Clock::now() + std::chrono::milliseconds(timeout);
  return true;
}

void serve(const Options &options) {
  const auto catalog = load_catalog(options.images);
  FileDescriptor listener(::socket(AF_INET, SOCK_STREAM, 0));
  if (listener.get() < 0)
    throw system_error("socket");
  nonblocking(listener.get());
  int reuse = 1;
  if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0)
    throw system_error("setsockopt");
  auto local = address(options.bind, options.port);
  if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&local),
             sizeof(local)) < 0)
    throw system_error("bind");
  if (::listen(listener.get(), options.max_clients) < 0)
    throw system_error("listen");
  socklen_t size = sizeof(local);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&local),
                    &size) < 0)
    throw system_error("getsockname");
  int pipe_fds[2];
  if (::pipe(pipe_fds) < 0)
    throw system_error("pipe");
  FileDescriptor wake_read(pipe_fds[0]), wake_write(pipe_fds[1]);
  nonblocking(wake_read.get());
  nonblocking(wake_write.get());
  Workers workers(catalog, options.workers,
                  static_cast<std::size_t>(options.max_clients),
                  wake_write.get());
  std::map<uint64_t, Connection> clients;
  uint64_t next_id = 0;
  std::cout << "Indexed " << catalog.size() << " images\nListening on "
            << options.bind << ':' << ntohs(local.sin_port) << std::endl;
  while (!stopping) {
    for (auto &result : workers.take_results()) {
      auto found = clients.find(result.id);
      if (found != clients.end()) {
        found->second.busy = false;
        found->second.reply = std::move(result.reply);
        found->second.reply.push_back('\0');
      }
    }
    auto now = Clock::now();
    for (auto it = clients.begin(); it != clients.end();) {
      if (now >= it->second.deadline)
        it = clients.erase(it);
      else
        ++it;
    }
    std::vector<pollfd> events{{listener.get(), POLLIN, 0},
                               {wake_read.get(), POLLIN, 0}};
    std::vector<uint64_t> ids;
    for (const auto &[id, client] : clients) {
      short interest =
          !client.reply.empty() ? POLLOUT : (client.busy ? 0 : POLLIN);
      events.push_back({client.socket.get(), interest, 0});
      ids.push_back(id);
    }
    int ready = ::poll(events.data(), events.size(), 50);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      throw system_error("poll");
    }
    if (events[1].revents & POLLIN) {
      char bytes[256];
      while (::read(wake_read.get(), bytes, sizeof(bytes)) > 0) {
      }
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      short flags = events[i + 2].revents;
      if (!flags)
        continue;
      auto found = clients.find(ids[i]);
      auto &client = found->second;
      bool ok = !(flags & (POLLERR | POLLNVAL | POLLHUP));
      if (ok && (flags & POLLIN))
        ok = receive(client, ids[i], workers);
      if (ok && (flags & POLLOUT))
        ok = send_reply(client, options.timeout);
      if (!ok)
        clients.erase(found);
    }
    if (events[0].revents & POLLIN) {
      // Limit each batch so a connection flood cannot starve existing clients.
      for (int i = 0; i < options.max_clients; ++i) {
        FileDescriptor fd(::accept(listener.get(), nullptr, nullptr));
        if (fd.get() < 0) {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          throw system_error("accept");
        }
        if (clients.size() >= static_cast<std::size_t>(options.max_clients))
          continue;
        nonblocking(fd.get());
        clients.emplace(
            ++next_id,
            Connection(std::move(fd), Clock::now() + std::chrono::milliseconds(
                                                         options.timeout)));
      }
    }
  }
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
      if (key != "--images" && key != "--bind" && key != "--port" &&
          key != "--workers" && key != "--max-clients" && key != "--timeout-ms")
        throw std::runtime_error("Unknown option: " + key);
      if (++i >= argc)
        throw std::runtime_error("Missing value for " + key);
      if (key == "--images")
        options.images = argv[i];
      else if (key == "--bind")
        options.bind = argv[i];
      else if (key == "--port")
        options.port = number(argv[i], 0, 65535, key);
      else if (key == "--workers")
        options.workers = number(argv[i], 1, 16, key);
      else if (key == "--max-clients")
        options.max_clients = number(argv[i], 1, 64, key);
      else
        options.timeout = number(argv[i], 100, 60000, key);
    }
    (void)address(options.bind, options.port);
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  try {
    serve(options);
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
