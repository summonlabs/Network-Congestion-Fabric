// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/ipc/process.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
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

[[nodiscard]] std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  for (const char c : argument) {
    if (c == '"') {
      out.append("\\\"");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

ChildProcess::ChildProcess() noexcept = default;

struct ChildProcess::Impl {
  HANDLE process{nullptr};
  HANDLE thread{nullptr};
  HANDLE stdout_read{nullptr};
  HANDLE stderr_read{nullptr};
  std::thread stdout_reader{};
  std::thread stderr_reader{};
  std::mutex mutex{};
  std::string stdout_text{};
  std::string stderr_text{};
  std::uint64_t dropped_stdout{0};
  std::uint64_t dropped_stderr{0};
  std::size_t max_capture{4u << 20};
  bool capture_stdout{false};
  bool capture_stderr{false};
  bool reaped{false};
  int exit_code{0};
};

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(std::move(other.impl_)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    stop_capture();
    if (impl_ && impl_->process != nullptr) {
      TerminateProcess(impl_->process, 0xDEADu);
      WaitForSingleObject(impl_->process, INFINITE);
      CloseHandle(impl_->process);
      CloseHandle(impl_->thread);
      impl_->process = nullptr;
      impl_->thread = nullptr;
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  if (!impl_) {
    return;
  }
  if (impl_->process != nullptr) {
    DWORD code = 0;
    if (GetExitCodeProcess(impl_->process, &code) && code == STILL_ACTIVE) {
      TerminateProcess(impl_->process, 0xDEADu);
    }
    WaitForSingleObject(impl_->process, INFINITE);
  }
  stop_capture();
  if (impl_->process != nullptr) {
    CloseHandle(impl_->process);
    impl_->process = nullptr;
  }
  if (impl_->thread != nullptr) {
    CloseHandle(impl_->thread);
    impl_->thread = nullptr;
  }
}

Result<ChildProcess> ChildProcess::spawn(const ProcessOptions& options) {
  if (options.executable.empty()) {
    return Status(ErrCode::InvalidArgument, "child process executable is empty");
  }

  auto impl = std::make_unique<Impl>();
  impl->max_capture = options.max_capture_bytes;
  impl->capture_stdout = options.capture_stdout;
  impl->capture_stderr = options.capture_stderr;

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE stdout_write = nullptr;
  HANDLE stderr_write = nullptr;
  if (options.capture_stdout) {
    if (!CreatePipe(&impl->stdout_read, &stdout_write, &security, 0)) {
      return Status(ErrCode::IoError, "cannot create the stdout pipe");
    }
    SetHandleInformation(impl->stdout_read, HANDLE_FLAG_INHERIT, 0);
  }
  if (options.capture_stderr) {
    if (!CreatePipe(&impl->stderr_read, &stderr_write, &security, 0)) {
      if (stdout_write != nullptr) {
        CloseHandle(stdout_write);
      }
      return Status(ErrCode::IoError, "cannot create the stderr pipe");
    }
    SetHandleInformation(impl->stderr_read, HANDLE_FLAG_INHERIT, 0);
  }

  std::string command = quote_argument(options.executable);
  for (const std::string& argument : options.arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
  // Build the wide command line once. Calling widen() twice would pair begin()
  // from one temporary with end() from another.
  const std::wstring wide_command = widen(command);
  std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = options.capture_stdout ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = options.capture_stderr ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION info{};
  const std::wstring working_directory = widen(options.working_directory);
  const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr,
                                      working_directory.empty() ? nullptr : working_directory.c_str(),
                                      &startup, &info);
  if (stdout_write != nullptr) {
    CloseHandle(stdout_write);
  }
  if (stderr_write != nullptr) {
    CloseHandle(stderr_write);
  }
  if (!created) {
    if (impl->stdout_read != nullptr) {
      CloseHandle(impl->stdout_read);
      impl->stdout_read = nullptr;
    }
    if (impl->stderr_read != nullptr) {
      CloseHandle(impl->stderr_read);
      impl->stderr_read = nullptr;
    }
    return Status(ErrCode::IoError, "CreateProcess failed", options.executable);
  }

  impl->process = info.hProcess;
  impl->thread = info.hThread;

  ChildProcess child;
  child.impl_ = std::move(impl);

  if (options.capture_stdout && child.impl_->stdout_read != nullptr) {
    HANDLE handle = child.impl_->stdout_read;
    Impl* raw = child.impl_.get();
    raw->stdout_reader = std::thread([raw, handle]() {
      std::vector<char> buffer(16384);
      for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
        if (!ok || read == 0) {
          break;
        }
        std::lock_guard<std::mutex> guard(raw->mutex);
        const std::size_t room =
            raw->stdout_text.size() < raw->max_capture ? raw->max_capture - raw->stdout_text.size() : 0;
        const std::size_t keep = read < room ? read : room;
        raw->stdout_text.append(buffer.data(), keep);
        if (keep < read) {
          raw->dropped_stdout += (read - keep);
        }
      }
    });
  }
  if (options.capture_stderr && child.impl_->stderr_read != nullptr) {
    HANDLE handle = child.impl_->stderr_read;
    Impl* raw = child.impl_.get();
    raw->stderr_reader = std::thread([raw, handle]() {
      std::vector<char> buffer(16384);
      for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
        if (!ok || read == 0) {
          break;
        }
        std::lock_guard<std::mutex> guard(raw->mutex);
        const std::size_t room =
            raw->stderr_text.size() < raw->max_capture ? raw->max_capture - raw->stderr_text.size() : 0;
        const std::size_t keep = read < room ? read : room;
        raw->stderr_text.append(buffer.data(), keep);
        if (keep < read) {
          raw->dropped_stderr += (read - keep);
        }
      }
    });
  }
  return child;
}

std::uint32_t ChildProcess::pid() const noexcept {
  if (!impl_ || impl_->process == nullptr) {
    return 0;
  }
  return static_cast<std::uint32_t>(GetProcessId(impl_->process));
}

bool ChildProcess::running() noexcept {
  if (!impl_ || impl_->process == nullptr) {
    return false;
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(impl_->process, &code)) {
    return false;
  }
  return code == STILL_ACTIVE;
}

Result<int> ChildProcess::wait() {
  if (!impl_ || impl_->process == nullptr) {
    return Status(ErrCode::NotReady, "no child process is attached");
  }
  const DWORD waited = WaitForSingleObject(impl_->process, INFINITE);
  if (waited != WAIT_OBJECT_0) {
    return Status(ErrCode::IoError, "waiting for the child process failed");
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(impl_->process, &code)) {
    return Status(ErrCode::IoError, "reading the child exit code failed");
  }
  impl_->reaped = true;
  impl_->exit_code = static_cast<int>(code);
  // The child has exited, so its stdout writers are closed. Joining the reader
  // threads here cannot deadlock: neither thread takes a lock the waiting thread
  // holds.
  stop_capture();
  return impl_->exit_code;
}

VoidResult ChildProcess::terminate() {
  if (!impl_ || impl_->process == nullptr) {
    return Status(ErrCode::NotReady, "no child process is attached");
  }
  if (!TerminateProcess(impl_->process, 0xDEADu)) {
    const DWORD error = GetLastError();
    if (error != ERROR_ACCESS_DENIED) {
      return Status(ErrCode::IoError, "terminating the child process failed");
    }
  }
  return VoidResult{};
}

void ChildProcess::stop_capture() noexcept {
  if (!impl_) {
    return;
  }
  if (impl_->stdout_read != nullptr) {
    CancelIoEx(impl_->stdout_read, nullptr);
  }
  if (impl_->stderr_read != nullptr) {
    CancelIoEx(impl_->stderr_read, nullptr);
  }
  if (impl_->stdout_reader.joinable()) {
    impl_->stdout_reader.join();
  }
  if (impl_->stderr_reader.joinable()) {
    impl_->stderr_reader.join();
  }
  if (impl_->stdout_read != nullptr) {
    CloseHandle(impl_->stdout_read);
    impl_->stdout_read = nullptr;
  }
  if (impl_->stderr_read != nullptr) {
    CloseHandle(impl_->stderr_read);
    impl_->stderr_read = nullptr;
  }
}

std::string ChildProcess::captured_stdout() const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->stdout_text;
}

