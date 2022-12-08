/*
 * Copyright (C) 2012 by Tomasz Moń <desowin@gmail.com>
 * Copyright (C) 2021 by Stephen M. Cameron <stephenmcameron@gmail.com>
 *
 * compile with:
 *   make sdlvideoviewer
 *
 * Based on V4L2 video capture example
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of a copyright holder shall not
 * be used in advertising or otherwise to promote the sale, use or other dealings
 * in this Software without prior written authorization of the copyright holder.
 */

#include <SDL/SDL.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define __USE_BSD

#include <getopt.h>		/* getopt_long() */
#include <fcntl.h>		/* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "camera_capture.h"

#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define max(a, b) (a > b ? a : b)
#define min(a, b) (a > b ? b : a)

static enum camera_io_method io = CAMERA_IO_METHOD_MMAP;
static size_t WIDTH = 640;
static size_t HEIGHT = 480;
#ifdef TRACK_COLOR
static void track_color(const void *p);
#endif

static uint8_t *buffer_sdl;
SDL_Surface *data_sf;

static void render(void *cookie)
{
	SDL_Surface *sf = cookie;
	SDL_Surface *screen = SDL_GetVideoSurface();
	if (SDL_BlitSurface(sf, NULL, screen, NULL) == 0)
		SDL_UpdateRect(screen, 0, 0, 0, 0);
}

static void usage(FILE * fp, __attribute__((unused)) int argc, char **argv)
{
	fprintf(fp,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"-d | --device name   Video device name [/dev/video]\n"
		"-h | --help          Print this message\n"
		"-m | --mmap          Use memory mapped buffers\n"
		"-r | --read          Use read() calls\n"
		"-u | --userp         Use application allocated buffers\n"
		"-x | --width         Video width\n"
		"-y | --height        Video height\n"
		"", argv[0]);
}

static const char short_options[] = "d:hmrux:y:";

static const struct option long_options[] = {
	{"device", required_argument, NULL, 'd'},
	{"help", no_argument, NULL, 'h'},
	{"mmap", no_argument, NULL, 'm'},
	{"read", no_argument, NULL, 'r'},
	{"userp", no_argument, NULL, 'u'},
	{"width", required_argument, NULL, 'x'},
	{"height", required_argument, NULL, 'y'},
	{0, 0, 0, 0}
};

static int sdl_filter(const SDL_Event * event)
{
	return event->type == SDL_QUIT;
}

#define mask32(BYTE) (*(uint32_t *)(uint8_t [4]){ [BYTE] = 0xff })

int main(int argc, char **argv)
{
	int fd = -1;
	char *dev_name = "/dev/video0";

	for (;;) {
		int index;
		int c;

		c = getopt_long(argc, argv, short_options, long_options, &index);

		if (-1 == c)
			break;

		switch (c) {
		case 0:                /* getopt_long() flag */
			break;
		case 'd':
			dev_name = optarg;
			break;
		case 'h':
			usage(stdout, argc, argv);
			exit(EXIT_SUCCESS);
		case 'm':
			io = CAMERA_IO_METHOD_MMAP;
			break;
		case 'r':
			io = CAMERA_IO_METHOD_READ;
			break;
		case 'u':
			io = CAMERA_IO_METHOD_USERPTR;
			break;
		case 'x':
			WIDTH = atoi(optarg);
			break;
		case 'y':
			HEIGHT = atoi(optarg);
			break;
		default:
			usage(stderr, argc, argv);
			exit(EXIT_FAILURE);
		}
	}

	fd = camera_open_device(dev_name);
	camera_init_device(fd, dev_name, io);

	atexit(SDL_Quit);
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		return 1;

	SDL_WM_SetCaption("SDL Video viewer", NULL);

	buffer_sdl = (uint8_t*)malloc(WIDTH*HEIGHT*3);

	SDL_SetVideoMode(WIDTH, HEIGHT, 24, SDL_HWSURFACE);

	data_sf = SDL_CreateRGBSurfaceFrom(buffer_sdl, WIDTH, HEIGHT, 24, WIDTH * 3, mask32(0), mask32(1), mask32(2), 0);

	SDL_SetEventFilter(sdl_filter);

	camera_start_capturing(fd, buffer_sdl, render, data_sf);
	do {
		SDL_Event event;
		while (SDL_PollEvent(&event))
			if (event.type == SDL_QUIT)
				goto done;
		camera_capture_frame(fd);
	} while (1);
done:
	camera_stop_capturing(fd);

	camera_uninit_device();
	camera_close_device(&fd);

	SDL_FreeSurface(data_sf);
	free(buffer_sdl);

	exit(EXIT_SUCCESS);

	return 0;
}
