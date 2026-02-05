# GoW Mods

God of War (PS2) specific modifications for PCSX2.

## Files

- **GowModWindow** — Qt window for logging game events (WAD loading, file I/O, commands) and displaying memory trace statistics.
- **GowHooks** — EE execution hooks that intercept game functions: WAD loader commands, `ProcessWadFile`, and memory tracing for `goServer_LoadClient`.
- **GowWadInjector** — Intercepts `sysFile` open/read/close/isReadDone calls to inject custom WAD files from a user-specified directory on the host filesystem.

## How it works

When the GoW event log window is opened, `gowInitHooks()` registers execution hooks at known game function addresses. These hooks log events to the window and, for WAD injection, redirect file I/O to host files. On window close, `gowShutdownHooks()` removes all hooks.

### WAD injection

Set a custom WAD directory via the Inject menu. When the game opens a resource, the injector checks if a matching `.WAD` file exists in that directory. If found, it serves reads from the host file instead of the disc image.
