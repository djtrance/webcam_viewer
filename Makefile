CFLAGS=-Wall -Wextra --pedantic -std=c99 -g
LIBS=-lm -lSDL

all:	sdlvideoviewer

sdlvideoviewer:	sdlvideoviewer.c camera_capture.o
	$(CC) ${CFLAGS} sdlvideoviewer.c -o sdlvideoviewer camera_capture.o ${LIBS}

camera_capture.o:	camera_capture.c camera_capture.h
	$(CC) ${CFLAGS} -c camera_capture.c

clean:
	rm -f sdlvideoviewer *.o

