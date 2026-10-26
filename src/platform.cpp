#include "core.hpp"
#include <shlobj.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <sstream>
#include <iomanip>

namespace dc {
std::wstring errorText(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"Windows error " + std::to_wstring(code);
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r')) result.pop_back();
    return result;
}
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring bytes(u64 n) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(n); int unit = 0;
    while (value >= 1000 && unit < 4) { value /= 1000; ++unit; }
    std::wostringstream out; out << std::fixed << std::setprecision(unit ? 1 : 0) << value << L" " << units[unit];
    return out.str();
}
fs::path dataDirectory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) throw std::runtime_error("LocalAppData unavailable");
    fs::path result = fs::path(path) / L"USBFlowSuite" / L"DataCopy"; CoTaskMemFree(path); return result;
}
std::vector<Volume> volumes() {
    std::vector<Volume> result;
    wchar_t name[MAX_PATH]; HANDLE search = FindFirstVolumeW(name, MAX_PATH);
    if (search == INVALID_HANDLE_VALUE) return result;
    do {
        wchar_t mounts[32768]{}; DWORD needed = 0;
        if (!GetVolumePathNamesForVolumeNameW(name, mounts, 32768, &needed) || !mounts[0]) continue;
        UINT type = GetDriveTypeW(mounts);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t label[MAX_PATH]{};
        if (!GetVolumeInformationW(name, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) continue;
        Volume v; v.id = name; v.root = mounts; v.label = label[0] ? label : L"Unlabeled";
        v.removable = type == DRIVE_REMOVABLE; v.kind = v.removable ? L"USB / Flash" : L"Disk";
        ULARGE_INTEGER free{}, total{};
        if (GetDiskFreeSpaceExW(name, &free, &total, nullptr)) { v.free = free.QuadPart; v.total = total.QuadPart; }
        std::wstring device = v.id; device.pop_back();
        HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            STORAGE_DEVICE_NUMBER disk{}; DWORD diskReturned=0;
            if(DeviceIoControl(h,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&disk,sizeof(disk),&diskReturned,nullptr)) v.diskNumber=disk.DeviceNumber;
            STORAGE_PROPERTY_QUERY query{}; query.PropertyId = StorageDeviceProperty; query.QueryType = PropertyStandardQuery;
            BYTE buffer[4096]{}; DWORD returned = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &returned, nullptr)) {
                auto d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
                if (d->BusType == BusTypeUsb) { v.removable = true; v.kind = L"USB"; }
            }
            query.PropertyId = StorageDeviceSeekPenaltyProperty;
            DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &returned, nullptr))
                v.kind += seek.IncursSeekPenalty ? L" · HDD" : L" · Flash / SSD";
            CloseHandle(h);
        }
        result.push_back(v);
    } while (FindNextVolumeW(search, name, MAX_PATH));
    FindVolumeClose(search); return result;
}
Location bind(const fs::path& path) {
    auto full = fs::absolute(path).lexically_normal();
    wchar_t root[32768]{}, id[MAX_PATH]{}, label[MAX_PATH]{};
    if (!GetVolumePathNameW(full.c_str(), root, 32768) || !GetVolumeNameForVolumeMountPointW(root, id, MAX_PATH))
        throw std::runtime_error("Select a path on a mounted local volume");
    GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
    return {id, label[0] ? label : L"Unlabeled", full.lexically_relative(fs::path(root)).wstring()};
}
fs::path resolve(const Location& location) {
    // Volume GUID disambiguates duplicate labels and survives drive-letter changes.
    wchar_t mounts[32768]{}; DWORD count = 0;
    if (!GetVolumePathNamesForVolumeNameW(location.volume.c_str(), mounts, 32768, &count) || !mounts[0]) return {};
    if (GetFileAttributesW(location.volume.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return (fs::path(location.volume) / location.relative).lexically_normal();
}
bool safeEject(const Volume& v, std::wstring& error) {
    if (!v.removable) { error = L"Ejection is only available for USB or removable devices."; return false; }
    if(v.diskNumber==MAXDWORD) { error=L"Could not identify the physical disk. Use Safely Remove Hardware in Windows."; return false; }
    // Request removal of the exact disk devnode, never its USB hub or enclosure.
    static const GUID diskInterface={0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
    HDEVINFO set=SetupDiGetClassDevsW(&diskInterface,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE) { error=errorText(); return false; }
    bool ok=false; error=L"Could not find the disk device node. Use Safely Remove Hardware in Windows.";
    for(DWORD i=0;;++i) {
        SP_DEVICE_INTERFACE_DATA interfaceData{}; interfaceData.cbSize=sizeof(interfaceData);
        if(!SetupDiEnumDeviceInterfaces(set,nullptr,&diskInterface,i,&interfaceData)) break;
        DWORD needed=0; SetupDiGetDeviceInterfaceDetailW(set,&interfaceData,nullptr,0,&needed,nullptr);
        if(!needed) continue;
        std::vector<BYTE> buffer(needed);
        auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data()); detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA info{}; info.cbSize=sizeof(info);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&interfaceData,detail,needed,nullptr,&info)) continue;
        HANDLE h=CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(h==INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER disk{}; DWORD returned=0;
        bool match=DeviceIoControl(h,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&disk,sizeof(disk),&returned,nullptr) && disk.DeviceNumber==v.diskNumber;
        CloseHandle(h); if(!match) continue;
        PNP_VETO_TYPE veto{}; wchar_t name[MAX_PATH]{};
        CONFIGRET result=CM_Request_Device_EjectW(info.DevInst,&veto,name,MAX_PATH,0);
        ok=result==CR_SUCCESS;
        if(!ok) error=L"Windows rejected device removal (code "+std::to_wstring(result)+L", veto "+std::to_wstring(veto)+L"). "+name;
        break;
    }
    SetupDiDestroyDeviceInfoList(set); return ok;
}
std::wstring stateText(State s) {
    switch (s) {
    case State::Queued: return L"Queued"; case State::Scanning: return L"Scanning";
    case State::Copying: return L"Copying"; case State::Paused: return L"Paused";
    case State::Waiting: return L"Waiting for device"; case State::Interrupted: return L"Interrupted";
    case State::Done: return L"Completed"; case State::Errors: return L"Completed with errors";
    case State::Cancelled: return L"Canceled";
    } return {};
}
std::wstring analytics(const fs::path& directory) {
    HMODULE dll=LoadLibraryW(L"winsqlite3.dll");
    if(!dll) return L"winsqlite3.dll was not found. Transfers and recovery remain available.";
    using Open=int(__cdecl*)(const char*,void**,int,const char*);
    using Exec=int(__cdecl*)(void*,const char*,int(__cdecl*)(void*,int,char**,char**),void*,char**);
    using Close=int(__cdecl*)(void*);
    auto open=reinterpret_cast<Open>(GetProcAddress(dll,"sqlite3_open_v2"));
    auto exec=reinterpret_cast<Exec>(GetProcAddress(dll,"sqlite3_exec"));
    auto close=reinterpret_cast<Close>(GetProcAddress(dll,"sqlite3_close"));
    void* db=nullptr; std::wstring result;
    if(open && exec && close && open(utf8((directory/L"metrics.sqlite").wstring()).c_str(),&db,1,nullptr)==0) {
        auto callback=+[](void* context,int count,char** values,char**)->int {
            auto& out=*static_cast<std::wstring*>(context);
            for(int i=0;i<count;++i) {
                std::string s=values[i]?values[i]:"—";
                int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
                std::wstring wide(n,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,wide.data(),n);
                if(!wide.empty()) wide.pop_back(); out+=wide; if(i+1<count) out+=L"    |    ";
            } out+=L"\r\n"; return 0;
        };
        auto query=[&](const wchar_t* title,const char* sql) {
            result+=title; result+=L"\r\n";
            if(exec(db,sql,callback,&result,nullptr)!=0) result+=L"No metrics have been recorded yet.\r\n";
            result+=L"\r\n";
        };
        query(L"WRITES BY DEVICE · observed copy MB/s",
            "SELECT target_label, round(sum(bytes)/1000000.0,1)||' MB', round(sum(bytes)/max(sum(seconds),0.001)/1000000.0,1)||' MB/s' FROM events WHERE event='copied' GROUP BY target_id ORDER BY sum(bytes) DESC;");
        query(L"READS BY DEVICE · also limited by the destination",
            "SELECT source_label, round(sum(bytes)/max(sum(seconds),0.001)/1000000.0,1)||' MB/s' FROM events WHERE event='copied' GROUP BY source_id;");
        query(L"TODAY / LAST 7 DAYS / CURRENT MONTH · GB written",
            "SELECT round(sum(CASE WHEN date(at,'localtime')=date('now','localtime') THEN bytes ELSE 0 END)/1000000000.0,3), round(sum(CASE WHEN datetime(at)>=datetime('now','-7 days') THEN bytes ELSE 0 END)/1000000000.0,3), round(sum(CASE WHEN strftime('%Y-%m',at,'localtime')=strftime('%Y-%m','now','localtime') THEN bytes ELSE 0 END)/1000000000.0,3) FROM events WHERE event='copied';");
        query(L"DAILY HISTORY · GB written",
            "SELECT date(at,'localtime'),round(sum(bytes)/1000000000.0,3)||' GB' FROM events WHERE event='copied' GROUP BY date(at,'localtime') ORDER BY at DESC LIMIT 31;");
        query(L"FILE TYPE · GB written",
            "SELECT CASE WHEN lower(extension) IN('.mp4','.mkv','.avi','.mov','.m4v','.webm') THEN 'Video' WHEN lower(extension) IN('.mp3','.flac','.wav','.aac','.ogg') THEN 'Audio' WHEN lower(extension) IN('.iso','.img') THEN 'ISO / Image' WHEN lower(extension) IN('.pdf','.doc','.docx','.txt','.xlsx','.csv','.pptx') THEN 'Documents' ELSE 'Other' END AS category,round(sum(bytes)/1000000000.0,3)||' GB' FROM events WHERE event='copied' GROUP BY category;");
        query(L"FRICTION · review the log before attributing issues to a disk",
            "SELECT source_label||' → '||target_label,event,count(*) FROM events WHERE event IN('error','disconnect') GROUP BY source_id,target_id,event ORDER BY count(*) DESC;");
        query(L"ALERTS · three or more incidents on a copy route",
            "SELECT source_label||' → '||target_label,'Check the cable, permissions, free space, and log: '||count(*)||' incidents' FROM events WHERE event IN('error','disconnect') GROUP BY source_id,target_id HAVING count(*)>=3;");
        result+=L"Speeds reflect copy activity, not an isolated benchmark.\r\n"
                L"Time saved: not calculated; a comparable parallel-copy benchmark is required.\r\n"
                L"Temperature, RPM, and bad sectors are not inferred from these events.\r\n";
    } else result=L"No metrics yet. They will appear after the first copy completes.\r\nAll data stays on this computer.";
    if(db && close) close(db); FreeLibrary(dll); return result;
}
}
