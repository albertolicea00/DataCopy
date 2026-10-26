#include "core.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <memory>
#include <algorithm>
#include <deque>
#include <sstream>
#include <iomanip>
#include <future>
#include <stdexcept>
#include <chrono>

namespace {
using namespace dc;
constexpr UINT finishedMessage=WM_APP+1;
constexpr int toolbarHeight=56, dockHeight=100, sideWidth=214;
enum : int { AddFiles=101,AddFolder,ChooseTarget,Pause,Resume,Cancel,Up,Down,Night,Eject,ResumeAll,OpenData,Refresh, QueueTab=301, FilesTab, LogTab, StatsTab, OptionsTab };
HWND windowHandle{}, list{}, sidebar{}, targetText{}, headline{}, detailText{}, statusText{}, progress{}, tabs{}, textView{};
HFONT font{}, titleFont{}, smallFont{};
HFONT paintFont{}, paintSmallFont{};
UINT dpi=96;
std::unique_ptr<Engine> engine;
std::vector<Volume> devices;
Snapshot view;
std::vector<RECT> cardRects;
fs::path selectedTarget;
int selectedDevice=-1, selectedTab=0;
std::wstring selectedVolumeId;
std::future<std::vector<Volume>> inventory;
u64 selectedJob=0;
std::deque<double> speeds;
std::vector<std::pair<HWND,int>> buttons;
NOTIFYICONDATAW tray{};
HANDLE instanceMutex{};
std::vector<std::wstring> ejectRequests;
bool rebuilding=false;
COLORREF green=RGB(66,146,73), muted=RGB(104,111,112), border=RGB(219,224,220);
int px(int value) { return MulDiv(value,static_cast<int>(dpi),96); }
void moveControl(HWND h,int x,int y,int width,int height,BOOL repaint=TRUE) { MoveWindow(h,px(x),px(y),px(width),px(height),repaint); }
RECT logicalClient() {
    RECT r{}; GetClientRect(windowHandle,&r);r.right=MulDiv(r.right,96,dpi);r.bottom=MulDiv(r.bottom,96,dpi);return r;
}
void updateFonts() {
    auto oldFont=font,oldSmall=smallFont,oldTitle=titleFont;
    auto make=[](int size,int weight) { return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI"); };
    font=make(14,FW_NORMAL);smallFont=make(12,FW_NORMAL);titleFont=make(23,FW_SEMIBOLD);
    EnumChildWindows(windowHandle,+[](HWND h,LPARAM)->BOOL {SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return TRUE;},0);
    if(headline) SendMessageW(headline,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
    if(sidebar) SendMessageW(sidebar,LB_SETITEMHEIGHT,0,px(66));
    if(oldFont) DeleteObject(oldFont);if(oldSmall) DeleteObject(oldSmall);if(oldTitle) DeleteObject(oldTitle);
}

std::wstring fromException(const std::exception& e) {
    auto s=std::string(e.what()); int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
    std::wstring result(n,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,result.data(),n); if(!result.empty()) result.pop_back(); return result;
}
void warning(const std::wstring& s) { MessageBoxW(windowHandle,s.c_str(),L"DataCopy",MB_OK|MB_ICONINFORMATION); }
void rectFill(HDC dc,RECT r,COLORREF color) { HBRUSH b=CreateSolidBrush(color); FillRect(dc,&r,b); DeleteObject(b); }
void line(HDC dc,int x,int y,int xx,int yy,COLORREF color) {
    HPEN p=CreatePen(PS_SOLID,1,color); auto old=SelectObject(dc,p); MoveToEx(dc,x,y,nullptr); LineTo(dc,xx,yy); SelectObject(dc,old); DeleteObject(p);
}
void drawText(HDC dc,std::wstring text,RECT r,COLORREF color,HFONT f,UINT flags=DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS) {
    auto old=SelectObject(dc,f); SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT); DrawTextW(dc,text.c_str(),-1,&r,flags); SelectObject(dc,old);
}
HWND control(const wchar_t* type,const wchar_t* text,DWORD style,int id=0,HWND parent=windowHandle) {
    HWND h=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return h;
}
HWND button(const wchar_t* label,int id) {
    auto h=control(L"BUTTON",label,BS_PUSHBUTTON|WS_TABSTOP,id); buttons.push_back({h,id}); return h;
}
Job* selected() {
    for(auto& j:view.jobs) if(j.id==selectedJob) return &j;
    return nullptr;
}
bool isPending(State s) { return s!=State::Done && s!=State::Errors && s!=State::Cancelled; }

std::vector<fs::path> pick(bool folder,bool multiple) {
    IFileOpenDialog* dialog=nullptr; std::vector<fs::path> paths;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return paths;
    DWORD flags=0; dialog->GetOptions(&flags);
    dialog->SetOptions(flags|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|(folder?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST)|(multiple?FOS_ALLOWMULTISELECT:0));
    dialog->SetTitle(folder?L"Select folder":L"Add files to batch");
    if(SUCCEEDED(dialog->Show(windowHandle))) {
        IShellItemArray* items=nullptr;
        if(SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count=0; items->GetCount(&count);
            for(DWORD i=0;i<count;++i) {
                IShellItem* item=nullptr;
                if(SUCCEEDED(items->GetItemAt(i,&item))) {
                    PWSTR path=nullptr;
                    if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) { paths.emplace_back(path); CoTaskMemFree(path); }
                    item->Release();
                }
            } items->Release();
        }
    } dialog->Release(); return paths;
}
bool chooseDestination() {
    auto chosen=pick(true,false); if(chosen.empty()) return false;
    auto bound=bind(chosen.front()); selectedTarget=resolve(bound); selectedDevice=-1; selectedVolumeId.clear();
    SetWindowTextW(targetText,(L"Destination: "+bound.label+L" · "+bound.relative).c_str()); InvalidateRect(windowHandle,nullptr,FALSE); return true;
}
void add(const std::vector<fs::path>& sources) {
    if(sources.empty()) return;
    try {
        if(selectedTarget.empty() && !chooseDestination()) return;
        selectedJob=engine->enqueue(sources,selectedTarget);
    }
    catch(const std::exception& e) { warning(fromException(e)); }
}
void refreshDevices() {
    if(!inventory.valid()) inventory=std::async(std::launch::async,[]{return volumes();});
}
void receiveDevices() {
    if(!inventory.valid() || inventory.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
    try { devices=inventory.get(); cardRects.clear(); }
    catch(const std::exception&) { return; }
    selectedDevice=-1;
    for(size_t i=0;i<devices.size();++i) if(devices[i].id==selectedVolumeId) selectedDevice=static_cast<int>(i);
    engine->devicesChanged(); InvalidateRect(windowHandle,nullptr,FALSE);
}
bool jobUsesDisk(const Job& j,const Volume& v) {
    auto same=[&](const std::wstring& id) {
        if(id==v.id) return true;
        if(v.diskNumber==MAXDWORD) return false;
        for(auto& device:devices) if(device.id==id && device.diskNumber==v.diskNumber) return true;
        return false;
    };
    if(same(j.target.volume)) return true;
    for(auto& source:j.sources) if(same(source.volume)) return true;
    return false;
}
std::wstring speedText(double speed) { return speed>0?bytes(static_cast<u64>(speed))+L"/s":L"—"; }
void listCell(int row,int column,const std::wstring& s) { ListView_SetItemText(list,row,column,const_cast<wchar_t*>(s.c_str())); }
void renderList() {
    rebuilding=true; SendMessageW(list,WM_SETREDRAW,FALSE,0); ListView_DeleteAllItems(list);
    if(selectedTab==0) {
        for(size_t i=0;i<view.jobs.size();++i) {
            auto& j=view.jobs[i]; LVITEMW item{}; item.mask=LVIF_TEXT|LVIF_PARAM;
            item.iItem=static_cast<int>(i); item.pszText=const_cast<wchar_t*>(j.name.c_str()); item.lParam=static_cast<LPARAM>(j.id);
            int row=ListView_InsertItem(list,&item); listCell(row,1,j.target.label); listCell(row,2,speedText(j.speed));
            std::wstring percent=j.total?std::to_wstring(std::min<u64>(100,j.completed*100/j.total))+L" %":L"—";
            listCell(row,3,percent); listCell(row,4,stateText(j.state));
            listCell(row,5,bytes(j.completed)+L" / "+bytes(j.total));
            if(j.id==selectedJob) ListView_SetItemState(list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        }
    } else if(selectedTab==1 && selectedJob) {
        auto files=engine->fileDetails(selectedJob,2001);
        size_t count=std::min<size_t>(files.size(),2000);
        for(size_t i=0;i<count;++i) {
            auto& f=files[i]; LVITEMW item{}; item.mask=LVIF_TEXT; item.iItem=static_cast<int>(i); item.pszText=const_cast<wchar_t*>(f.relative.c_str());
            int row=ListView_InsertItem(list,&item); listCell(row,1,f.source.label); listCell(row,2,L"—");
            listCell(row,3,f.size?std::to_wstring(f.offset*100/f.size)+L" %":(f.done?L"100 %":L"0 %"));
            listCell(row,4,f.skipped?L"Skipped due to an error":f.done?L"Completed":L"Pending"); listCell(row,5,bytes(f.size));
        }
        if(files.size()>count) SetWindowTextW(detailText,L"Showing the first 2,000 files. The engine processes the entire batch.");
    }
    SendMessageW(list,WM_SETREDRAW,TRUE,0); InvalidateRect(list,nullptr,TRUE); rebuilding=false;
}
void renderSidebar() {
    rebuilding=true;
    int top=static_cast<int>(SendMessageW(sidebar,LB_GETTOPINDEX,0,0));
    SendMessageW(sidebar,WM_SETREDRAW,FALSE,0); SendMessageW(sidebar,LB_RESETCONTENT,0,0);
    for(auto& j:view.jobs) {
        auto label=j.name+L"\n"+stateText(j.state);
        int i=static_cast<int>(SendMessageW(sidebar,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str())));
        SendMessageW(sidebar,LB_SETITEMDATA,i,static_cast<LPARAM>(j.id));
        if(j.id==selectedJob) SendMessageW(sidebar,LB_SETCURSEL,i,0);
    }
    SendMessageW(sidebar,LB_SETTOPINDEX,top,0); SendMessageW(sidebar,WM_SETREDRAW,TRUE,0); InvalidateRect(sidebar,nullptr,TRUE); rebuilding=false;
}
void updateTextView() {
    std::wstring text;
    if(selectedTab==2) { for(auto& l:view.log) text+=l+L"\r\n"; if(text.empty()) text=L"The session log will appear here. Full history is saved in events.log."; }
    if(selectedTab==3) text=analytics();
    if(selectedTab==4) text=
        L"TRANSFERS\r\n\r\n"
        L"Global concurrency: one batch and one file at a time.\r\n"
        L"Automatic direct I/O: files of at least 64 MiB, when the device supports the required alignment.\r\n"
        L"The progress panel shows the actual I/O mode in use.\r\n\r\n"
        L"RECOVERY\r\n\r\n"
        L"When reopening an interrupted session, no copy starts without your approval.\r\n"
        L"Partial files are stored beside the destination with a .partial extension.\r\n"
        L"Their contents are compared with the source before resuming.\r\n"
        L"Different existing files are preserved and reported as errors.\r\n"
        L"Canceling preserves the partial file and never deletes the source.\r\n\r\n"
        L"NIGHT MODE\r\n\r\n"
        L"Limits writes to approximately 20 MB/s; it does not control RPM or guarantee silence.\r\n\r\n"
        L"DEVICES\r\n\r\n"
        L"The label is the display name; the volume GUID preserves identity when the drive letter changes.\r\n"
        L"Reconnections are checked automatically every two seconds.\r\n"
        L"Eject waits until the device has no pending batches.\r\n"
        L"Windows may reject ejection if another application keeps files open.\r\n\r\n"
        L"PRIVACY\r\n\r\n"
        L"The session, log, and SQLite metrics are stored in LocalAppData.\r\n"
        L"There are no servers or telemetry uploads. Open data lets you inspect these files.";
    SetWindowTextW(textView,text.c_str());
}
void refresh(bool full=false) {
    view=engine->snapshot();
    if(!selectedJob && !view.jobs.empty()) selectedJob=view.jobs.front().id;
    auto j=selected();
    SetWindowTextW(headline,j?j->name.c_str():L"Ready to copy");
    std::wstring detail=j?stateText(j->state)+L"  ·  "+j->target.label+L"\r\n"+j->current+L"\r\n"+j->detail:
        L"Drop files onto a device or add a batch.\r\nOnly one transfer is active at any time.";
    SetWindowTextW(detailText,detail.c_str());
    int pct=j && j->total?static_cast<int>(std::min<u64>(100,j->completed*100/j->total)):0;
    SendMessageW(progress,PBM_SETPOS,pct,0);
    size_t pending=0; for(auto& job:view.jobs) if(isPending(job.state)) ++pending;
    std::wstring status=std::to_wstring(pending)+L" pending batches   ·   1 concurrent transfer   ·   "+(view.night?L"Night: ~20 MB/s":L"No speed limit");
    if(j) status+=L"   ·   "+speedText(j->speed);
    if(!view.storageError.empty()) status=L"ATTENTION: "+view.storageError;
    SetWindowTextW(statusText,status.c_str());
    for(auto [h,id]:buttons) {
        if(id==Pause || id==Resume || id==Cancel || id==Up || id==Down) EnableWindow(h,j && isPending(j->state));
        if(id==Night) SetWindowTextW(h,view.night?L"☾ Night: on":L"☾ Night: off");
    }
    speeds.push_back(j?j->speed:0); if(speeds.size()>100) speeds.pop_front();
    renderSidebar(); if(selectedTab<=1) renderList();
    if(selectedTab==2 || full) updateTextView();
    auto client=logicalClient();
    RECT graph{px(sideWidth+20),px(toolbarHeight+dockHeight+156),px(client.right-20),px(toolbarHeight+dockHeight+236)};
    InvalidateRect(windowHandle,&graph,FALSE);
}

void layout() {
    auto r=logicalClient(); int w=r.right,h=r.bottom;
    int x=14;
    for(auto [control,id]:buttons) {
        int bw=0,by=12;
        switch(id) {
        case AddFiles:bw=125;break; case AddFolder:bw=130;break; case ChooseTarget:bw=148;break;
        case Night:bw=128;x=w-282;break; case OpenData:bw=126;break;
        default:continue;
        }
        moveControl(control,x,by,bw,31); x+=bw+8;
    }
    moveControl(sidebar,10,toolbarHeight+dockHeight+42,sideWidth-20,h-toolbarHeight-dockHeight-84);
    int left=sideWidth+22,right=w-left-22,top=toolbarHeight+dockHeight+14;
    moveControl(headline,left,top,right,32);
    moveControl(detailText,left,top+39,right,66);
    moveControl(progress,left,top+116,right,12);
    moveControl(targetText,left,top+135,right,22);
    moveControl(tabs,left,top+220,right,29);
    int contentY=top+258, contentH=std::max(40,h-contentY-83);
    moveControl(list,left,contentY,right,contentH); moveControl(textView,left,contentY,right,contentH);
    x=left;
    for(auto [control,id]:buttons) {
        int bw=0;
        switch(id) {
        case Pause:bw=78;break;case Resume:bw=92;break;case Cancel:bw=85;break;case Up:bw=52;break;case Down:bw=52;break;
        case ResumeAll:bw=130;break;case Eject:bw=112;break;default:continue;
        }
        moveControl(control,x,h-71,bw,30); x+=bw+6;
    }
    moveControl(statusText,12,h-28,w-24,22);
    int widths[]={30,18,12,9,18,19};
    for(int i=0;i<6;++i) ListView_SetColumnWidth(list,i,px(std::max(66,right*widths[i]/106)));
    InvalidateRect(windowHandle,nullptr,TRUE);
}
void paint() {
    PAINTSTRUCT ps{}; HDC dc=BeginPaint(windowHandle,&ps); RECT physical{}; GetClientRect(windowHandle,&physical); auto r=logicalClient();
    // Double buffer the custom surface; child controls own their native painting.
    HDC mem=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,physical.right),std::max(1L,physical.bottom)); auto old=SelectObject(mem,bitmap);
    SetMapMode(mem,MM_ANISOTROPIC);SetWindowExtEx(mem,96,96,nullptr);SetViewportExtEx(mem,dpi,dpi,nullptr);
    rectFill(mem,r,RGB(250,251,250)); rectFill(mem,{0,0,r.right,toolbarHeight},RGB(243,245,243));
    line(mem,0,toolbarHeight-1,r.right,toolbarHeight-1,border);
    int top=toolbarHeight+10, gap=10, cardWidth=std::max(156,std::min(215,static_cast<int>((r.right-28)/4-10)));
    cardRects.clear();
    int maxCards=std::max(1,static_cast<int>((r.right-24)/(cardWidth+gap)));
    for(size_t i=0;i<devices.size() && static_cast<int>(i)<maxCards;++i) {
        RECT card{14+static_cast<int>(i)*(cardWidth+gap),top,14+static_cast<int>(i)*(cardWidth+gap)+cardWidth,top+78};
        cardRects.push_back(card); bool active=static_cast<int>(i)==selectedDevice;
        rectFill(mem,card,active?RGB(232,244,232):RGB(255,255,255));
        HBRUSH b=CreateSolidBrush(active?green:border); FrameRect(mem,&card,b); DeleteObject(b);
        auto& v=devices[i];
        drawText(mem,v.label,{card.left+12,card.top+6,card.right-10,card.top+29},RGB(39,49,41),paintFont);
        drawText(mem,v.kind+L" · "+bytes(v.free)+L" free",{card.left+12,card.top+30,card.right-10,card.top+50},muted,paintSmallFont);
        RECT bar{card.left+12,card.bottom-17,card.right-12,card.bottom-12}; rectFill(mem,bar,RGB(229,234,229));
        if(v.total) { bar.right=bar.left+static_cast<int>((cardWidth-24)*static_cast<double>(v.total-v.free)/v.total); rectFill(mem,bar,green); }
    }
    if(devices.empty()) drawText(mem,L"No mounted volumes found. Connect a USB device or select a destination folder.",{20,top,r.right-20,top+78},muted,paintFont);
    if(devices.size()>cardRects.size()) drawText(mem,L"More destinations: use Choose destination",{r.right-240,top+79,r.right-14,top+96},muted,paintSmallFont);
    int y=toolbarHeight+dockHeight;
    line(mem,0,y,r.right,y,border); rectFill(mem,{0,y,sideWidth,r.bottom-34},RGB(240,242,240));
    line(mem,sideWidth,y,sideWidth,r.bottom-34,border);
    drawText(mem,L"TRANSFERS",{17,y+10,sideWidth-10,y+35},muted,paintSmallFont);
    int gx=sideWidth+22,gy=y+174,gw=r.right-gx-22,gh=56;
    rectFill(mem,{gx,gy,gx+gw,gy+gh},RGB(239,246,239));
    for(int k=1;k<4;++k) line(mem,gx,gy+k*gh/4,gx+gw,gy+k*gh/4,RGB(223,235,223));
    double maxSpeed=1000000;
    for(auto s:speeds) maxSpeed=std::max(maxSpeed,s*1.2);
    HPEN pen=CreatePen(PS_SOLID,2,green); auto prev=SelectObject(mem,pen);
    int i=0; for(double s:speeds) {
        int xx=gx+static_cast<int>(i*gw/99.0), yy=gy+gh-2-static_cast<int>(s/maxSpeed*(gh-5));
        if(i==0) MoveToEx(mem,xx,yy,nullptr); else LineTo(mem,xx,yy); ++i;
    }
    SelectObject(mem,prev); DeleteObject(pen);
    line(mem,0,r.bottom-34,r.right,r.bottom-34,border);
    SetMapMode(mem,MM_TEXT);SetWindowOrgEx(mem,0,0,nullptr);SetViewportOrgEx(mem,0,0,nullptr);
    BitBlt(dc,0,0,physical.right,physical.bottom,mem,0,0,SRCCOPY); SelectObject(mem,old); DeleteObject(bitmap); DeleteDC(mem); EndPaint(windowHandle,&ps);
}

