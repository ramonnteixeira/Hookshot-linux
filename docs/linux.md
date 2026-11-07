# Linux

Hookshot builds and runs as a native Linux program on this branch. It is the same static recompilation as on Windows:
the game's code translated to C, the console's system reimplemented in `host/`, the Switch 2 graphics replayed on
Vulkan in `gpu/vk/`. Only the layer that talks to the operating system changes.

**Status: compiles and links, not yet run with the game.** Everything below has been checked on Linux without the game
(see *Checks*); a run of the actual game on Linux is the next step, and reports are welcome.

## What you need

- 64-bit Linux on x86-64 with a CPU from 2013 or later (x86-64-v3: Haswell, Zen 1).
- A GPU and driver with Vulkan 1.3 and mesh shaders (NVIDIA's driver, or Mesa's RADV/ANV recent enough).
- To build: Python 3 with `lz4`, `numpy`, `capstone`, `unicorn`, `zstandard` and Pillow; clang 17 or later and lld; the development files of
  FFmpeg (`libavcodec`, `libswscale`, `libswresample`, `libavutil`, through `pkg-config`), Vulkan, SPIR-V and the
  Vulkan Memory Allocator.
- To run: SDL2 (window, sound, controllers), the Vulkan loader, and FFmpeg built with libdav1d for the AV1 movies.

On Debian or Ubuntu 24.04:

```
sudo apt install clang lld pkg-config \
    python3-lz4 python3-numpy python3-capstone python3-unicorn python3-zstandard python3-pil \
    libavcodec-dev libswscale-dev libswresample-dev libavutil-dev \
    libvulkan-dev spirv-headers libsdl2-2.0-0 libdav1d7
```

(Where the distribution has no `python3-unicorn`, `pip install --user --break-system-packages unicorn`.)

If `/tmp` is a small RAM disk (`df -h /tmp`), point the compiler's temporary files at the disk before building:
`mkdir -p ~/tmp && export TMPDIR=~/tmp`.

If your distribution has no package for the Vulkan Memory Allocator, clone it and point the build at it:
`git clone https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator` and `export RX_VMA=<that folder>`.

## Building

The same as on Windows, from the command line (the Builder app is Windows only):

```
python3 -I nx2/make_game.py --nup /path/your-game.nup --out ~/Games/Hookshot --language en-US
```

or with `--romfs` and `--exefs` folders. The build checks what it needs from the system first and says what is
missing. Its working files go to `~/.local/share/HookshotBuilder` (`--work` to change it). Then run
`~/Games/Hookshot/hookshot`.

The game folder holds `hookshot` and `hookshot_vk.so` beside the game data; SDL2, FFmpeg and Vulkan are the system's.

## How the port is made

- **`host/win32_compat.h`, `host/plat_posix.c`**: the part of the Win32 API the host layer uses, implemented on POSIX,
  so the host code stays one source for both systems. Locks and condition variables (pthread rwlocks, futexes),
  threads, events and timers, files (backslashes in paths become slashes), directory listing, modules (`x.dll` opens
  `x.so`), virtual memory (`mmap`, `mprotect`, `madvise`), fibers (`ucontext`), vectored exception handlers (signals,
  with an x86-64 `CONTEXT`) and `__try` (`w32_guard`, `sigsetjmp`). On Windows the header only includes
  `<windows.h>`.
- **Guest memory**: the 512 GB window is mapped whole with `MAP_NORESERVE`, so pages appear on first touch and the
  Windows commit-on-touch handler is not needed.
- **Window, input, sound**: `host/display_sdl.c` (SDL2, X11 or Wayland) in place of `display.c`; the Vulkan surface comes
  from SDL. `nn_audio.c` queues to an SDL audio device in place of `waveOut`. Keyboard state comes from SDL; pads come
  through SDL as before (`pad_sdl.c`), there is no XInput. SDL2 is opened at run time, as `SDL2.dll` is on Windows.
- **Movies**: the system's FFmpeg; AV1 through libdav1d in place of Media Foundation.
- **Renderer**: `gpu/vk` builds as `hookshot_vk.so`, which uses the executable's functions (`-rdynamic`). The Insert
  menu draws its text with `gpu/vk/gdi_lite.h` and the public-domain `third_party/font8x8`. There is no WebGPU renderer
  on Linux (`host/rx_no_webgpu.c` stands in for it).
- **`nx2/build.py`**: ELF outputs, lld's symbol ordering for the hot functions, the system's FFmpeg and headers.

Not ported yet: the samplers and profiler behind `HOS_PROFILE`, `HOS_SAMPLE`, `HOS_THREAD_TIMES` and `HOS_WATCH` (use
`perf`), `HOS_PGO`'s dump files (compiler-rt's `LLVM_PROFILE_FILE` instead), and `--dll` (the lifted game for Eden).

## Checks

These need no game and run in CI (`.github/workflows/linux-port.yml`):

```
clang -O1 -D_GNU_SOURCE -rdynamic -Ihost tests/win32_compat_test.c host/plat_posix.c -lpthread -ldl -o /tmp/w32t && /tmp/w32t
python3 -I scripts/linux_check.py           # host/*.c
python3 -I scripts/linux_check.py --vk      # the Vulkan back end (gpu/vk, Eden's recompiler, sirit)
```
