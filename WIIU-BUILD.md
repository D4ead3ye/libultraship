# Building for Wii U

## Toolchain

devkitPro with the **Wii U workload** — the default installer selection does
not include it:

```
pacman -S wiiu-dev wiiu-sdl2 wiiu-sdl2_net ppc-zlib ppc-bzip2 ppc-libogg ppc-libvorbis ppc-libpng ppc-libzip ppc-tinyxml2
```

## Why the build does not use devkitPro's cmake

devkitPro's cmake modules refuse to run outside msys2
(`dkp-initialize-path.cmake` errors on `CMAKE_HOST_WIN32`). But **msys2 strips
`TMP`, `TEMP` and `TMPDIR` from every child process**, including ones started by
`make` and `ninja`. With all three unset, `powerpc-eabi-gcc` falls back to
`GetTempPath()`, which returns `C:\WINDOWS`, and every compile and link dies
with:

```
Cannot create temporary file in C:\WINDOWS\: Permission denied
```

`-pipe` gets compiles through but not links, which still need a temp file.

So the build uses **native cmake and native ninja** against a copy of
devkitPro's cmake modules in `../dkp-cmake` with that one host check removed.
Nothing else in the modules is changed; they take their paths from `DEVKITPRO`.

## Configure and build

```bash
export DEVKITPRO=C:/devkitPro
export TMPDIR=C:/claude/bk-wiiu/tmp TMP=$TMPDIR TEMP=$TMPDIR
cmake -S . -B build-wiiu -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=C:/claude/bk-wiiu/dkp-cmake/WiiU.cmake \
  -DCMAKE_BUILD_TYPE=Release
ninja -C build-wiiu
```

Use native `cmake`/`ninja` on PATH, not the msys2 ones.

`CMAKE_SYSTEM_NAME` comes out as `CafeOS`, which is what the platform branches
in `CMakeLists.txt` and `src/CMakeLists.txt` key off.
