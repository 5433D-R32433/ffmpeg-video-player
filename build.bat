@echo off

if not defined DevEnvDir (
call vc x64
)
REM 

set CFLAGS= -DSDL_MAIN_HANDLED /MDd /Z7 /GS- /Oi /Gm- /GR- /sdl- /FC /TC /I..\include  
set LFLAGS= /link /LIBPATH:..\lib /out:vp.exe sdl2d.lib advapi32.lib shell32.lib ole32.lib version.lib winmm.lib Setupapi.lib Imm32.lib OleAut32.lib user32.lib kernel32.lib gdi32.lib avformat.lib avcodec.lib  swscale.lib avdevice.lib avfilter.lib avutil.lib ws2_32.lib vpx.lib opus.lib lzmad.lib zlibd.lib bz2d.lib GlU32.Lib OpenCL.lib OpenGL32.Lib aom.lib ass.lib brotlicommon-static.lib brotlidec-static.lib brotlienc-static.lib charset.lib dav1d.lib freetyped.lib fribidi.lib harfbuzz-subset.lib harfbuzz.lib iconv.lib ilbc.lib libmp3lame-static.lib libmpghip-static.lib libpng16d.lib libspeexd.lib libwebpmuxd.lib lzmad.lib modplug.lib ogg.lib openh264.lib openjp2.lib snappyd.lib soxr.lib swresample.lib theora.lib theoradec.lib theoraenc.lib vorbis.lib vorbisenc.lib vorbisfile.lib webpd.lib webpdecoderd.lib webpdemuxd.lib zlibd.lib secur32.lib ole32.lib advapi32.lib Cfgmgr32.lib Mfplat.lib bcrypt.lib Strmiids.lib Mfuuid.lib /NODEFAULTLIB:LIBCMTD /NXCOMPAT:NO /DYNAMICBASE:NO


set SOURCES= ../main.c

if exist build (
rmdir /S /Q build
mkdir build
pushd build
cl %CFLAGS% %SOURCES% %LFLAGS%
popd build
) else (
mkdir build
pushd build
cl %CFLAGS% %SOURCES% %LFLAGS%
popd build
)