std::string ChildProcess::captured_stderr() const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->stderr_text;
}

std::uint64_t ChildProcess::dropped_stdout_bytes() const noexcept {
  if (!impl_) {
    return 0;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->dropped_stdout;
}

std::uint64_t ChildProcess::dropped_stderr_bytes() const noexcept {
  if (!impl_) {
    return 0;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->dropped_stderr;
}

Result<std::string> ChildProcess::current_executable_path() {
  std::vector<wchar_t> buffer(4096);
  const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (written == 0 || written >= buffer.size()) {
    return Status(ErrCode::IoError, "cannot determine the current executable path");
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(written), nullptr, 0,
                                         nullptr, nullptr);
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(written), out.data(), needed, nullptr,
                      nullptr);
  return out;
}

void sleep_micros(Tick micros) noexcept { Sleep(static_cast<DWORD>((micros + 999ull) / 1000ull)); }

#else  // POSIX

struct ChildProcess::Impl {
  int pid{-1};
  int stdout_fd{-1};
  int stderr_fd{-1};
  std::thread stdout_reader{};
  std::thread stderr_reader{};
  mutable std::mutex mutex{};
  std::string stdout_text{};
  std::string stderr_text{};
  std::uint64_t dropped_stdout{0};
  std::uint64_t dropped_stderr{0};
  std::size_t max_capture{4u << 20};
  int exit_code{0};
  bool reaped{false};
};

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(std::move(other.impl_)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    stop_capture();
    if (impl_ && impl_->pid > 0 && !impl_->reaped) {
      ::kill(impl_->pid, SIGKILL);
      int status = 0;
      ::waitpid(impl_->pid, &status, 0);
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  if (!impl_) {
    return;
  }
  if (impl_->pid > 0 && !impl_->reaped) {
    ::kill(impl_->pid, SIGKILL);
    int status = 0;
    ::waitpid(impl_->pid, &status, 0);
  }
  stop_capture();
}

Result<ChildProcess> ChildProcess::spawn(const ProcessOptions& options) {
  if (options.executable.empty()) {
    return Status(ErrCode::InvalidArgument, "child process executable is empty");
  }
  auto impl = std::make_unique<Impl>();
  impl->max_capture = options.max_capture_bytes;

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (options.capture_stdout && ::pipe(stdout_pipe) != 0) {
    return Status(ErrCode::IoError, "cannot create the stdout pipe");
  }
  if (options.capture_stderr && ::pipe(stderr_pipe) != 0) {
    return Status(ErrCode::IoError, "cannot create the stderr pipe");
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    return Status(ErrCode::IoError, "fork failed");
  }
  if (pid == 0) {
    if (options.capture_stdout) {
      ::dup2(stdout_pipe[1], STDOUT_FILENO);
      ::close(stdout_pipe[0]);
      ::close(stdout_pipe[1]);
    }
    if (options.capture_stderr) {
      ::dup2(stderr_pipe[1], STDERR_FILENO);
      ::close(stderr_pipe[0]);
      ::close(stderr_pipe[1]);
    }
    if (!options.working_directory.empty()) {
      ::chdir(options.working_directory.c_str());
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(options.executable.c_str()));
    for (const std::string& argument : options.arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(options.executable.c_str(), argv.data());
    ::_exit(127);
  }

  if (options.capture_stdout) {
    ::close(stdout_pipe[1]);
    impl->stdout_fd = stdout_pipe[0];
  }
  if (options.capture_stderr) {
    ::close(stderr_pipe[1]);
    impl->stderr_fd = stderr_pipe[0];
  }
  impl->pid = static_cast<int>(pid);

  ChildProcess child;
  child.impl_ = std::move(impl);

  if (options.capture_stdout) {
    Impl* raw = child.impl_.get();
    const int fd = raw->stdout_fd;
    raw->stdout_reader = std::thread([raw, fd]() {
      std::vector<char> buffer(16384);
      for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count <= 0) {
          break;
        }
        std::lock_guard<std::mutex> guard(raw->mutex);
        const std::size_t room =
            raw->stdout_text.size() < raw->max_capture ? raw->max_capture - raw->stdout_text.size() : 0;
        const std::size_t keep = static_cast<std::size_t>(count) < room ? static_cast<std::size_t>(count) : room;
        raw->stdout_text.append(buffer.data(), keep);
        if (keep < static_cast<std::size_t>(count)) {
          raw->dropped_stdout += static_cast<std::size_t>(count) - keep;
        }
      }
    });
  }
  if (options.capture_stderr) {
    Impl* raw = child.impl_.get();
    const int fd = raw->stderr_fd;
    raw->stderr_reader = std::thread([raw, fd]() {
      std::vector<char> buffer(16384);
      for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count <= 0) {
          break;
        }
        std::lock_guard<std::mutex> guard(raw->mutex);
        const std::size_t room =
            raw->stderr_text.size() < raw->max_capture ? raw->max_capture - raw->stderr_text.size() : 0;
        const std::size_t keep = static_cast<std::size_t>(count) < room ? static_cast<std::size_t>(count) : room;
        raw->stderr_text.append(buffer.data(), keep);
        if (keep < static_cast<std::size_t>(count)) {
          raw->dropped_stderr += static_cast<std::size_t>(count) - keep;
        }
      }
    });
  }
  return child;
}

