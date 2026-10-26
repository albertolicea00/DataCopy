# DataCopy

A native Windows 10/11 copy utility built with **C++20 and Win32**, without a web
interface or background service. The interface follows TeraCopy 3.17's layout:
a toolbar, task sidebar, progress panel, and detail tabs, with an additional
volume dock for dropping files from File Explorer.

**Status:** initial implementation. The C++ executable and integration suite
still require compilation and execution with a Windows toolchain. See
[required downloads](docs/REQUIRED_DOWNLOADS.md) and
[validation and limitations](docs/VALIDATION.md).

## Implemented features

- A globally sequential queue: one worker and one active file at a time.
- File and folder batches, pause, resume, cancel, and queue priority.
- Volume cards showing labels, media types, and available space.
- Drop files onto a card or choose a destination folder.
- Volume GUID identity with label-based display, independent of drive letters.
- Wait for disconnected devices while continuing other batches.
- Explicit recovery when reopening an interrupted session.
- Owned partial files, existing-prefix comparison, and continuation from the last
  verified block; different existing destination files are preserved.
- Automatic direct I/O for large files when alignment requirements are supported,
  with separate handling of the final unaligned segment.
- Per-file error handling and a local event log.
- Local SQLite metrics for transfer volume, file categories, device performance,
  and incidents, without uploading data.
- Night mode with an approximate 20 MB/s write limit.
- Native notifications and safe-removal requests, either immediately or after
  the disk's pending batches finish.

## Build

Install Visual Studio's Desktop development with C++ workload, Windows SDK, and
CMake. From the repository root:

```powershell
.\build.ps1
.\build\Release\DataCopy.exe
```

The build launcher also runs the integration suite through CTest.
Run `build.ps1` only where PowerShell execution policy permits it, or use the manual CMake commands in the setup guide.

## Usage

1. Select a volume card, or use **Choose destination** to select a subfolder.
2. Drop files onto the card, or select **Files** / **Folders**.
3. Use **Pause**, **Resume**, **Cancel**, **↑**, and **↓** for the selected batch.
4. Review **Files**, **Log**, and **Statistics** for results.
5. **Eject USB** waits for the disk's pending reads and writes.

After an interrupted session, the recovery summary appears before any copying
starts. New batches added while recovery is pending remain paused.

## Local data

Stored under `%LOCALAPPDATA%\USBFlowSuite\DataCopy`:

| File | Contents |
| --- | --- |
| `session.dat` | Queue and recovery checkpoints; atomically replaced |
| `events.log` | Cumulative UTF-8 event log |
| `metrics.sqlite` | SQLite metrics using Windows-provided `winsqlite3.dll` |

Partial files remain beside their destinations as
`name.datacopy-ID.partial` until completion. Canceling does not delete them.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `src/core.*` | Queue, manifests, persistence, and block copying |
| `src/platform.cpp` | Volumes, SQLite, and PnP ejection |
| `src/main.cpp` | Win32 window and native controls |
| `tests/core_tests.cpp` | Integrity, recovery, and queue integration tests |
| `docs/REQUIRED_DOWNLOADS.md` | Build dependencies and setup instructions |
| `docs/VALIDATION.md` | Validation coverage, pending checks, and limitations |

No fixed transfer rate or time-saved estimate is promised without measurements.
The interface follows TeraCopy's functional layout; exact visual equivalence
still requires review of the running executable.
