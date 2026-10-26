# Required downloads

DataCopy targets Windows 10/11 x64 and uses C++20 and native Win32 APIs. Running
the application does not require Node.js, Python, .NET, Qt, WebView2, or a
separately downloaded third-party library.

## Build dependencies

Install Visual Studio Community or Visual Studio Build Tools with:

- **Desktop development with C++** (`Microsoft.VisualStudio.Workload.NativeDesktop`).
- MSVC x64/x86 tools with C++20 support.
- Windows 10 or Windows 11 SDK.
- C++ CMake tools for Windows, including CTest.

CMake 3.24 or later is required; use a version that supports the installed
Visual Studio release. The CMake version bundled with that release is preferred.

Use **Visual Studio Installer → Modify** to add missing components to an existing
installation. No machine-specific installation path is required by the project.

Reference repositories or installers are not runtime or build dependencies.

## Build and test

Run the build launcher from the repository root:

```powershell
.\build.ps1
```

Alternatively, open **Developer PowerShell for Visual Studio** at the repository
root and run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\DataCopy.exe
```

Run `build.ps1` only where PowerShell execution policy permits it, or use the manual CMake commands below.

## SQLite

Telemetry uses `winsqlite3.dll`, provided by Windows 10/11. If it is unavailable,
the engine keeps the event log and reports that the metric could not be recorded.
Transfer recovery does not depend on SQLite.

## Installation reference

[Install C++ support in Visual Studio](https://learn.microsoft.com/cpp/build/vscpp-step-0-installation).
