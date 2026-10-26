#pragma once
#include <windows.h>
#include <filesystem>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

namespace dc {
namespace fs = std::filesystem;
using u64 = unsigned long long;
struct Volume {
    std::wstring id, root, label, kind;
    u64 free = 0, total = 0;
    bool removable = false;
    DWORD diskNumber = MAXDWORD;
};
struct Location {
    std::wstring volume, label, relative;
};
std::vector<Volume> volumes();
Location bind(const fs::path& path);
fs::path resolve(const Location& path);
std::wstring errorText(DWORD code = GetLastError());
std::wstring bytes(u64 n);
std::string utf8(const std::wstring& text);
fs::path dataDirectory();
bool safeEject(const Volume& volume, std::wstring& error);
std::wstring analytics(const fs::path& directory = dataDirectory());

enum class State : unsigned { Queued, Scanning, Copying, Paused, Waiting, Interrupted, Done, Errors, Cancelled };
std::wstring stateText(State state);
struct File {
    Location source;
    std::wstring relative;
    u64 size = 0, modified = 0, offset = 0;
    bool done = false, skipped = false;
};
struct Job {
    u64 id = 0;
    std::wstring name;
    Location target;
    std::vector<Location> sources;
    std::vector<File> files;
    std::vector<std::wstring> directories;
    State state = State::Queued;
    u64 total = 0, completed = 0, skipped = 0;
    unsigned errors = 0;
    bool scanned = false;
    double speed = 0;
    std::wstring current, detail;
};
struct Snapshot {
    std::vector<Job> jobs;
    std::vector<std::wstring> log;
    bool night = false;
    bool recovery = false;
    std::wstring storageError;
};

class Engine {
public:
    explicit Engine(fs::path directory = dataDirectory());
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    u64 enqueue(const std::vector<fs::path>& sources, const fs::path& target);
    Snapshot snapshot() const;
    std::vector<File> fileDetails(u64 id, size_t limit = 2000) const;
    void pause(u64 id);
    void resume(u64 id);
    void cancel(u64 id);
    void move(u64 id, int delta);
    void resumeAll();
    void setNight(bool enabled);
    void devicesChanged();
    void shutdown();
    std::function<void(const std::wstring&)> onFinished;
private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    fs::path directory_;
    std::vector<Job> jobs_;
    std::vector<std::wstring> log_;
    bool night_ = false, recovery_ = false;
    u64 nextId_ = 1;
    std::wstring storageError_;
    void run();
    void process(u64 id);
    void scan(u64 id);
    void copy(u64 id, size_t index);
    bool interrupted(u64 id);
    Job& find(u64 id);
    bool save(); // mutex held; pause the queue if recovery cannot be persisted
    void load();
    void log(u64 id, const std::wstring& message);
    void metric(const Job& job, const File& file, u64 transferred, double seconds, const std::wstring& event);
};
}
