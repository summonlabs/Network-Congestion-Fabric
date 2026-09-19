// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/ipc/channel.hpp"

#include "ncf/core/strong_id.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ncf {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

[[nodiscard]] std::wstring pipe_path(const std::string& name) {
  std::wstring path = L"\\\\.\\pipe\\";
  path.append(widen(name));
  return path;
}

class OverlappedChannel final : public IByteChannel {
 public:
  OverlappedChannel(HANDLE handle, std::uint64_t id) : handle_(handle), id_(id) {
    read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~OverlappedChannel() override { close(); }

  Result<std::size_t> read(std::span<std::byte> out) override {
    if (handle_ == INVALID_HANDLE_VALUE) {
      return Status(ErrCode::Closed, "channel is closed");
    }
    if (out.empty()) {
      return static_cast<std::size_t>(0);
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = read_event_;
    ResetEvent(read_event_);
    DWORD read = 0;
    const DWORD request = static_cast<DWORD>(out.size() < 0xFFFFFFFFull ? out.size() : 0xFFFFFFFFull);
    BOOL ok = ReadFile(handle_, out.data(), request, &read, &overlapped);
    if (!ok) {
      const DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        if (WaitForSingleObject(read_event_, INFINITE) != WAIT_OBJECT_0) {
          return Status(ErrCode::IoError, "overlapped read wait failed");
        }
        if (!GetOverlappedResult(handle_, &overlapped, &read, FALSE)) {
          const DWORD final_error = GetLastError();
          if (final_error == ERROR_OPERATION_ABORTED) {
            return Status(ErrCode::Cancelled, "overlapped read was cancelled");
          }
          if (final_error == ERROR_BROKEN_PIPE || final_error == ERROR_PIPE_NOT_CONNECTED) {
            closed_ = true;
            return static_cast<std::size_t>(0);
          }
          return Status(ErrCode::IoError, "overlapped read failed");
        }
      } else if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
        closed_ = true;
        return static_cast<std::size_t>(0);
      } else if (error == ERROR_OPERATION_ABORTED) {
        return Status(ErrCode::Cancelled, "read was cancelled");
      } else {
        return Status(ErrCode::IoError, "read failed");
      }
    } else if (read == 0) {
      closed_ = true;
    }
    return static_cast<std::size_t>(read);
  }

  VoidResult write(std::span<const std::byte> bytes) override {
    if (handle_ == INVALID_HANDLE_VALUE) {
      return Status(ErrCode::Closed, "channel is closed");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const std::size_t remaining = bytes.size() - offset;
      const DWORD request = static_cast<DWORD>(remaining < 0xFFFFFFFFull ? remaining : 0xFFFFFFFFull);
      OVERLAPPED overlapped{};
      overlapped.hEvent = write_event_;
      ResetEvent(write_event_);
      DWORD written = 0;
      BOOL ok = WriteFile(handle_, bytes.data() + offset, request, &written, &overlapped);
      if (!ok) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
          if (WaitForSingleObject(write_event_, INFINITE) != WAIT_OBJECT_0) {
            return Status(ErrCode::IoError, "overlapped write wait failed");
          }
          if (!GetOverlappedResult(handle_, &overlapped, &written, FALSE)) {
            const DWORD final_error = GetLastError();
            if (final_error == ERROR_OPERATION_ABORTED) {
              return Status(ErrCode::Cancelled, "write was cancelled");
            }
            return Status(ErrCode::IoError, "overlapped write failed");
          }
        } else if (error == ERROR_OPERATION_ABORTED) {
          return Status(ErrCode::Cancelled, "write was cancelled");
        } else {
          return Status(ErrCode::IoError, "write failed");
        }
      }
      if (written == 0) {
        return Status(ErrCode::IoError, "write made no progress");
      }
      offset += written;
    }
    return VoidResult{};
  }

  void cancel_pending() noexcept override {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CancelIoEx(handle_, nullptr);
    }
  }

  void close() noexcept override {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CancelIoEx(handle_, nullptr);
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      closed_ = true;
    }
    if (read_event_ != nullptr) {
      CloseHandle(read_event_);
      read_event_ = nullptr;
    }
    if (write_event_ != nullptr) {
      CloseHandle(write_event_);
      write_event_ = nullptr;
    }
  }

  [[nodiscard]] bool closed() const noexcept override { return closed_ || handle_ == INVALID_HANDLE_VALUE; }

  [[nodiscard]] std::uint64_t native_handle() const noexcept override {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle_));
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
  HANDLE read_event_{nullptr};
  HANDLE write_event_{nullptr};
  std::uint64_t id_{0};
  bool closed_{false};
};

