#include "core.hpp"
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <cwctype>
#include <cstring>

namespace dc {
namespace {
constexpr DWORD blockSize = 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { close(); }
    void close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return value; }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
void check(BOOL ok) { if (!ok) throw std::system_error(static_cast<int>(GetLastError()), std::system_category()); }
void seek(HANDLE h, u64 offset) { LARGE_INTEGER n{}; n.QuadPart = offset; check(SetFilePointerEx(h, n, nullptr, FILE_BEGIN)); }
u64 length(HANDLE h) { LARGE_INTEGER n{}; check(GetFileSizeEx(h, &n)); return n.QuadPart; }
u64 modified(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{}; check(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d));
    return (static_cast<u64>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}
template<class T> void put(std::ostream& out, T n) { out.write(reinterpret_cast<const char*>(&n), sizeof(n)); }
template<class T> T get(std::istream& in) {
    T n{}; in.read(reinterpret_cast<char*>(&n), sizeof(n)); if (!in) throw std::runtime_error("Incomplete checkpoint"); return n;
}
void putString(std::ostream& out, const std::wstring& s) {
    put(out, static_cast<unsigned>(s.size())); out.write(reinterpret_cast<const char*>(s.data()), s.size() * sizeof(wchar_t));
}
std::wstring getString(std::istream& in) {
    unsigned n = get<unsigned>(in); if (n > 32768) throw std::runtime_error("Invalid checkpoint path");
    std::wstring s(n, 0); in.read(reinterpret_cast<char*>(s.data()), n * sizeof(wchar_t));
    if (!in) throw std::runtime_error("Incomplete checkpoint"); return s;
}
void putLocation(std::ostream& out, const Location& l) { putString(out,l.volume); putString(out,l.label); putString(out,l.relative); }
Location getLocation(std::istream& in) {
    Location l; l.volume=getString(in); l.label=getString(in); l.relative=getString(in);
    auto rel=fs::path(l.relative);
    if (rel.is_absolute() || rel.has_root_name()) throw std::runtime_error("Absolute path in checkpoint");
    for (const auto& part : rel) if (part == L"..") throw std::runtime_error("Path escapes the volume");
    if (l.volume.rfind(L"\\\\?\\Volume{",0)!=0 || l.volume.back()!=L'\\') throw std::runtime_error("Invalid volume identity");
    return l;
}
bool terminal(State s) { return s==State::Done || s==State::Errors || s==State::Cancelled; }
bool deviceError(const std::exception& error) {
    auto system=dynamic_cast<const std::system_error*>(&error);
    if(!system) return false;
    switch(system->code().value()) {
    case ERROR_DEVICE_NOT_CONNECTED: case ERROR_NOT_READY: case ERROR_DEV_NOT_EXIST: case ERROR_NO_SUCH_DEVICE:
        return true;
    default: return false;
    }
}
bool descendant(const fs::path& child, const fs::path& parent) {
    auto a=child.lexically_normal().wstring(), b=parent.lexically_normal().wstring();
    std::transform(a.begin(),a.end(),a.begin(),towlower); std::transform(b.begin(),b.end(),b.begin(),towlower);
    if (a==b) return true;
    if (!b.empty() && b.back()!=L'\\') b+=L'\\';
    return a.rfind(b,0)==0;
}
void rejectReparse(const fs::path& path) {
    for (auto p=path; !p.empty() && p!=p.parent_path(); p=p.parent_path()) {
        DWORD attr=GetFileAttributesW(p.c_str());
        if (attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Symbolic links, junctions, and redirected paths are not supported");
    }
}
void safeRelative(const std::wstring& path) {
    fs::path p(path);
    if (p.empty() || p.is_absolute() || p.has_root_name()) throw std::runtime_error("Invalid relative path");
    for (auto& part:p) if (part==L"..") throw std::runtime_error("Path escapes the destination");
}
std::string quote(const std::wstring& s) {
    std::string out="'";
    for(char c:utf8(s)) { out+=c; if(c=='\'') out+='\''; }
    return out+"'";
}
}

Engine::Engine(fs::path directory) : directory_(std::move(directory)) {
    fs::create_directories(directory_); load(); worker_=std::thread([this]{run();});
}
Engine::~Engine() { shutdown(); }
void Engine::shutdown() {
    stop_=true; wake_.notify_all(); if(worker_.joinable()) worker_.join();
}
Job& Engine::find(u64 id) {
    auto it=std::find_if(jobs_.begin(),jobs_.end(),[id](const Job& j){return j.id==id;});
    if(it==jobs_.end()) throw std::runtime_error("Batch not found"); return *it;
}
Snapshot Engine::snapshot() const {
    std::lock_guard lock(mutex_); Snapshot s;
    for(const auto& j:jobs_) {
        Job summary; summary.id=j.id; summary.name=j.name; summary.target=j.target; summary.state=j.state;
        summary.total=j.total; summary.completed=j.completed; summary.skipped=j.skipped; summary.errors=j.errors;
        summary.speed=j.speed; summary.current=j.current; summary.detail=j.detail; summary.scanned=j.scanned; summary.sources=j.sources;
        s.jobs.push_back(std::move(summary));
    }
    s.log=log_; s.night=night_; s.recovery=recovery_; s.storageError=storageError_; return s;
}
std::vector<File> Engine::fileDetails(u64 id,size_t limit) const {
    std::lock_guard lock(mutex_);
    for(const auto& j:jobs_) if(j.id==id) return {j.files.begin(),j.files.begin()+std::min(limit,j.files.size())};
    return {};
}
u64 Engine::enqueue(const std::vector<fs::path>& paths,const fs::path& destination) {
    if(paths.empty()) throw std::runtime_error("Select files or folders");
    rejectReparse(fs::absolute(destination));
    if(!fs::is_directory(destination)) throw std::runtime_error("The destination must be an existing folder");
    Job job; job.target=bind(destination);
    std::vector<std::wstring> names;
    for(auto& p:paths) {
        auto full=fs::absolute(p).lexically_normal(); rejectReparse(full);
        if(!fs::exists(full) || full.filename().empty()) throw std::runtime_error("The source does not exist or is a volume root");
        auto sourceLocation=bind(full);
        if(descendant(fs::absolute(destination),full) ||
           (sourceLocation.volume==job.target.volume && descendant(job.target.relative,sourceLocation.relative)))
            throw std::runtime_error("The destination cannot be inside the source");
        auto name=full.filename().wstring(); auto lower=name;
        std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
        if(std::find(names.begin(),names.end(),lower)!=names.end()) throw std::runtime_error("Two sources have the same name; add them as separate batches");
        names.push_back(lower); job.sources.push_back(sourceLocation);
    }
    job.name=paths.front().filename().wstring(); if(paths.size()>1) job.name+=L" + "+std::to_wstring(paths.size()-1);
    std::lock_guard lock(mutex_); job.id=nextId_++; auto id=job.id;
    if(recovery_) job.state=State::Paused;
    jobs_.push_back(std::move(job)); save(); wake_.notify_all(); return id;
}
void Engine::pause(u64 id) {
    std::lock_guard lock(mutex_); auto& j=find(id);
    if(!terminal(j.state)) { j.state=State::Paused; j.speed=0; save(); }
}
void Engine::resume(u64 id) {
    std::lock_guard lock(mutex_); auto& j=find(id);
    if(!terminal(j.state)) { recovery_=false; j.state=State::Queued; j.detail.clear(); save(); wake_.notify_all(); }
}
void Engine::cancel(u64 id) {
    std::lock_guard lock(mutex_); auto& j=find(id);
    if(!terminal(j.state)) { j.state=State::Cancelled; j.speed=0; j.detail=L"Partial files are kept; source files are never deleted."; save(); }
}
void Engine::move(u64 id,int delta) {
    std::lock_guard lock(mutex_);
    for(size_t i=0;i<jobs_.size();++i) if(jobs_[i].id==id) {
        auto other=static_cast<long long>(i)+delta;
        if(other>=0 && other<static_cast<long long>(jobs_.size())) std::swap(jobs_[i],jobs_[static_cast<size_t>(other)]);
        break;
    } save(); wake_.notify_all();
}
void Engine::resumeAll() {
    std::lock_guard lock(mutex_); recovery_=false;
    for(auto& j:jobs_) if(!terminal(j.state)) { j.state=State::Queued; j.detail.clear(); }
    save(); wake_.notify_all();
}
void Engine::setNight(bool enabled) { std::lock_guard lock(mutex_); night_=enabled; save(); }
void Engine::devicesChanged() { wake_.notify_all(); }
bool Engine::interrupted(u64 id) {
    if(stop_) return true;
    std::lock_guard lock(mutex_); auto state=find(id).state;
    return state==State::Paused || state==State::Cancelled || state==State::Interrupted;
}
void Engine::log(u64 id,const std::wstring& message) {
    SYSTEMTIME t{}; GetLocalTime(&t); wchar_t prefix[64];
    swprintf_s(prefix,L"%02d:%02d:%02d  #%llu  ",t.wHour,t.wMinute,t.wSecond,id);
    std::wstring line=prefix+message;
    { std::lock_guard lock(mutex_); log_.push_back(line); if(log_.size()>250) log_.erase(log_.begin()); }
    std::ofstream out(directory_/L"events.log",std::ios::app|std::ios::binary); out<<utf8(line)<<"\n";
}
void Engine::save() {
    try {
        auto tmp=directory_/L"session.tmp";
        std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
        put(out,0x44435031u); put(out,nextId_); put(out,night_); put(out,static_cast<unsigned>(jobs_.size()));
        for(auto& j:jobs_) {
            put(out,j.id); putString(out,j.name); putLocation(out,j.target); put(out,static_cast<unsigned>(j.state));
            put(out,j.scanned); put(out,j.total); put(out,j.completed); put(out,j.skipped); put(out,j.errors);
            put(out,static_cast<unsigned>(j.sources.size())); for(auto& s:j.sources) putLocation(out,s);
            put(out,static_cast<unsigned>(j.directories.size())); for(auto& d:j.directories) putString(out,d);
            put(out,static_cast<unsigned>(j.files.size()));
            for(auto& f:j.files) {
                putLocation(out,f.source); putString(out,f.relative); put(out,f.size); put(out,f.modified);
                put(out,f.offset); put(out,f.done); put(out,f.skipped);
            }
        }
        out.flush(); if(!out) throw std::runtime_error("Could not write the checkpoint"); out.close();
        Handle h(CreateFileW(tmp.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        check(static_cast<BOOL>(static_cast<bool>(h))); check(FlushFileBuffers(h)); h.close();
        check(MoveFileExW(tmp.c_str(),(directory_/L"session.dat").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH));
        storageError_.clear();
    } catch(const std::exception& e) { auto s=std::string(e.what()); storageError_=L"Could not save the session: "+std::wstring(s.begin(),s.end()); }
}
void Engine::load() {
    auto file=directory_/L"session.dat"; if(!fs::exists(file)) return;
    try {
        std::ifstream in(file,std::ios::binary); if(get<unsigned>(in)!=0x44435031u) throw std::runtime_error("Unknown checkpoint version");
        nextId_=get<u64>(in); night_=get<bool>(in); auto count=get<unsigned>(in);
        if(count>100000) throw std::runtime_error("Too many batches");
        std::vector<Job> loaded;
        for(unsigned i=0;i<count;++i) {
            Job j; j.id=get<u64>(in); j.name=getString(in); j.target=getLocation(in);
            auto state=get<unsigned>(in); if(state>static_cast<unsigned>(State::Cancelled)) throw std::runtime_error("Invalid state");
            j.state=static_cast<State>(state); j.scanned=get<bool>(in); j.total=get<u64>(in); j.completed=get<u64>(in);
            j.skipped=get<u64>(in); j.errors=get<unsigned>(in);
            auto n=get<unsigned>(in); if(n>100000) throw std::runtime_error("Too many sources");
            for(unsigned k=0;k<n;++k) j.sources.push_back(getLocation(in));
            n=get<unsigned>(in); if(n>5000000) throw std::runtime_error("Too many folders");
            for(unsigned k=0;k<n;++k) { auto rel=getString(in); safeRelative(rel); j.directories.push_back(rel); }
            n=get<unsigned>(in); if(n>5000000) throw std::runtime_error("Too many files");
            for(unsigned k=0;k<n;++k) {
                File f; f.source=getLocation(in); f.relative=getString(in); safeRelative(f.relative);
                f.size=get<u64>(in); f.modified=get<u64>(in); f.offset=get<u64>(in); f.done=get<bool>(in); f.skipped=get<bool>(in);
                if(f.offset>f.size) throw std::runtime_error("Invalid offset"); j.files.push_back(f);
            }
            if(!terminal(j.state)) { j.state=State::Interrupted; recovery_=true; }
            loaded.push_back(std::move(j));
        }
        jobs_=std::move(loaded);
    } catch(const std::exception&) {
        // Keep the original for diagnosis; never overwrite a damaged checkpoint automatically.
        storageError_=L"Unreadable session. session.dat is preserved; inspect it before adding new batches.";
        throw std::runtime_error("The session checkpoint is damaged. Preserve session.dat and move it before starting a new session.");
    }
}
void Engine::run() {
    while(!stop_) {
        u64 id=0;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock,std::chrono::seconds(2),[this]{
                return stop_ || (!recovery_ && std::any_of(jobs_.begin(),jobs_.end(),[](const Job& j){return j.state==State::Queued;}));
            });
            if(stop_) break;
            if(recovery_) continue;
            for(auto& j:jobs_) {
                if(j.state!=State::Queued && j.state!=State::Waiting) continue;
                bool connected=!resolve(j.target).empty();
                for(auto& source:j.sources) connected=connected && !resolve(source).empty();
                if(!connected) { j.state=State::Waiting; j.speed=0; continue; }
                id=j.id; j.state=j.scanned?State::Copying:State::Scanning; break;
            }
        }
        if(id) {
            SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED);
            try { process(id); }
            catch(const std::exception& e) {
                auto text=std::string(e.what()); log(id,L"Batch error: "+std::wstring(text.begin(),text.end()));
                std::lock_guard lock(mutex_); auto& j=find(id);
                if(!terminal(j.state) && j.state!=State::Paused) { j.state=State::Errors; ++j.errors; }
                j.speed=0; save();
            }
            SetThreadExecutionState(ES_CONTINUOUS);
        }
    }
    std::lock_guard lock(mutex_);
    for(auto& j:jobs_) if(j.state==State::Copying || j.state==State::Scanning) j.state=State::Interrupted;
    save();
}
void Engine::scan(u64 id) {
    Job local;
    { std::lock_guard lock(mutex_); local=find(id); }
    local.files.clear(); local.directories.clear(); local.total=0; local.errors=0;
    auto addFile=[&](const fs::path& path,const fs::path& relative) {
        if(interrupted(id)) return;
        try {
            rejectReparse(path); File f; f.source=bind(path); f.relative=relative.wstring();
            f.size=fs::file_size(path); f.modified=modified(path); local.total+=f.size; local.files.push_back(f);
        } catch(const std::exception& e) {
            ++local.errors; auto s=std::string(e.what()); log(id,L"Skipped due to an error: "+path.wstring()+L" · "+std::wstring(s.begin(),s.end()));
        }
    };
    for(auto& source:local.sources) {
        if(interrupted(id)) return;
        auto root=resolve(source); if(root.empty()) throw std::runtime_error("Device disconnected during scanning");
        auto top=fs::path(source.relative).filename();
        if(fs::is_regular_file(root)) { addFile(root,top); continue; }
        if(!fs::is_directory(root)) throw std::runtime_error("The source no longer exists");
        local.directories.push_back(top.wstring());
        // Explicit traversal logs unreadable directories instead of silently omitting them.
        std::vector<fs::path> pending{root};
        while(!pending.empty()) {
            if(interrupted(id)) return;
            auto dir=pending.back(); pending.pop_back();
            std::error_code ec; fs::directory_iterator it(dir,ec), end;
            if(ec) { ++local.errors; log(id,L"Folder skipped due to an error: "+dir.wstring()); continue; }
            for(;it!=end;) {
                if(interrupted(id)) return;
                if(ec) { ++local.errors; log(id,L"Error enumerating: "+dir.wstring()); break; }
                auto p=it->path(); auto relative=top/p.lexically_relative(root);
                DWORD attr=GetFileAttributesW(p.c_str());
                if(attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_REPARSE_POINT)) {
                    ++local.errors; log(id,L"Skipped (link or inaccessible): "+p.wstring());
                } else {
                    if(attr&FILE_ATTRIBUTE_DIRECTORY) { local.directories.push_back(relative.wstring()); pending.push_back(p); }
                    else addFile(p,relative);
                }
                it.increment(ec);
                if(ec) { ++local.errors; log(id,L"Error enumerating: "+dir.wstring()); break; }
            }
        }
    }
    std::lock_guard lock(mutex_); auto& j=find(id);
    if(j.state==State::Paused || j.state==State::Cancelled || stop_) return;
    j.files=std::move(local.files); j.directories=std::move(local.directories); j.total=local.total;
    j.completed=0; j.errors=local.errors; j.scanned=true; j.state=State::Copying; save();
}
void Engine::process(u64 id) {
    bool scanned;
    { std::lock_guard lock(mutex_); scanned=find(id).scanned; }
    if(!scanned) {
        try { scan(id); }
        catch(const std::exception&) {
            bool missing=false;
            { std::lock_guard lock(mutex_); auto& j=find(id);
              missing=resolve(j.target).empty(); for(auto& s:j.sources) missing=missing || resolve(s).empty();
              if(missing && !terminal(j.state) && j.state!=State::Paused) { j.state=State::Waiting; j.speed=0; save(); } }
            if(missing) { log(id,L"Scanning stopped: waiting for device."); return; }
            throw;
        }
    }
    if(interrupted(id)) return;
    Job local; { std::lock_guard lock(mutex_); local=find(id); }
    auto target=resolve(local.target);
    if(target.empty()) { std::lock_guard lock(mutex_); find(id).state=State::Waiting; save(); return; }
    for(auto& d:local.directories) {
        if(interrupted(id)) return;
        try { rejectReparse(target/d); fs::create_directories(target/d); }
        catch(const std::exception&) { log(id,L"Could not create folder: "+d); std::lock_guard lock(mutex_); ++find(id).errors; }
    }
    for(size_t i=0;i<local.files.size();++i) {
        if(interrupted(id)) return;
        if(local.files[i].done || local.files[i].skipped) continue;
        try { copy(id,i); }
        catch(const std::exception& e) {
            if(interrupted(id)) return;
            bool missing=deviceError(e) || resolve(local.target).empty() || resolve(local.files[i].source).empty();
            if(missing) {
                log(id,L"Device disconnected; the partial file is preserved and the queue continues.");
                metric(local,local.files[i],0,0,L"disconnect");
                std::lock_guard lock(mutex_); auto& j=find(id); j.state=State::Waiting; j.speed=0; save(); return;
            }
            auto text=std::string(e.what()); log(id,L"Skipped due to an error: "+local.files[i].relative+L" · "+std::wstring(text.begin(),text.end()));
            metric(local,local.files[i],0,0,L"error");
            std::lock_guard lock(mutex_); auto& j=find(id); auto& f=j.files[i];
            j.completed-=f.offset; f.offset=0; f.skipped=true; j.skipped+=f.size; ++j.errors; save();
        }
        { std::lock_guard lock(mutex_); if(find(id).state==State::Waiting) return; }
    }
    if(interrupted(id)) return;
    std::wstring message;
    { std::lock_guard lock(mutex_); auto& j=find(id); j.state=j.errors?State::Errors:State::Done;
      j.speed=0; j.current.clear(); message=j.target.label+L": "+j.name+L" · "+bytes(j.completed);
      if(j.errors) message+=L" · "+std::to_wstring(j.errors)+L" errors (see log)";
      save(); }
    log(id,L"Batch completed: "+message);
    if(onFinished) onFinished(message);
}

