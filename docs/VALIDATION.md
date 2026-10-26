# Validation and limitations

## Validation status

The implementation includes CMake configuration and an integration suite. The
C++ executable and integration suite still require compilation, execution, and
visual review with a Windows toolchain. Included tests are not reported as
passing until they have actually run.

Checks completed independently of the C++ compiler include PowerShell syntax,
manifest XML, and whitespace validation. The SQLite schema and seven analytics
queries were executed against an in-memory database using Windows-provided
`winsqlite3.dll` and a sample copy event. These checks do not validate the C++
transfer engine.

The command launcher has also been checked for missing-tool detection and rejection of unsupported build configurations. It returns exit code 2 without downloading dependencies or changing system policies.

## Integration suite

The build launcher configures CMake, builds the project, and runs CTest. Tests
use temporary directories and do not operate on the user's USB devices.

Coverage includes:

1. Nested copying with Unicode names, an empty file, and an empty folder.
2. Preserving a different existing destination while copying subsequent files.
3. Verifying an identical existing destination through content comparison.
4. Pause, shutdown, explicit recovery, and repair of a corrupt partial file.
5. A file larger than 64 MiB with an unaligned final segment.
6. Rejecting a source modified before recovery.
7. One active worker, cancellation, and continuation of the queue.
8. Rejecting a destination folder nested inside the source.
9. Preserving an unreadable checkpoint without overwriting it.
10. Rejecting new batches when the session cannot be saved.
11. Pausing the active queue when recovery storage fails.
12. Continuing a batch when one source disappears while queued.
13. Rejecting invalid booleans in an otherwise valid session header.
14. Normalizing volume roots without dot segments in extended-length paths.

## Pending checks with physical devices

- Review the interface against TeraCopy 3.17 at 100%, 125%, 150%, and 200% display scale.
- Drop individual files, multiple folders, and long names from File Explorer.
- Disconnect a destination during copying and verify another connected batch continues.
- Reconnect the same USB device with another drive letter and verify automatic resumption.
- Connect two devices with identical labels and confirm their tasks remain distinct.
- Test PnP ejection with a flash drive, a multipartition USB disk, and an Orico enclosure.
- Keep a file open in another application and confirm Windows can veto removal.
- Check native notifications with Windows notification preferences and Do not disturb.
- Measure direct and buffered I/O on NTFS and exFAT. Files larger than 4 GiB on FAT32 must report an error and allow the queue to continue.
- Simulate abrupt shutdowns using disposable test data and verify complete output with an external hash.

## Current limitations

- Copy only; source files are never moved or deleted.
- Mounted local volumes only. UNC, MTP, and redirected cloud destinations are unsupported.
- Symbolic links, junctions, and reparse points are not followed.
- The main data stream and file timestamps are copied. ACLs, alternate data
  streams, compression, hard links, and directory metadata are not replicated
  as they would be by a backup utility.
- Different existing destination files are preserved and logged as errors.
- Recovery verifies the entire existing prefix. This can take time for large
  files; length alone is insufficient to trust a partial file after power loss.
- Confirmed writes pass through `FlushFileBuffers` before a checkpoint is saved.
  The storage hardware and firmware determine the final physical durability guarantee.
- Partial filenames include the batch ID. Canceling preserves them; a new batch
  does not automatically adopt an old partial. Remove unused partials manually
  only after recovery is no longer needed.
- The local session format is versioned and is not an interchange format. The
  entire checkpoint is rewritten; workloads with millions of entries need
  persistence profiling and optimization before intensive use.
- No fixed transfer speed is guaranteed. Direct I/O uses aligned memory and
  checks storage geometry; the interface indicates buffered compatibility mode
  when direct I/O cannot be used.
- Night mode limits writes to approximately 20 MB/s. It does not stop motors,
  control RPM, or guarantee silence.
- Scheduled ejection lasts for the open session. Removal targets the exact disk
  device node, not the entire USB hub. Windows may reject removal depending on hardware.
- Incident counts are not SMART diagnostics and cannot identify damaged sectors.
- Read/write metrics reflect observed copy throughput, constrained by both
  endpoints. They are not isolated device benchmarks.
- Telemetry counts bytes per attempt, including paused attempts. Recopying
  counts additional I/O activity. Abrupt shutdown may lose metrics for the current
  attempt even when its partial file remains recoverable.
- Time saved is not estimated without a comparable parallel-copy benchmark.
- Physical Orico bay numbers cannot be identified without controller information;
  display uses the volume label, media type, and volume GUID.

## Technical references

- [TeraCopy screen layout](https://support.codesector.com/en/articles/8789947-main-screen).
- [Microsoft: unbuffered I/O and alignment](https://learn.microsoft.com/windows/win32/fileio/file-buffering).
- [Microsoft: FlushFileBuffers](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers).
- [Microsoft: PnP ejection](https://learn.microsoft.com/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_request_device_ejectw).
