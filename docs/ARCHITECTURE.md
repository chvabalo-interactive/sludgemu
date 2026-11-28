# Architecture

## Process model

`sludgemu.exe` is a normal GUI-subsystem Windows program. It maps the title into its own
process; there is no separate launcher process and no hosted view. To the user the title looks
like any native game: it creates its own top-level window, which we turn into a resizable
desktop window with borderless fullscreen on F11 / Alt+Enter.

## Loader (`src/loader`)

The title is mapped by hand instead of through the native loader, because we need full control
over import resolution and over the address space:

1. Map headers and sections at the image's preferred base (relocate only if that fails), so
   addresses match the disassembler.
2. Register `.pdata` with `RtlAddFunctionTable` so SEH, C++ exceptions and stack walks work.
3. Set up implicit TLS (see below).
4. Bind imports. For each imported DLL name:
   1. **HLE module**: a console system DLL implemented by sludgemu (`xmem.dll`,
      `d3d12_xs.dll`, ...). Exports we haven't written resolve to logging stubs.
   2. **Title-local DLL**: a DLL shipped in the package. If it imports nothing console-specific
      (no HLE module, no other title DLL) it goes through the **native loader**, which handles
      its TLS, unwind data and loader bookkeeping; afterwards its IAT gets the same host
      overrides as hand-mapped images, so all title code sees the console environment.
      Otherwise it is mapped by hand, recursively.
      The VC++ runtime DLLs are the exception: they always come from the host.
   3. **Host DLL**: loaded from System32 / API sets. Individual functions can be replaced by
      *host overrides*, e.g. `LoadLibraryExW`, `GetProcAddress` and `GetModuleHandleW` (which
      must know about our mapped modules) or `CreateWindowExA` (window management).
5. Apply section protections, run DLL entry points, then the title's TLS callbacks and entry
   point.

Every unresolved import becomes a stub that logs `module!function called from <address>` with
the address in disassembler terms, and returns 0. Nothing fails silently.

### TLS

Compilers address an executable's thread-local variables with a hard-coded TLS index of 0, since
the native loader always gives the main executable slot 0. So the title exe has to own slot 0.
`sludgemu.exe`'s own implicit TLS is nothing but a 64 KiB reserve block, so its slot 0 is handed
to the title. An exe TLS callback copies the title's TLS template into that block on every thread
attach, which is what the native loader would do. `sludgemu.exe` therefore must not use implicit
TLS itself: no `thread_local`, no thread-safe statics (`/Zc:threadSafeInit-`), no incremental
linking. Its per-thread state uses `TlsAlloc` slots instead. A startup check enforces this.

The title's TLS callbacks and its hand-mapped DLLs' thread notifications run only on threads
the title created. Running title CRT code under the loader lock on our helper threads or on host
thread-pool workers deadlocks. Thread-locals with dynamic initializers are still set up lazily on
first access by MSVC's TLS guards.

Hand-mapped DLLs can't have implicit TLS, because they would need a slot of their own. Title DLLs
with TLS normally import nothing console-specific, and they are loaded natively.

## CPU topology

Titles size job systems from the processor topology. Console titles see 7 SMT cores (14 hardware
threads) in two core complexes, and get exactly that from `GetLogicalProcessorInformation`.
Affinity requests use console processor numbers, so they are recorded but not applied: the host
scheduler places threads better on hybrid CPUs than a fixed mapping would.

## Memory

