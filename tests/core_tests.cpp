#include "core.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <cstring>

using namespace dc;
namespace {
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
struct Fixture {
    fs::path root,source,target,state;
    Fixture() {
        root=fs::temp_directory_path()/(L"DataCopy-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        source=root/L"source";target=root/L"target";state=root/L"state";
        fs::create_directories(source);fs::create_directories(target);
    }
    ~Fixture() { std::error_code ec;fs::remove_all(root,ec); }
};
void write(const fs::path& path,size_t size,unsigned seed=17) {
    fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);
    char buffer[65536];size_t left=size;
    while(left) {
        auto n=std::min(left,sizeof(buffer));
        for(size_t i=0;i<n;++i) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;buffer[i]=static_cast<char>(seed); }
        out.write(buffer,n);left-=n;
    }
    require(static_cast<bool>(out),"Could not write test data");
}
bool equal(const fs::path& a,const fs::path& b) {
    if(!fs::exists(a)||!fs::exists(b)||fs::file_size(a)!=fs::file_size(b)) return false;
    std::ifstream x(a,std::ios::binary),y(b,std::ios::binary);char xx[65536],yy[65536];
    while(x) { x.read(xx,sizeof(xx));auto n=x.gcount();y.read(yy,n);if(y.gcount()!=n||memcmp(xx,yy,n)) return false; }
    return true;
}
Job job(Engine& engine,u64 id) {
    for(auto& j:engine.snapshot().jobs) if(j.id==id) return j;
    throw std::runtime_error("Job missing");
}
Job wait(Engine& engine,u64 id,std::function<bool(const Job&)> predicate) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(std::chrono::steady_clock::now()<deadline) {
        auto j=job(engine,id);if(predicate(j)) return j;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Timed out waiting for job state");
}
Job finished(Engine& engine,u64 id) { return wait(engine,id,[](const Job& j){return j.state==State::Done||j.state==State::Errors;}); }
void basic() {
    Fixture f;write(f.source/L"movie_Δ"/L"video.bin",2*1024*1024+113);write(f.source/L"movie_Δ"/L"zero.txt",0);
    fs::create_directories(f.source/L"movie_Δ"/L"empty_Ω");
    Engine e(f.state);auto id=e.enqueue({f.source/L"movie_Δ"},f.target);auto j=finished(e,id);
    require(j.state==State::Done,"Basic copy did not complete");
    require(equal(f.source/L"movie_Δ"/L"video.bin",f.target/L"movie_Δ"/L"video.bin"),"Copied bytes differ");
    require(fs::is_directory(f.target/L"movie_Δ"/L"empty_Ω"),"Empty directory lost");
    require(fs::file_size(f.target/L"movie_Δ"/L"zero.txt")==0,"Empty file lost");
    require(j.completed==j.total,"Completed byte count wrong");
}
void collisions() {
    Fixture f;write(f.source/L"same.bin",512,1);write(f.target/L"same.bin",512,2);write(f.source/L"next.bin",1000);
    write(f.root/L"expected.bin",512,2);
    Engine e(f.state);auto id=e.enqueue({f.source/L"same.bin",f.source/L"next.bin"},f.target);auto j=finished(e,id);
    require(j.state==State::Errors && j.errors==1,"Collision was not reported");
    require(equal(f.target/L"same.bin",f.root/L"expected.bin"),"Existing destination overwritten");
    require(equal(f.source/L"next.bin",f.target/L"next.bin"),"Error aborted remaining files");
}
void duplicateIdentical() {
    Fixture f;write(f.source/L"same.bin",1000);fs::copy_file(f.source/L"same.bin",f.target/L"same.bin");
    Engine e(f.state);auto j=finished(e,e.enqueue({f.source/L"same.bin"},f.target));
    require(j.state==State::Done && j.completed==1000,"Identical existing file not verified");
}
void recoverCorruptPartial() {
    Fixture f;auto source=f.source/L"large.bin";write(source,12*1024*1024+13);u64 id=0;
    {
        Engine e(f.state);e.setNight(true);id=e.enqueue({source},f.target);
        wait(e,id,[](const Job& j){return j.completed>=2*1024*1024;});e.pause(id);e.shutdown();
        require(job(e,id).state==State::Paused,"Pause was not preserved");
    }
    auto partial=f.target/(L"large.bin.datacopy-"+std::to_wstring(id)+L".partial");
    require(fs::exists(partial)&&fs::file_size(partial)>0,"Partial missing");
    { std::fstream out(partial,std::ios::binary|std::ios::in|std::ios::out);out.put('\0');out.put('\0'); }
    {
        Engine e(f.state);require(e.snapshot().recovery,"Session not offered for recovery");
        auto before=job(e,id).completed;std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(job(e,id).completed==before && job(e,id).state==State::Interrupted,"Recovery auto-started without consent");
        e.setNight(false);e.resumeAll();require(finished(e,id).state==State::Done,"Recovery failed");
        require(equal(source,f.target/L"large.bin"),"Corrupt partial was trusted");
        require(!fs::exists(partial),"Completed partial not renamed");
    }
}
void directTail() {
    Fixture f;write(f.source/L"direct.bin",65*1024*1024+137);
    Engine e(f.state);auto j=finished(e,e.enqueue({f.source/L"direct.bin"},f.target));
    require(j.state==State::Done,"Direct/compatible large copy failed");
    require(equal(f.source/L"direct.bin",f.target/L"direct.bin"),"Non-aligned file tail corrupted");
}
void changedSource() {
    Fixture f;write(f.source/L"change.bin",6*1024*1024);u64 id;
    {
        Engine e(f.state);e.setNight(true);id=e.enqueue({f.source/L"change.bin"},f.target);
        wait(e,id,[](const Job& j){return j.completed>=1024*1024;});e.pause(id);e.shutdown();
    }
    write(f.source/L"change.bin",7*1024*1024,38);
    Engine e(f.state);e.resumeAll();auto j=finished(e,id);
    require(j.state==State::Errors,"Changed source was silently resumed");
    require(!fs::exists(f.target/L"change.bin"),"Changed source produced a final file");
}
void cancelAndSingleWorker() {
    Fixture f;write(f.source/L"first.bin",5*1024*1024);write(f.source/L"second.bin",4*1024*1024);
    Engine e(f.state);e.setNight(true);auto a=e.enqueue({f.source/L"first.bin"},f.target);auto b=e.enqueue({f.source/L"second.bin"},f.target);
    wait(e,a,[](const Job& j){return j.completed>=1024*1024;});
    auto s=e.snapshot();unsigned active=0;for(auto& j:s.jobs) if(j.state==State::Copying||j.state==State::Scanning) ++active;
    require(active==1 && job(e,b).state==State::Queued,"More than one active job");
    e.cancel(a);e.setNight(false);require(finished(e,b).state==State::Done,"Queue did not continue after cancellation");
    require(job(e,a).state==State::Cancelled,"Cancellation not retained");
    require(fs::exists(f.source/L"first.bin"),"Cancellation removed source");
    require(!fs::exists(f.target/L"first.bin"),"Cancelled transfer finalized");
}
void rejectNestedTarget() {
    Fixture f;fs::create_directories(f.source/L"inside");write(f.source/L"file.bin",10);
    Engine e(f.state);bool rejected=false;
    try { e.enqueue({f.source},f.source/L"inside"); } catch(const std::exception&) { rejected=true; }
    require(rejected,"Recursive self-copy accepted");
}
void corruptCheckpoint() {
    Fixture f;fs::create_directories(f.state);{std::ofstream out(f.state/L"session.dat");out<<"broken";}
    bool rejected=false;try { Engine e(f.state); } catch(const std::exception&) { rejected=true; }
    require(rejected,"Corrupt checkpoint silently accepted");
    require(fs::file_size(f.state/L"session.dat")==6,"Corrupt checkpoint overwritten");
}
}
int main() {
    unsigned failed=0;
    const std::pair<const char*,void(*)()> tests[]={
        {"nested files, Unicode and empty directories",basic},{"collision preserves file and continues",collisions},
        {"identical destination verified",duplicateIdentical},{"explicit recovery and corrupt partial",recoverCorruptPartial},
        {"large file and unaligned tail",directTail},{"changed source rejected",changedSource},
        {"single worker and cancellation",cancelAndSingleWorker},{"recursive target rejected",rejectNestedTarget},
        {"corrupt checkpoint preserved",corruptCheckpoint}
    };
    for(auto [name,test]:tests) {
        try {test();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e) {++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