std::uint32_t ChildProcess::pid() const noexcept {
  return impl_ ? static_cast<std::uint32_t>(impl_->pid) : 0;
}

bool ChildProcess::running() noexcept {
  if (!impl_ || impl_->pid <= 0 || impl_->reaped) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(impl_->pid, &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  impl_->reaped = true;
  impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return false;
}

Result<int> ChildProcess::wait() {
  if (!impl_ || impl_->pid <= 0) {
    return Status(ErrCode::NotReady, "no child process is attached");
  }
  if (!impl_->reaped) {
    int status = 0;
    if (::waitpid(impl_->pid, &status, 0) < 0) {
      return Status(ErrCode::IoError, "waitpid failed");
    }
    impl_->reaped = true;
    impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  stop_capture();
  return impl_->exit_code;
}

VoidResult ChildProcess::terminate() {
  if (!impl_ || impl_->pid <= 0) {
    return Status(ErrCode::NotReady, "no child process is attached");
  }
  ::kill(impl_->pid, SIGKILL);
  return VoidResult{};
}

void ChildProcess::stop_capture() noexcept {
  if (!impl_) {
    return;
  }
  if (impl_->stdout_reader.joinable()) {
    impl_->stdout_reader.join();
  }
  if (impl_->stderr_reader.joinable()) {
    impl_->stderr_reader.join();
  }
  if (impl_->stdout_fd >= 0) {
    ::close(impl_->stdout_fd);
    impl_->stdout_fd = -1;
  }
  if (impl_->stderr_fd >= 0) {
    ::close(impl_->stderr_fd);
    impl_->stderr_fd = -1;
  }
}

std::string ChildProcess::captured_stdout() const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->stdout_text;
}

std::string ChildProcess::captured_stderr() const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->stderr_text;
}

std::uint64_t ChildProcess::dropped_stdout_bytes() const noexcept { return impl_ ? impl_->dropped_stdout : 0; }
std::uint64_t ChildProcess::dropped_stderr_bytes() const noexcept { return impl_ ? impl_->dropped_stderr : 0; }

Result<std::string> ChildProcess::current_executable_path() {
  std::vector<char> buffer(4096);
  const ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (count <= 0) {
    return Status(ErrCode::IoError, "cannot determine the current executable path");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(count));
}

void sleep_micros(Tick micros) noexcept { ::usleep(static_cast<useconds_t>(micros)); }

#endif

}  // namespace ncf
