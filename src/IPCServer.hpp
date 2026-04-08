#ifndef IPC_SERVER_HPP
#define IPC_SERVER_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class IPCServer {
public:
  IPCServer();
  ~IPCServer();

  void start();
  void stop();

private:
  void server_loop();
  static int handle_client(int fd, const std::shared_ptr<std::atomic<bool>>& running);
  void join_client_threads();

  std::thread th_;
  std::shared_ptr<std::atomic<bool>> running_{std::make_shared<std::atomic<bool>>(false)};
  std::mutex client_mutex_;
  std::vector<int> client_fds_;
  std::vector<std::thread> client_threads_;
};

#endif
