INCLUDES := -I $(SYSROOT)/include -I $(SYSROOT)/usr/include -I $(SYSROOT)/include/arm-linux-gnueabihf -I $(SYSROOT)/arm-linux-gnueabihf/libc/usr/include
LIBS := -L $(SYSROOT)/lib -L $(SYSROOT)/usr/lib -L $(SYSROOT)/lib/arm-linux-gnueabihf -L $(SYSROOT)/arm-linux-gnueabihf/libc/usr/lib -L $(SYSROOT)/usr/lib/arm-linux-gnueabihf
CFLAGS := -O2 -march=armv7-a -mfpu=neon -Wl,'-z noexecstack'

all: capture video_echo 

capture: capture.c
	$(CROSS_COMPILE)gcc $(CFLAGS) $(INCLUDES) $(LIBS) -o capture capture.c huffman.c -ljpeg 

video_echo: video_echo.c
	$(CROSS_COMPILE)gcc $(CFLAGS) $(INCLUDES) $(LIBS) -o video_echo video_echo.c memcpy_neon.S huffman.c -ljpeg

clean:
	@rm -vf video_echo capture *.o *~