void Engine::copy(u64 id,size_t index) {
    Job job; File file;
    { std::lock_guard lock(mutex_); auto& j=find(id); file=j.files[index];
      job.id=j.id; job.target=j.target; j.current=file.relative; j.detail=L"Verifying partial file / preparing copy"; }
    auto src=resolve(file.source), root=resolve(job.target);
    if(src.empty() || root.empty()) throw std::runtime_error("Device unavailable");
    auto dst=root/file.relative; rejectReparse(src); rejectReparse(dst);
    fs::create_directories(dst.parent_path());
    auto part=dst; part+=L".datacopy-"+std::to_wstring(id)+L".partial";
    rejectReparse(part);
    Handle input(CreateFileW(src.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!input) check(FALSE);
    if(length(input)!=file.size || modified(src)!=file.modified) throw std::runtime_error("The source changed after the batch was created");
    auto memory=static_cast<BYTE*>(VirtualAlloc(nullptr,blockSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!memory) throw std::bad_alloc();
    struct Free { BYTE* p; ~Free(){VirtualFree(p,0,MEM_RELEASE);} } cleanup{memory};
    bool existing=fs::exists(dst);
    Handle output(CreateFileW((existing?dst:part).c_str(),GENERIC_READ|(existing?0:GENERIC_WRITE),FILE_SHARE_READ,
        nullptr,existing?OPEN_EXISTING:OPEN_ALWAYS,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!output) check(FALSE);
    u64 offset=length(output);
    if(offset>file.size) throw std::runtime_error("The destination file is larger than the source; it will not be overwritten");
    // Compare all existing bytes, not just length: power loss may leave an unwritten or stale tail.
    u64 verified=0;
    while(verified<offset) {
        if(interrupted(id)) return;
        DWORD n=static_cast<DWORD>(std::min<u64>(blockSize,offset-verified)), a=0,b=0;
        check(ReadFile(input,memory,n,&a,nullptr)); check(ReadFile(output,memory+blockSize,n,&b,nullptr));
        if(a!=n || b!=n || memcmp(memory,memory+blockSize,n)!=0) {
            if(existing) throw std::runtime_error("A different file already exists; it will be preserved");
            offset=verified; seek(output,offset); check(SetEndOfFile(output)); seek(input,offset); break;
        }
        verified+=n;
    }
    if(existing && offset!=file.size) throw std::runtime_error("A different file already exists; it will be preserved");
    { std::lock_guard lock(mutex_); auto& j=find(id); auto& f=j.files[index];
      j.completed=j.completed-f.offset+offset; f.offset=offset; j.detail=existing?L"Existing file verified":L"Sequential copy"; }
    auto started=std::chrono::steady_clock::now(); auto tick=started; u64 tickBytes=offset, initial=offset, saved=offset;
    struct Account {
        std::function<void()> action;
        ~Account() { try { action(); } catch(...) {} }
    } account{[&] {
        if(offset>initial) metric(job,file,offset-initial,std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count(),L"copied");
    }};
    bool direct=false;
    if(!existing && file.size>=64ull*1024*1024 && offset%65536==0 && file.size-offset>=blockSize) {
        output.close(); input.close();
        input.value=CreateFileW(src.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        output.value=CreateFileW(part.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        FILE_STORAGE_INFO a{},b{};
        direct=input && output && GetFileInformationByHandleEx(input,FileStorageInfo,&a,sizeof(a)) &&
            GetFileInformationByHandleEx(output,FileStorageInfo,&b,sizeof(b));
        auto aligned=[](const FILE_STORAGE_INFO& s) {
            DWORD n=std::max(s.LogicalBytesPerSector,s.PhysicalBytesPerSectorForPerformance);
            return n && n<=65536 && 65536%n==0;
        };
        direct=direct && aligned(a) && aligned(b);
        if(!direct) {
            input.close(); output.close();
            input.value=CreateFileW(src.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
            output.value=CreateFileW(part.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
            log(id,L"Compatibility mode: the system does not support direct I/O for "+file.relative);
        }
        if(!input || !output) check(FALSE);
    }
    if(length(input)!=file.size || modified(src)!=file.modified) throw std::runtime_error("The source changed while opening the copy handles");
    seek(input,offset); seek(output,offset);
    { std::lock_guard lock(mutex_); find(id).detail=direct?L"Direct I/O · Windows cache bypassed":L"Compatibility mode · Windows cache"; }
    while(!existing && offset<file.size) {
        if(interrupted(id)) { check(FlushFileBuffers(output)); std::lock_guard lock(mutex_); save(); return; }
        DWORD n=static_cast<DWORD>(std::min<u64>(blockSize,file.size-offset));
        if(direct && n<blockSize) {
            check(FlushFileBuffers(output)); input.close(); output.close(); direct=false;
            input.value=CreateFileW(src.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
            output.value=CreateFileW(part.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            if(!input || !output) check(FALSE);
            if(length(input)!=file.size || modified(src)!=file.modified) throw std::runtime_error("The source changed while copying the final segment");
            seek(input,offset); seek(output,offset);
        }
        DWORD read=0,written=0; check(ReadFile(input,memory,n,&read,nullptr));
        if(read!=n) throw std::runtime_error("Incomplete source read");
        check(WriteFile(output,memory,n,&written,nullptr));
        if(written!=n) throw std::runtime_error("Incomplete destination write");
        offset+=written;
        auto now=std::chrono::steady_clock::now(); double elapsed=std::chrono::duration<double>(now-tick).count();
        bool night;
        { std::lock_guard lock(mutex_); auto& j=find(id); auto& f=j.files[index];
          j.completed+=offset-f.offset; f.offset=offset; night=night_;
          if(elapsed>=0.25) { j.speed=static_cast<double>(offset-tickBytes)/elapsed; tick=now; tickBytes=offset; } }
        if(offset-saved>=32ull*1024*1024) {
            check(FlushFileBuffers(output)); std::lock_guard lock(mutex_); save(); saved=offset;
        }
        if(night) {
            // Cap at 20 MB/s; use short waits so pause/cancel stay responsive.
            auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
            while(std::chrono::steady_clock::now()<until && !interrupted(id)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if(interrupted(id)) return;
    if(!existing) {
        check(FlushFileBuffers(output));
        FILETIME created{},accessed{},written{}; check(GetFileTime(input,&created,&accessed,&written));
        check(SetFileTime(output,&created,&accessed,&written));
        output.close(); input.close();
        // No REPLACE_EXISTING: a concurrent application cannot make us overwrite its file.
        check(MoveFileExW(part.c_str(),dst.c_str(),MOVEFILE_WRITE_THROUGH));
    }
    if(existing) metric(job,file,0,0,L"existing_verified");
    { std::lock_guard lock(mutex_); find(id).files[index].done=true; save(); }
}

void Engine::metric(const Job& job,const File& file,u64 transferred,double seconds,const std::wstring& event) {
    // Windows ships SQLite. Keep it dynamically linked so transfers work if unavailable.
    HMODULE dll=LoadLibraryW(L"winsqlite3.dll");
    if(!dll) { log(job.id,L"Telemetry unavailable: winsqlite3.dll is missing"); return; }
    using Open=int(__cdecl*)(const char*,void**);
    using Exec=int(__cdecl*)(void*,const char*,int(__cdecl*)(void*,int,char**,char**),void*,char**);
    using Close=int(__cdecl*)(void*);
    auto open=reinterpret_cast<Open>(GetProcAddress(dll,"sqlite3_open"));
    auto exec=reinterpret_cast<Exec>(GetProcAddress(dll,"sqlite3_exec"));
    auto close=reinterpret_cast<Close>(GetProcAddress(dll,"sqlite3_close"));
    void* db=nullptr; bool ok=false;
    if(open && exec && close && open(utf8((directory_/L"metrics.sqlite").wstring()).c_str(),&db)==0) {
        std::string sql="PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY, at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), job INTEGER, source_id TEXT, source_label TEXT, target_id TEXT, target_label TEXT, extension TEXT, bytes INTEGER, seconds REAL, event TEXT);";
        sql+="INSERT INTO events(job,source_id,source_label,target_id,target_label,extension,bytes,seconds,event) VALUES("+
            std::to_string(job.id)+","+quote(file.source.volume)+","+quote(file.source.label)+","+quote(job.target.volume)+","+
            quote(job.target.label)+","+quote(fs::path(file.relative).extension().wstring())+","+std::to_string(transferred)+","+
            std::to_string(seconds)+","+quote(event)+");";
        ok=exec(db,sql.c_str(),nullptr,nullptr,nullptr)==0;
    }
    if(db && close) close(db); FreeLibrary(dll);
    if(!ok) log(job.id,L"Could not record the local metric.");
}
}