[[nodiscard]] HANDLE create_instance(const std::wstring& full, const ListenerOptions& options, bool first) {
  DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first) {
    open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  }
  const DWORD instances = options.max_instances == 0
                              ? PIPE_UNLIMITED_INSTANCES
                              : static_cast<DWORD>(options.max_instances < PIPE_UNLIMITED_INSTANCES
                                                       ? options.max_instances
                                                       : PIPE_UNLIMITED_INSTANCES);
  const DWORD buffer = static_cast<DWORD>(options.buffer_bytes < (1u << 20) ? options.buffer_bytes : (1u << 20));
  return CreateNamedPipeW(full.c_str(), open_mode,
                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                          instances, buffer, buffer, 0, nullptr);
}

}  // namespace

PipeListener::PipeListener(PipeListener&& other) noexcept
    : endpoint_(std::move(other.endpoint_)), options_(other.options_), handle_(other.handle_),
      cancel_event_(other.cancel_event_) {
  other.handle_ = 0;
  other.cancel_event_ = 0;
}

PipeListener& PipeListener::operator=(PipeListener&& other) noexcept {
  if (this != &other) {
    close();
    endpoint_ = std::move(other.endpoint_);
    options_ = other.options_;
    handle_ = other.handle_;
    cancel_event_ = other.cancel_event_;
    other.handle_ = 0;
    other.cancel_event_ = 0;
  }
  return *this;
}

PipeListener::~PipeListener() { close(); }

bool PipeListener::valid() const noexcept { return handle_ != 0; }

void PipeListener::close() noexcept {
  if (handle_ != 0) {
    CancelIoEx(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle_)), nullptr);
    CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle_)));
    handle_ = 0;
  }
  if (cancel_event_ != 0) {
    CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(cancel_event_)));
    cancel_event_ = 0;
  }
}

Result<PipeListener> PipeListener::create(const ListenerOptions& options) {
  if (options.name.empty()) {
    return Status(ErrCode::InvalidArgument, "listener name is empty");
  }
  const std::wstring full = pipe_path(options.name);
  HANDLE handle = create_instance(full, options, true);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY) {
      return Status(ErrCode::AlreadyExists, "an endpoint with this name already exists", options.name);
    }
    if (error == ERROR_INVALID_NAME) {
      return Status(ErrCode::InvalidArgument, "endpoint name is not a legal pipe name", options.name);
    }
    return Status(ErrCode::IoError, "cannot create the listening endpoint", options.name);
  }
  PipeListener listener;
  listener.endpoint_ = options.name;
  listener.options_ = options;
  listener.options_.name = options.name;
  listener.handle_ = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
  HANDLE cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (cancel_event == nullptr) {
    listener.close();
    return Status(ErrCode::IoError, "cannot create the listener cancellation event");
  }
  listener.cancel_event_ = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(cancel_event));
  return listener;
}

