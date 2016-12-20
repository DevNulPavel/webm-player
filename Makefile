
INCLUDE = -I. -Inestegg/include -Inestegg/halloc -Ilibvpx/vpx -Ilibvpx/vpx_codec -Ilibvpx/vpx_ports
LIBS_PATH = -L. -L/usr/local/lib
LIBS = -lvorbis -logg -lSDL vpx-build/libvpx.a

UNAME = $(shell uname -s)
# Linux
ifeq "$(UNAME)" "Linux"
	LIBS += -lasound
endif
# OSX
ifeq "$(UNAME)" "Darwin"
	INCLUDE += -I/opt/local/include -I/usr/local/include
	LIBS += -lSDLmain -framework Carbon -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework Cocoa
endif



all: webm

vpx-build/vpx_config.h: libvpx/configure
	mkdir -p vpx-build && cd vpx-build && ../libvpx/configure

vpx-build/libvpx.a: vpx-build/vpx_config.h
	cd vpx-build && make

nestegg/configure: nestegg/configure.ac
	cd nestegg && autoreconf --install && ./configure && cd ..

nestegg/src/nestegg.o: nestegg/configure nestegg/src/nestegg.c
	make -C nestegg

webm.o: webm.cpp vpx-build/libvpx.a nestegg/src/nestegg.o
	g++ -g -c $(INCLUDE) -o webm.o webm.cpp

webm: webm.o nestegg/halloc/src/halloc.o nestegg/src/nestegg.o vpx-build/libvpx.a
	g++ -g -o webm webm.o nestegg/halloc/src/halloc.o nestegg/src/nestegg.o $(LIBS_PATH) $(LIBS)

clean: 
	rm -f *.o webm && rm -r vpx-build && make -C nestegg clean

nestegg/halloc/src/halloc.o: nestegg/halloc/src/halloc.c
	gcc -g -c $(INCLUDE) -o halloc.o nestegg/halloc/src/halloc.c

nestegg.o: nestegg/src/nestegg.c
	gcc -g -c $(INCLUDE) -o nestegg.o nestegg/src/nestegg.c

