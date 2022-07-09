#!/bin/sh


if [ -d "./linux_build" ] 
then
	rm -rfv linux_build
	mkdir linux_build	
else
	mkdir linux_build
fi

cd linux_build
gcc ../main.c -o vp -Wno-implicit-function-declaration -lm -lX11 -lwayland-client -lavcodec -lavformat -lswresample -lavutil -lSDL2 -lswscale -lz