Result<ChannelPtr> PipeListener::accept(const std::atomic<bool>& cancel) {
  if (handle_ == 0) {
    return Status(ErrCode::Closed, "listener is closed");
  }
  HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle_));
  HANDLE cancel_event = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(cancel_event_));

  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) {
    return Status(ErrCode::IoError, "cannot create the accept event",
                  "win32 error " + std::to_string(GetLastError()));
  }

  BOOL connected = ConnectNamedPipe(handle, &overlapped);
  DWORD error = connected ? ERROR_SUCCESS : GetLastError();
  bool pending = false;
  if (!connected && error == ERROR_IO_PENDING) {
    pending = true;
  } else if (!connected && error == ERROR_PIPE_CONNECTED) {
    connected = TRUE;
  }

  while (pending) {
    HANDLE waits[2] = {overlapped.hEvent, cancel_event};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 25);
    if (wait == WAIT_OBJECT_0) {
      DWORD transferred = 0;
      if (GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
        connected = TRUE;
      } else {
        connected = FALSE;
        error = GetLastError();
      }
      pending = false;
      break;
    }
    if (wait == WAIT_OBJECT_0 + 1 || cancel.load(std::memory_order_acquire)) {
      CancelIoEx(handle, &overlapped);
      CloseHandle(overlapped.hEvent);
      return Status(ErrCode::Cancelled, "accept was cancelled");
    }
    if (wait == WAIT_FAILED) {
      CloseHandle(overlapped.hEvent);
      return Status(ErrCode::IoError, "accept wait failed");
    }
  }

  CloseHandle(overlapped.hEvent);

  const std::wstring full = pipe_path(endpoint_);
  const auto replace_instance = [this, &full]() -> VoidResult {
    ListenerOptions next_options = options_;
    if (next_options.name.empty()) {
      next_options.name = endpoint_;
    }
    HANDLE replacement = create_instance(full, next_options, false);
    if (replacement == INVALID_HANDLE_VALUE) {
      const DWORD create_error = GetLastError();
      if (handle_ != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle_)));
        handle_ = 0;
      }
      return Status(ErrCode::IoError, "cannot create the next pipe instance",
                    "win32 error " + std::to_string(create_error));
    }
    if (handle_ != 0) {
      CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle_)));
    }
    handle_ = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(replacement));
    return VoidResult{};
  };

  if (!connected) {
    if (error == ERROR_OPERATION_ABORTED) {
      return Status(ErrCode::Cancelled, "accept was cancelled");
    }
    if (error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
      // A peer connected and vanished before the connection completed. The
      // instance is spent, so it is replaced and the event is reported as a
      // benign, retryable outcome rather than as a listener failure. Leaving the
      // dead instance in place would wedge the endpoint permanently.
      const VoidResult replaced = replace_instance();
      if (!replaced.ok()) {
        return replaced.status();
      }
      return Status(ErrCode::Unavailable, "peer vanished before the connection completed");
    }
    return Status(ErrCode::IoError, "accept failed", "win32 error " + std::to_string(error));
  }

  // Ownership of the connected instance passes to the caller, so the listener
  // gives up its handle before the replacement is created. Otherwise the
  // replacement path would close the very connection being handed over.
  handle_ = 0;
  const VoidResult replaced = replace_instance();
  if (!replaced.ok()) {
    CloseHandle(handle);
    return replaced.status();
  }

  static std::atomic<std::uint64_t> counter{0};
  return ChannelPtr(new OverlappedChannel(handle, counter.fetch_add(1)));
}

Result<ChannelPtr> connect_endpoint(const std::string& endpoint, Tick connect_timeout_micros) {
  if (endpoint.empty()) {
    return Status(ErrCode::InvalidArgument, "endpoint name is empty");
  }
  const std::wstring full = pipe_path(endpoint);
  const Tick start = static_cast<Tick>(GetTickCount64()) * 1000ull;
  for (;;) {
    HANDLE handle = CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      return ChannelPtr(new OverlappedChannel(handle, 0));
    }
    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
      return Status(ErrCode::IoError, "cannot connect to the endpoint", endpoint);
    }
    const Tick now = static_cast<Tick>(GetTickCount64()) * 1000ull;
    if (connect_timeout_micros != 0 && now - start >= connect_timeout_micros) {
      return Status(ErrCode::Unavailable, "endpoint did not become available in time", endpoint);
    }
    WaitNamedPipeW(full.c_str(), 25);
  }
}

#else  // POSIX

namespace {

class SocketChannel final : public IByteChannel {
 public:
  explicit SocketChannel(int fd) : fd_(fd) {}
  ~SocketChannel() override { close(); }

  Result<std::size_t> read(std::span<std::byte> out) override {
    if (fd_ < 0) {
      return Status(ErrCode::Closed, "channel is closed");
    }
    const ssize_t count = ::recv(fd_, out.data(), out.size(), 0);
    if (count < 0) {
      if (errno == EINTR) {
        return static_cast<std::size_t>(0);
      }
      return Status(ErrCode::IoError, "socket receive failed");
    }
    if (count == 0) {
      closed_ = true;
    }
    return static_cast<std::size_t>(count);
  }

  VoidResult write(std::span<const std::byte> bytes) override {
    if (fd_ < 0) {
      return Status(ErrCode::Closed, "channel is closed");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t count = ::send(fd_, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return Status(ErrCode::IoError, "socket send failed");
      }
      offset += static_cast<std::size_t>(count);
    }
    return VoidResult{};
  }

  void cancel_pending() noexcept override {
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
    }
  }

  void close() noexcept override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
      closed_ = true;
    }
  }

  [[nodiscard]] bool closed() const noexcept override { return closed_ || fd_ < 0; }
  [[nodiscard]] std::uint64_t native_handle() const noexcept override {
    return static_cast<std::uint64_t>(fd_);
  }

 private:
  int fd_{-1};
  bool closed_{false};
};

[[nodiscard]] std::string unix_path(const std::string& name) {
  return std::string("/tmp/ncf-") + name + ".sock";
}

}  // namespace

PipeListener::PipeListener(PipeListener&& other) noexcept
    : endpoint_(std::move(other.endpoint_)), handle_(other.handle_), cancel_event_(other.cancel_event_) {
  other.handle_ = 0;
  other.cancel_event_ = 0;
}