"Physical" memory is split into 64 KiB page frames, backed by committed pagefile sections of
256 MiB ("chunks") that are created as frames get used, so the commit charge follows what the
title actually uses. Freed frames give their RAM back to the OS and are zeroed on reuse. Ranges
reserved for physical mapping are Win32 placeholders, and frames are mapped into them with
`MapViewOfFile3`, which gives real aliasing: the same frame at several addresses, as on console.
Mapping is 64 KiB granular (the console page size, which matches Windows' view granularity), but a
run of consecutive frames is mapped as one view. Each range keeps a list of regions that mirrors
the address space exactly (one placeholder or one view per entry), and every change updates both
together.

The frame numbers a title gets are IDs, bound to real memory only when first mapped. Titles
allocate frames in bulk and map slices of them wherever they need them, in no particular order;
binding late lets each 256 MiB slice of a range claim a free chunk and lay its pages out in address
order, so memory that is contiguous for the title is contiguous in RAM too. The graphics layer
relies on that (see below). Allocating IDs reserves the budget, so binding never fails.

Graphics allocations are placed inside the graphics address window reported by
`D3D12ConfigureVirtualMemoryX`, and the console D3D12 runtime's own allocations (descriptor heaps,
committed resources) go through `XMemAlloc` as on console, so the title's allocation hooks can
hand out memory it set aside for them.

## Graphics (`src/hle/d3d12`, `src/gpu`)

Console D3D12 objects are plain structs with a function table (see `console_object.h`); every
entry is ours and talks to a desktop D3D12 device from the Agility SDK.

Unified memory without copies: the GPU reads and writes title memory in place, through "windows":
desktop buffers placed on heaps opened over the host pages (`OpenExistingHeapFromAddress1`). A
window covers one contiguous block of title memory and is opened when the GPU first needs it, only
over the 64 MiB granules in use, since opening a heap pins its pages. Any title GPU address
translates to a desktop one through the window holding it; windows close when the title remaps or
frees the memory under them. Buffers are title memory seen through windows. Textures need desktop
layouts, so they get desktop textures in video memory; their title pages are never pinned.

Descriptor heaps live in title memory (handles are plain addresses there) and are mirrored by
desktop heaps slot for slot; every descriptor write and copy updates both.

Console textures sit in title memory in the console GPU's layout (RDNA2 swizzle modes, mip chains
smallest first, compression metadata planes), and titles size, place and fill them from that
layout, so it has to be exact. `gpu/console_layout` computes it with addrlib configured as the
console GPU; the two console models differ in memory pipes and packers. The same layouts back the
console texture layout library (`xg_xs.dll`) and resource sizes.

## File system

On console the package is mounted at `G:\`. File-system overrides and DirectStorage translate
those paths to the package directory (`core/vfs`); other paths reach the title from runtime APIs
and are host paths already.

## Game Runtime

Titles reach the Game Runtime (`xgameruntime.dll`) through statically linked thunks: each flat API
(`XUserAddAsync`, `XTaskQueueCreate`, ...) asks `QueryApiImpl(provider, iid)` for an interface and
calls one vtable method. The HLE module describes every vtable as provider + interface IDs + slot
table; unimplemented methods are named logging stubs returning `E_NOTIMPL`. Task queues and async
blocks are implemented natively on the Windows thread pool.

## Dependencies

Fetched at configure time, never vendored:
- the D3D12 Agility SDK (NuGet): the desktop D3D12 runtime we translate to, independent of the OS
  build. `D3D12Core.dll` goes to `D3D12\` next to `sludgemu.exe`, which exports
  `D3D12SDKVersion` / `D3D12SDKPath`. Its headers take precedence over the Windows SDK's.
- the DirectStorage redistributable (NuGet), copied next to `sludgemu.exe` and delay-loaded.
- libdeflate, for zlib decoding.
- AMD's address library (addrlib, from Mesa), for console texture layouts.

## Debugging conventions

- Log addresses in title images as `module+rva [preferred-base address]`, so they can be pasted
  into a disassembler directly.
- Diagnostics built in: `--dump-threads-after=N` for hangs, and `--watch-write=SPEC` for
  hardware write watchpoints on a location (`module.dll+0xRVA`, `0xADDR`, or `ldrentry:N` for
  native loader entries). Crash logs also check the native loader's module index for
  corruption. Title asserts (breakpoints) are logged first-chance with a backtrace, since titles
  install their own unhandled-exception filters. Diagnostic code paths never take the loader lock.
- Graphics problems are investigated with a frame capture (RenderDoc / Nsight Graphics), not by
  guessing.
