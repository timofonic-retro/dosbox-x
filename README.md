DOSBox-X with

- OpenGL Voodoo restored [1][2]
- OpenGLide support restored (disabled by default) [3]
- Dynrec core restored/backported [1]
- x86 FPU core restored/backported [1]
- Printer emulation restored [3]
- SDL_Sound use for WAVE/AIFF/MP3 in CUE files restored [3]
- Direct3D output re-enabled [2][4]
- Tandy DAC emulation backported [3]
- backported fixes from mainline
- various minor fixes

[1] tested on x86, x86-64<br/>
[2] tested fullscreen and windowed mode<br/>
[3] untested<br/>
[4] tested MinGW32 cross-build under WINE up to the DOS prompt<br/>

In general, only casual (Win32: very casual) testing has been performed.


Sample configurations (host openSUSE 13.1 on x86-64):

- x86 (x86 dynamic core, x86 FPU):

  CC="gcc -m32" CXX="g++ -m32" ./configure --host=i686-suse-linux

- x86-64 (recompiling core, x86 FPU):

  ./configure

- Win32:

  FREETYPE_CONFIG=/usr/i686-w64-mingw32/sys-root/mingw/bin/freetype-config CC="i686-w64-mingw32-gcc" CXX="i686-w64-mingw32-g++" ./configure --host=mingw32 --includedir=/usr/i686-w64-mingw32/sys-root/mingw/include --with-sdl-prefix=/usr/i686-w64-mingw32/sys-root/mingw