PipeListener& PipeListener::operator=(PipeListener&& other) noexcept {
  if (this != &other) {
    close();
    endpoint_ = std::move(other.endpoint_);
    handle_ = other.handle_;
    cancel_event_ = other.cancel_event_;
    other.handle_ = 0;
    other.cancel_event_ = 0;
  }
  return *this;
}

PipeListener::~PipeListener() { close(); }

bool PipeListener::valid() const noexcept { return handle_ != 0; }

void PipeListener::close() noexcept {
  if (handle_ != 0) {
    const int fd = static_cast<int>(handle_);
    ::close(fd);
    handle_ = 0;
  }
  if (cancel_event_ != 0) {
    const int fd = static_cast<int>(cancel_event_);
    ::close(fd);
    cancel_event_ = 0;
  }
}

Result<PipeListener> PipeListener::create(const ListenerOptions& options) {
  if (options.name.empty()) {
    return Status(ErrCode::InvalidArgument, "listener name is empty");
  }
  const std::string path = unix_path(options.name);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return Status(ErrCode::IoError, "cannot create a unix socket");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return Status(ErrCode::InvalidArgument, "endpoint path is too long");
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  ::unlink(path.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return Status(ErrCode::IoError, "cannot bind the endpoint", path);
  }
  if (::listen(fd, static_cast<int>(options.max_instances == 0 ? 16 : options.max_instances)) != 0) {
    ::close(fd);
    return Status(ErrCode::IoError, "cannot listen on the endpoint", path);
  }
  int cancel_pipe[2] = {-1, -1};
  if (::pipe(cancel_pipe) != 0) {
    ::close(fd);
    return Status(ErrCode::IoError, "cannot create the cancellation pipe");
  }
  PipeListener listener;
  listener.endpoint_ = options.name;
  listener.handle_ = static_cast<std::uint64_t>(fd);
  listener.cancel_event_ = static_cast<std::uint64_t>(cancel_pipe[0]);
  ::close(cancel_pipe[1]);
  return listener;
}

Result<ChannelPtr> PipeListener::accept(const std::atomic<bool>& cancel) {
  if (handle_ == 0) {
    return Status(ErrCode::Closed, "listener is closed");
  }
  const int listen_fd = static_cast<int>(handle_);
  const int cancel_fd = static_cast<int>(cancel_event_);
  for (;;) {
    pollfd fds[2] = {};
    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;
    fds[1].fd = cancel_fd;
    fds[1].events = POLLIN;
    const int ready = ::poll(fds, 2, 25);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status(ErrCode::IoError, "accept poll failed");
    }
    if (cancel.load(std::memory_order_acquire) || (ready > 0 && (fds[1].revents & POLLIN) != 0)) {
      return Status(ErrCode::Cancelled, "accept was cancelled");
    }
    if (ready > 0 && (fds[0].revents & POLLIN) != 0) {
      const int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        return Status(ErrCode::IoError, "accept failed");
      }
      return ChannelPtr(new SocketChannel(fd));
    }
  }
}

Result<ChannelPtr> connect_endpoint(const std::string& endpoint, Tick connect_timeout_micros) {
  if (endpoint.empty()) {
    return Status(ErrCode::InvalidArgument, "endpoint name is empty");
  }
  const std::string path = unix_path(endpoint);
  const Tick start = static_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch().count());
  for (;;) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return Status(ErrCode::IoError, "cannot create a unix socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return ChannelPtr(new SocketChannel(fd));
    }
    ::close(fd);
    const Tick now = static_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch().count());
    if (connect_timeout_micros != 0 && now - start >= connect_timeout_micros) {
      return Status(ErrCode::Unavailable, "endpoint did not become available in time", endpoint);
    }
    ::usleep(25000);
  }
}

#endif

std::string make_endpoint_name(const std::string& prefix) {
  static std::atomic<std::uint64_t> counter{0};
#if defined(_WIN32)
  const std::uint64_t pid = static_cast<std::uint64_t>(GetCurrentProcessId());
  const std::uint64_t stamp = static_cast<std::uint64_t>(GetTickCount64());
#else
  const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
  const std::uint64_t stamp = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
  const std::uint64_t suffix = counter.fetch_add(1);
  std::string name = prefix.empty() ? "ncf" : prefix;
  name.push_back('-');
  name.append(to_hex(pid));
  name.push_back('-');
  name.append(to_hex(stamp));
  name.push_back('-');
  name.append(to_hex(suffix));
  return name;
}

}  // namespace ncf
