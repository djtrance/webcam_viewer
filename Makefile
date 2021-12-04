CFLAGS=-Wall -Wextra --pedantic -std=c99
LIBS=-lm -lSDL

all:	sdlvideoviewer

sdlvideoviewer:	sdlvideoviewer.c
	$(CC) ${CFLAGS} sdlvideoviewer.c -o sdlvideoviewer ${LIBS}

clean:
	rm -f sdlvideoviewer

