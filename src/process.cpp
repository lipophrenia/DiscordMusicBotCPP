#include "process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace musicbot {
namespace {

constexpr std::size_t kMaximumErrorBytes = 1024U * 1024U;

#ifdef _WIN32

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE release() noexcept {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_{};
};

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("failed to convert a process argument to UTF-16");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring quote_windows_argument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result{L'"'};
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

ProcessResult stream_process_platform(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const OutputHandler& output_handler,
    std::stop_token stop_token) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE stdout_read_raw{};
    HANDLE stdout_write_raw{};
    HANDLE stderr_read_raw{};
    HANDLE stderr_write_raw{};
    if (!CreatePipe(&stdout_read_raw, &stdout_write_raw, &attributes, 0)) {
        throw std::runtime_error("failed to create child stdout pipe");
    }
    UniqueHandle stdout_read(stdout_read_raw);
    UniqueHandle stdout_write(stdout_write_raw);
    if (!CreatePipe(&stderr_read_raw, &stderr_write_raw, &attributes, 0)) {
        throw std::runtime_error("failed to create child stderr pipe");
    }
    UniqueHandle stderr_read(stderr_read_raw);
    UniqueHandle stderr_write(stderr_write_raw);
    SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quote_windows_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_windows_argument(widen(argument));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write.get();
    startup.hStdError = stderr_write.get();
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_info)) {
        throw std::runtime_error("failed to start " + executable.string());
    }
    UniqueHandle process(process_info.hProcess);
    UniqueHandle process_thread(process_info.hThread);
    stdout_write.reset();
    stderr_write.reset();

    ProcessResult result;
    std::atomic_bool cancelled{false};
    std::thread stderr_thread([&] {
        std::array<char, 4096> bytes{};
        DWORD count{};
        while (ReadFile(stderr_read.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                        &count, nullptr) && count > 0) {
            const auto used = std::min(kMaximumErrorBytes, result.error_output.size());
            result.error_output.append(bytes.data(), std::min<std::size_t>(count,
                                                                           kMaximumErrorBytes - used));
        }
    });

    std::atomic_bool complete{false};
    std::jthread cancellation_thread([&](std::stop_token local_stop) {
        using namespace std::chrono_literals;
        while (!complete.load(std::memory_order_acquire) && !local_stop.stop_requested()) {
            if (stop_token.stop_requested()) {
                cancelled.store(true, std::memory_order_release);
                TerminateProcess(process.get(), 130);
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    std::exception_ptr callback_error;
    std::array<std::uint8_t, 64U * 1024U> bytes{};
    DWORD count{};
    while (ReadFile(stdout_read.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                    &count, nullptr) && count > 0) {
        try {
            if (!output_handler(std::span(bytes.data(), static_cast<std::size_t>(count)))) {
                cancelled.store(true, std::memory_order_release);
                TerminateProcess(process.get(), 130);
                break;
            }
        } catch (...) {
            callback_error = std::current_exception();
            TerminateProcess(process.get(), 1);
            break;
        }
    }

    WaitForSingleObject(process.get(), INFINITE);
    complete.store(true, std::memory_order_release);
    cancellation_thread.request_stop();
    cancellation_thread.join();
    DWORD exit_code{};
    GetExitCodeProcess(process.get(), &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    result.cancelled = cancelled.load(std::memory_order_acquire);
    stdout_read.reset();
    stderr_thread.join();
    if (callback_error) {
        std::rethrow_exception(callback_error);
    }
    return result;
}

#else

ProcessResult stream_process_platform(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const OutputHandler& output_handler,
    std::stop_token stop_token) {
    // Prepare argv before fork. The bot is multithreaded, so the child must only
    // call async-signal-safe functions until execvp.
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    int stdout_pipe[2]{};
    int stderr_pipe[2]{};
    if (pipe(stdout_pipe) != 0) {
        throw std::runtime_error("failed to create child stdout pipe");
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        throw std::runtime_error("failed to create child stderr pipe");
    }

    const auto pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        throw std::runtime_error("failed to create child process");
    }
    if (pid == 0) {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        execvp(argv.front(), argv.data());
        static constexpr char prefix[] = "failed to execute ";
        static constexpr char suffix[] = "\n";
        static_cast<void>(write(STDERR_FILENO, prefix, sizeof(prefix) - 1));
        static_cast<void>(write(STDERR_FILENO, storage.front().data(), storage.front().size()));
        static_cast<void>(write(STDERR_FILENO, suffix, sizeof(suffix) - 1));
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    ProcessResult result;
    std::atomic_bool cancelled{false};
    std::thread stderr_thread([&] {
        std::array<char, 4096> bytes{};
        ssize_t count{};
        while ((count = read(stderr_pipe[0], bytes.data(), bytes.size())) > 0) {
            const auto used = std::min(kMaximumErrorBytes, result.error_output.size());
            result.error_output.append(bytes.data(), std::min<std::size_t>(
                                                        static_cast<std::size_t>(count),
                                                        kMaximumErrorBytes - used));
        }
        close(stderr_pipe[0]);
    });

    std::atomic_bool complete{false};
    std::jthread cancellation_thread([&](std::stop_token local_stop) {
        using namespace std::chrono_literals;
        while (!complete.load(std::memory_order_acquire) && !local_stop.stop_requested()) {
            if (stop_token.stop_requested()) {
                cancelled.store(true, std::memory_order_release);
                kill(pid, SIGTERM);
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    std::exception_ptr callback_error;
    std::array<std::uint8_t, 64U * 1024U> bytes{};
    ssize_t count{};
    while ((count = read(stdout_pipe[0], bytes.data(), bytes.size())) > 0) {
        try {
            if (!output_handler(std::span(bytes.data(), static_cast<std::size_t>(count)))) {
                cancelled.store(true, std::memory_order_release);
                kill(pid, SIGTERM);
                break;
            }
        } catch (...) {
            callback_error = std::current_exception();
            kill(pid, SIGTERM);
            break;
        }
    }
    close(stdout_pipe[0]);
    int status{};
    waitpid(pid, &status, 0);
    complete.store(true, std::memory_order_release);
    cancellation_thread.request_stop();
    cancellation_thread.join();
    stderr_thread.join();
    result.cancelled = cancelled.load(std::memory_order_acquire);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    if (callback_error) {
        std::rethrow_exception(callback_error);
    }
    return result;
}

#endif

}  // namespace

ProcessResult stream_process(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const OutputHandler& output_handler,
    std::stop_token stop_token) {
    return stream_process_platform(executable, arguments, output_handler, stop_token);
}

ProcessResult capture_process(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    std::size_t maximum_output_bytes,
    std::stop_token stop_token) {
    std::vector<std::uint8_t> captured;
    captured.reserve(std::min(maximum_output_bytes, std::size_t{256U * 1024U}));
    auto result = stream_process(
        executable, arguments,
        [&](std::span<const std::uint8_t> bytes) {
            const auto used = std::min(maximum_output_bytes, captured.size());
            if (bytes.size() > maximum_output_bytes - used) {
                throw std::runtime_error("child process returned too much data");
            }
            captured.insert(captured.end(), bytes.begin(), bytes.end());
            return true;
        },
        stop_token);
    result.output = std::move(captured);
    return result;
}

}  // namespace musicbot
