#include "local_audio.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace musicbot {
namespace {

std::string utf8_filename(const std::filesystem::path& path) {
    const auto value = path.filename().u8string();
    return {value.begin(), value.end()};
}

bool allowed_filename(const std::string& filename) {
    if (filename.empty() || filename.size() > 255 || filename.front() == '.' ||
        filename.find_first_of("/\\:") != std::string::npos ||
        std::any_of(filename.begin(), filename.end(), [](unsigned char c) {
            return c < 32 || c == 127;
        })) return false;
    auto extension = std::filesystem::u8path(filename).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".webm";
}

#ifndef _WIN32
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const noexcept { return fd_; }
private:
    int fd_;
};
#endif
}  // namespace

LocalAudioLibrary::LocalAudioLibrary(std::filesystem::path directory)
    : directory_(std::filesystem::absolute(std::move(directory)).lexically_normal()) {}

LocalAudioPage LocalAudioLibrary::list(std::size_t page) const {
    if (page == 0) throw LocalAudioError("Номер страницы должен быть больше нуля.");
    std::error_code error;
    const auto status = std::filesystem::status(directory_, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(status))) return {};
    if (error || !std::filesystem::is_directory(status)) {
        throw LocalAudioError("Каталог локальной музыки недоступен. Проверьте LOCAL_AUDIO_DIR.");
    }
    std::vector<std::string> names;
    std::filesystem::directory_iterator iterator(directory_, error);
    if (error) throw LocalAudioError("Не удалось прочитать каталог локальной музыки.");
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const auto& entry = *iterator;
        const auto filename = utf8_filename(entry.path());
        if (allowed_filename(filename)) {
            const auto entry_status = entry.symlink_status(error);
            if (!error && std::filesystem::is_regular_file(entry_status)) names.push_back(filename);
        }
        iterator.increment(error);
        if (error) throw LocalAudioError("Не удалось прочитать каталог локальной музыки.");
    }
    std::sort(names.begin(), names.end());
    LocalAudioPage result;
    result.total_files = names.size();
    result.total_pages = (names.size() + page_size - 1) / page_size;
    if (names.empty()) return result;
    if (page > result.total_pages) throw LocalAudioError("Такой страницы нет. Используйте /list-local page:1.");
    const auto start = (page - 1) * page_size;
    const auto count = std::min(page_size, names.size() - start);
    result.filenames.assign(names.begin() + start, names.begin() + start + count);
    return result;
}

std::filesystem::path LocalAudioLibrary::resolve(const std::string& filename) const {
    if (!allowed_filename(filename)) {
        throw LocalAudioError("Укажите имя .webm-файла из /list-local, без пути к каталогу.");
    }
    const auto path = directory_ / std::filesystem::u8path(filename);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        throw LocalAudioError("Локальная запись не найдена. Обновите список через /list-local.");
    }
    return path;
}

Track LocalAudioLibrary::create_track(const std::string& filename, std::uint64_t requester_id,
                                      std::string requester_name, std::uint64_t text_channel_id) const {
    const auto path = resolve(filename);
    Track track;
    const auto title = path.stem().u8string();
    track.title.assign(title.begin(), title.end());
    track.local_filename = filename;
    track.requester_id = requester_id;
    track.requester_name = std::move(requester_name);
    track.text_channel_id = text_channel_id;
    return track;
}

bool LocalAudioLibrary::stream(const std::string& filename, const OutputHandler& output,
                               std::stop_token stop) const {
    if (stop.stop_requested()) return false;
    if (!output) throw std::invalid_argument("missing local audio output handler");
    const auto path = resolve(filename); // Revalidate when dequeued, not just when selected.
    std::array<std::uint8_t, 64U * 1024U> buffer{};
#ifndef _WIN32
    // A file replaced by a symlink or FIFO after validation must not be followed
    // or block the worker indefinitely.
    FileDescriptor input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status{};
    if (input.get() < 0 || fstat(input.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        throw LocalAudioError("Не удалось открыть локальную запись для чтения.");
    }
    while (!stop.stop_requested()) {
        const auto count = read(input.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw LocalAudioError("Ошибка чтения локальной записи.");
        if (count == 0) return true;
        if (!output(std::span(buffer.data(), static_cast<std::size_t>(count)))) return false;
    }
#else
    std::ifstream input(path, std::ios::binary);
    if (!input) throw LocalAudioError("Не удалось открыть локальную запись для чтения.");
    while (!stop.stop_requested()) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && !output(std::span(buffer.data(), static_cast<std::size_t>(count)))) return false;
        if (input.bad()) throw LocalAudioError("Ошибка чтения локальной записи.");
        if (input.eof()) return true;
        if (input.fail()) throw LocalAudioError("Ошибка чтения локальной записи.");
    }
#endif
    return false;
}
}  // namespace musicbot
