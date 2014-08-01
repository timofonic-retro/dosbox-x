DOSBox-X with

- OpenGL Voodoo restored
- OpenGLide support restored (disabled by default)
- Dynrec core restored/backported
- x86 FPU core restored/backported
- Printer emulation restored
- SDL_Sound use for WAVE/AIFF/MP3 in CUE files restored
- Direct3D output re-enabled
- Tandy DAC emulation backported
- backported fixes from mainline
- various minor fixes

Tested casually on Linux x86 and x86_64. Tested very casually on Win32,
using MinGW cross-compiler and WINE.

Sample configurations (host openSUSE 13.1 on AMD64):

- x86 (x86 dynamic core, x86 FPU):

  CC="gcc -m32" CXX="g++ -m32" ./configure --host=i686-suse-linux

- AMD64 (recompiling core):

  ./configure

- Win32:

  FREETYPE_CONFIG=/usr/i686-w64-mingw32/sys-root/mingw/bin/freetype-config \
  CC="i686-w64-mingw32-gcc" CXX="i686-w64-mingw32-g++" \
  ./configure --host=mingw32 \
	--includedir=/usr/i686-w64-mingw32/sys-root/mingw/include \
	--with-sdl-prefix=/usr/i686-w64-mingw32/sys-root/mingw