void ejectSelected() {
    if(selectedDevice<0 || selectedDevice>=static_cast<int>(devices.size())) { warning(L"Select a device card first."); return; }
    auto v=devices[selectedDevice]; auto s=engine->snapshot();
    // Also protect reads from this device, not only writes to it.
    bool pending=false;
    for(auto& j:s.jobs) if(isPending(j.state)) {
        pending=pending || jobUsesDisk(j,v);
    }
    if(pending) {
        if(std::find(ejectRequests.begin(),ejectRequests.end(),v.id)==ejectRequests.end()) ejectRequests.push_back(v.id);
        warning(L"Ejection scheduled. It will be attempted when this device has no pending reads or writes.\nKeep DataCopy open until then."); return;
    }
    std::wstring error; if(!safeEject(v,error)) warning(L"Windows could not eject the volume: "+error);
    else { selectedTarget.clear(); selectedVolumeId.clear(); SetWindowTextW(targetText,L"Destination: select a device"); }
    refreshDevices();
}
void processEjections() {
    for(auto it=ejectRequests.begin();it!=ejectRequests.end();) {
        auto device=std::find_if(devices.begin(),devices.end(),[&](const Volume& v){return v.id==*it;});
        bool pending=false;
        if(device!=devices.end()) for(auto& j:view.jobs) if(isPending(j.state)) pending=pending || jobUsesDisk(j,*device);
        if(pending) { ++it; continue; }
        if(device!=devices.end()) {
            std::wstring error;
            if(!safeEject(*device,error)) warning(L"Could not complete the scheduled ejection: "+error);
        }
        it=ejectRequests.erase(it);
    }
}
void command(int id) {
    try {
        switch(id) {
        case AddFiles:add(pick(false,true));break;
        case AddFolder:add(pick(true,true));break;
        case ChooseTarget:chooseDestination();break;
        case Pause:if(selectedJob) engine->pause(selectedJob);break;
        case Resume:if(selectedJob) engine->resume(selectedJob);break;
        case Cancel:if(selectedJob) engine->cancel(selectedJob);break;
        case Up:if(selectedJob) engine->move(selectedJob,-1);break;
        case Down:if(selectedJob) engine->move(selectedJob,1);break;
        case ResumeAll:engine->resumeAll();break;
        case Night:engine->setNight(!view.night);break;
        case OpenData:ShellExecuteW(windowHandle,L"open",dataDirectory().c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
        case Eject:ejectSelected();break;
        }
        refresh(true);
    } catch(const std::exception& e) { warning(fromException(e)); }
}
void recover() {
    auto snapshot=engine->snapshot(); if(!snapshot.recovery) return;
    unsigned count=0; u64 remaining=0; std::wstring targets;
    for(auto& j:snapshot.jobs) if(isPending(j.state)) {
        ++count; remaining+=j.total-std::min(j.total,j.completed);
        if(targets.find(j.target.label)==std::wstring::npos) { if(!targets.empty()) targets+=L", "; targets+=j.target.label; }
    }
    auto message=L"The previous session was interrupted with "+std::to_wstring(count)+L" pending batches.\n\n"+
        bytes(remaining)+L" remaining according to the latest checkpoint.\nDestinations: "+targets+
        L"\n\nUnscanned batches will be measured when resumed. Each partial file will be verified before copying continues.\n\nResume all now?";
    if(MessageBoxW(windowHandle,message.c_str(),L"Recover previous session",MB_YESNO|MB_ICONINFORMATION|MB_DEFBUTTON2)==IDYES) engine->resumeAll();
}
void createControls() {
    dpi=GetDpiForWindow(windowHandle);updateFonts();
    paintFont=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    paintSmallFont=CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    button(L"+ Files",AddFiles); button(L"+ Folders",AddFolder); button(L"Choose destination",ChooseTarget);
    button(L"☾ Night: off",Night); button(L"Open data",OpenData);
    button(L"Pause",Pause); button(L"Resume",Resume); button(L"Cancel",Cancel);
    button(L"↑",Up); button(L"↓",Down); button(L"Resume all",ResumeAll); button(L"Eject USB",Eject);
    sidebar=control(L"LISTBOX",L"",LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOTIFY|WS_VSCROLL|WS_TABSTOP,201);
    SendMessageW(sidebar,LB_SETITEMHEIGHT,0,px(66));
    headline=control(L"STATIC",L"",SS_LEFT); SendMessageW(headline,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
    detailText=control(L"STATIC",L"",SS_LEFT); targetText=control(L"STATIC",L"Destination: select a device",SS_LEFT);
    progress=control(PROGRESS_CLASSW,L"",PBS_SMOOTH); SendMessageW(progress,PBM_SETRANGE32,0,100); SendMessageW(progress,PBM_SETBARCOLOR,0,green);
    statusText=control(L"STATIC",L"",SS_LEFT);
    tabs=control(WC_TABCONTROLW,L"",WS_TABSTOP,202);
    const wchar_t* names[]={L"Queue",L"Files",L"Log",L"Statistics",L"Options"};
    for(int i=0;i<5;++i) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(names[i]); TabCtrl_InsertItem(tabs,i,&item); }
    list=control(WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP,203);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    const wchar_t* columns[]={L"Name",L"Device",L"Speed",L"Progress",L"Status",L"Transferred"};
    for(int i=0;i<6;++i) { LVCOLUMNW c{}; c.mask=LVCF_TEXT|LVCF_WIDTH; c.pszText=const_cast<wchar_t*>(columns[i]); c.cx=130; ListView_InsertColumn(list,i,&c); }
    textView=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,204);
    SendMessageW(textView,EM_SETLIMITTEXT,4*1024*1024,0); ShowWindow(textView,SW_HIDE);
    DragAcceptFiles(windowHandle,TRUE);
    tray.cbSize=sizeof(tray); tray.hWnd=windowHandle; tray.uID=1; tray.uFlags=NIF_ICON|NIF_TIP;
    tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION); wcscpy_s(tray.szTip,L"DataCopy · Transfer queue"); Shell_NotifyIconW(NIM_ADD,&tray);
}
LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_CREATE:windowHandle=hwnd;createControls();return 0;
    case WM_SIZE:layout();return 0;
    case WM_DPICHANGED: {
        dpi=HIWORD(w);updateFonts();auto r=reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info=reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize={px(1060),px(730)}; return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:paint();return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT: {
        auto dc=reinterpret_cast<HDC>(w); SetTextColor(dc,RGB(48,57,49)); SetBkColor(dc,RGB(250,251,250));
        static HBRUSH b=CreateSolidBrush(RGB(250,251,250)); return reinterpret_cast<LRESULT>(b);
    }
    case WM_DRAWITEM: {
        auto item=reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if(item->CtlID==201 && item->itemID!=static_cast<UINT>(-1)) {
            bool active=item->itemState&ODS_SELECTED;
            rectFill(item->hDC,item->rcItem,active?RGB(222,237,222):RGB(240,242,240));
            auto id=static_cast<u64>(item->itemData);
            for(auto& job:view.jobs) if(job.id==id) {
                auto r=item->rcItem; r.left+=px(12);r.right-=px(8);r.bottom=r.top+px(31);
                drawText(item->hDC,job.name,r,RGB(35,48,36),font);
                r.top+=px(30);r.bottom+=px(24);drawText(item->hDC,stateText(job.state)+L" · "+job.target.label,r,muted,smallFont); break;
            } return TRUE;
        } break;
    }
    case WM_COMMAND:
        if(LOWORD(w)==201 && HIWORD(w)==LBN_SELCHANGE && !rebuilding) {
            int i=static_cast<int>(SendMessageW(sidebar,LB_GETCURSEL,0,0));
            if(i>=0) { selectedJob=static_cast<u64>(SendMessageW(sidebar,LB_GETITEMDATA,i,0)); speeds.clear(); refresh(); }
        } else if(HIWORD(w)==BN_CLICKED) command(LOWORD(w));
        return 0;
    case WM_NOTIFY: {
        auto info=reinterpret_cast<NMHDR*>(l);
        if(info->hwndFrom==tabs && info->code==TCN_SELCHANGE) {
            selectedTab=TabCtrl_GetCurSel(tabs); ShowWindow(list,selectedTab<=1?SW_SHOW:SW_HIDE); ShowWindow(textView,selectedTab>=2?SW_SHOW:SW_HIDE); refresh(true);
        }
        if(info->hwndFrom==list && info->code==LVN_ITEMCHANGED && !rebuilding && selectedTab==0) {
            auto change=reinterpret_cast<NMLISTVIEW*>(l);
            if((change->uNewState&LVIS_SELECTED) && change->iItem>=0) {
                LVITEMW item{};item.mask=LVIF_PARAM;item.iItem=change->iItem;ListView_GetItem(list,&item);
                selectedJob=static_cast<u64>(item.lParam);speeds.clear();refresh();
            }
        } return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{MulDiv(GET_X_LPARAM(l),96,dpi),MulDiv(GET_Y_LPARAM(l),96,dpi)};
        for(size_t i=0;i<cardRects.size() && i<devices.size();++i) if(PtInRect(&cardRects[i],p)) {
            selectedDevice=static_cast<int>(i);selectedVolumeId=devices[i].id;selectedTarget=devices[i].id;
            SetWindowTextW(targetText,(L"Destination: "+devices[i].label+L" · volume root").c_str()); InvalidateRect(hwnd,nullptr,FALSE); break;
        } return 0;
    }
    case WM_DROPFILES: {
        auto drop=reinterpret_cast<HDROP>(w); POINT point{}; DragQueryPoint(drop,&point);point.x=MulDiv(point.x,96,dpi);point.y=MulDiv(point.y,96,dpi);
        std::vector<fs::path> paths;
        UINT count=DragQueryFileW(drop,0xFFFFFFFF,nullptr,0);
        for(UINT i=0;i<count;++i) {
            UINT n=DragQueryFileW(drop,i,nullptr,0); std::wstring path(n+1,0);
            DragQueryFileW(drop,i,path.data(),n+1);path.resize(n);paths.emplace_back(path);
        }
        DragFinish(drop);
        for(size_t i=0;i<cardRects.size() && i<devices.size();++i) if(PtInRect(&cardRects[i],point)) {
            selectedDevice=static_cast<int>(i); selectedVolumeId=devices[i].id; selectedTarget=devices[i].id;
            SetWindowTextW(targetText,(L"Destination: "+devices[i].label+L" · volume root").c_str()); break;
        }
        add(paths);refresh();return 0;
    }
    case WM_DEVICECHANGE:refreshDevices();return 0;
    case WM_TIMER: {
        if(w==1) { receiveDevices(); refresh(); }
        if(w==2) { processEjections(); refreshDevices(); }
        if(w==3) { KillTimer(hwnd,3); recover(); refresh(); }
        return 0;
    }
    case finishedMessage: {
        std::unique_ptr<std::wstring> messageText(reinterpret_cast<std::wstring*>(l));
        tray.uFlags=NIF_INFO;tray.dwInfoFlags=NIIF_INFO;wcscpy_s(tray.szInfoTitle,L"DataCopy · Batch completed");
        wcsncpy_s(tray.szInfo,messageText->c_str(),_TRUNCATE);Shell_NotifyIconW(NIM_MODIFY,&tray);return 0;
    }
    case WM_CLOSE: {
        KillTimer(hwnd,1);KillTimer(hwnd,2);KillTimer(hwnd,3);engine->shutdown();
        MSG pending{};
        while(PeekMessageW(&pending,hwnd,finishedMessage,finishedMessage,PM_REMOVE))
            delete reinterpret_cast<std::wstring*>(pending.lParam);
        DestroyWindow(hwnd);return 0;
    }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE,&tray);DeleteObject(font);DeleteObject(titleFont);DeleteObject(smallFont);DeleteObject(paintFont);DeleteObject(paintSmallFont);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    dpi=GetDpiForSystem();
    instanceMutex=CreateMutexW(nullptr,FALSE,L"Local\\USBFlowSuite.DataCopy.SingleInstance");
    if(!instanceMutex || GetLastError()==ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,L"DataCopy is already running. Only one copy engine is allowed per Windows session.",L"DataCopy",MB_OK|MB_ICONINFORMATION);
        if(instanceMutex) CloseHandle(instanceMutex); CoUninitialize(); return 0;
    }
    INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_LISTVIEW_CLASSES|ICC_PROGRESS_CLASS|ICC_TAB_CLASSES};InitCommonControlsEx(&cc);
    try {
        engine=std::make_unique<Engine>();
        WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=procedure;wc.hInstance=instance;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wc.lpszClassName=L"DataCopy.Native";
        RegisterClassExW(&wc);
        HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"DataCopy — Transfers",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
            CW_USEDEFAULT,CW_USEDEFAULT,px(1180),px(810),nullptr,nullptr,instance,nullptr);
        if(!hwnd) throw std::runtime_error("Could not create the main window");
        engine->onFinished=[hwnd](const std::wstring& s) {
            auto message=new std::wstring(s);
            if(!PostMessageW(hwnd,finishedMessage,0,reinterpret_cast<LPARAM>(message))) delete message;
        };
        refreshDevices();refresh(true);ShowWindow(hwnd,show);UpdateWindow(hwnd);
        SetTimer(hwnd,1,750,nullptr);SetTimer(hwnd,2,4000,nullptr);SetTimer(hwnd,3,100,nullptr);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0) {
            if(!IsDialogMessageW(hwnd,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}
        }
        engine.reset();
    } catch(const std::exception& e) {
        if(engine) engine->shutdown();
        MessageBoxW(nullptr,fromException(e).c_str(),L"DataCopy · Error",MB_OK|MB_ICONERROR);
        engine.reset();CloseHandle(instanceMutex);CoUninitialize();return 1;
    }
    CloseHandle(instanceMutex);CoUninitialize();return 0;
}
