/*
 * Copyright (C) 2012 by Tomasz Moń <desowin@gmail.com>
 *
 * compile with:
 *   gcc -o sdlvideoviewer sdlvideoviewer.c -lSDL
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

#include <asm/types.h>		/* for videodev2.h */

#include <linux/videodev2.h>

#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define max(a, b) (a > b ? a : b)
#define min(a, b) (a > b ? b : a)

enum camera_io_method {
	IO_METHOD_READ,
	IO_METHOD_MMAP,
	IO_METHOD_USERPTR,
};

struct buffer
{
	void *start;
	size_t length;
};

static enum camera_io_method io = IO_METHOD_MMAP;
struct buffer *buffers = NULL;
static unsigned int n_buffers = 0;

static size_t WIDTH = 640;
static size_t HEIGHT = 480;
#ifdef TRACK_COLOR
static void track_color(const void *p);
#endif

static uint8_t *buffer_sdl;
SDL_Surface *data_sf;

static void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));

	exit(EXIT_FAILURE);
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int r;

	do {
		r = ioctl(fd, request, arg);
	} while (-1 == r && EINTR == errno);
	return r;
}

static void render(SDL_Surface * sf)
{
	SDL_Surface *screen = SDL_GetVideoSurface();
	if (SDL_BlitSurface(sf, NULL, screen, NULL) == 0)
		SDL_UpdateRect(screen, 0, 0, 0, 0);
}

void YCbCrToRGB(int y, int cb, int cr, uint8_t * r, uint8_t * g, uint8_t * b)
{
	double Y = (double)y;
	double Cb = (double)cb;
	double Cr = (double)cr;

	int R = (int)(Y + 1.40200 * (Cr - 0x80));
	int G = (int)(Y - 0.34414 * (Cb - 0x80) - 0.71414 * (Cr - 0x80));
	int B = (int)(Y + 1.77200 * (Cb - 0x80));

	*r = max(0, min(255, R));
	*g = max(0, min(255, G));
	*b = max(0, min(255, B));
}

/*
 * YCbCr to RGB lookup table
 *
 * Indexes are [Y][Cb][Cr]
 * Y, Cb, Cr range is 0-255
 *
 * Stored value bits:
 *   24-16 Red
 *   15-8  Green
 *   7-0   Blue
 */
uint32_t YCbCr_to_RGB[256][256][256];

static void generate_YCbCr_to_RGB_lookup()
{
	int y;
	int cb;
	int cr;

	for (y = 0; y < 256; y++) {
		for (cb = 0; cb < 256; cb++) {
			for (cr = 0; cr < 256; cr++) {
				double Y = (double)y;
				double Cb = (double)cb;
				double Cr = (double)cr;

				int R = (int)(Y+1.40200*(Cr - 0x80));
				int G = (int)(Y-0.34414*(Cb - 0x80)-0.71414*(Cr - 0x80));
				int B = (int)(Y+1.77200*(Cb - 0x80));

				R = max(0, min(255, R));
				G = max(0, min(255, G));
				B = max(0, min(255, B));

				YCbCr_to_RGB[y][cb][cr] = R << 16 | G << 8 | B;
			}
		}
	}
}

#define COLOR_GET_RED(color)   ((color >> 16) & 0xFF)
#define COLOR_GET_GREEN(color) ((color >> 8) & 0xFF)
#define COLOR_GET_BLUE(color)  (color & 0xFF)

/**
 *  Converts YUV422 to RGB
 *  Before first use call generate_YCbCr_to_RGB_lookup();
 *
 *  input is pointer to YUV422 encoded data in following order: Y0, Cb, Y1, Cr.
 *  output is pointer to 24 bit RGB buffer.
 *  Output data is written in following order: R1, G1, B1, R2, G2, B2.
 */
static inline void YUV422_to_RGB(uint8_t * output, const uint8_t * input)
{
	uint8_t y0 = input[0];
	uint8_t cb = input[1];
	uint8_t y1 = input[2];
	uint8_t cr = input[3];

	uint32_t rgb = YCbCr_to_RGB[y0][cb][cr];
	output[0] = COLOR_GET_RED(rgb);
	output[1] = COLOR_GET_GREEN(rgb);
	output[2] = COLOR_GET_BLUE(rgb);

	rgb = YCbCr_to_RGB[y1][cb][cr];
	output[3] = COLOR_GET_RED(rgb);
	output[4] = COLOR_GET_GREEN(rgb);
	output[5] = COLOR_GET_BLUE(rgb);
}

static void process_image(const void *p)
{
	const uint8_t *buffer_yuv = p;

	size_t x;
	size_t y;

	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x += 2)
			YUV422_to_RGB(buffer_sdl + (y * WIDTH + x) * 3,
					  buffer_yuv + (y * WIDTH + x) * 2);
#ifdef TRACK_COLOR
	track_color(&buffer_yuv);
#endif
	render(data_sf);
}

#ifdef TRACK_COLOR
static void  track_color(const void *p)
{
	size_t x;
	size_t y;
	const uint8_t *buffer_track = p;

	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x += 2)
			YUV422_to_RGB(buffer_sdl + (y * WIDTH + x) * 3,
					buffer_track + (y * WIDTH + x) * 2);

	render(data_sf);
}
#endif

static int read_frame(int fd)
{
	struct v4l2_buffer buf;
	unsigned int i;

	switch (io) {
	case IO_METHOD_READ:
		if (-1 == read(fd, buffers[0].start, buffers[0].length)) {
			switch (errno) {
			case EAGAIN:
				return 0;
			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("read");
			}
		}
		process_image(buffers[0].start);
		break;
	case IO_METHOD_MMAP:
		CLEAR(buf);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
			switch (errno) {
			case EAGAIN:
				return 0;
			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("VIDIOC_DQBUF");
			}
		}
		assert(buf.index < n_buffers);
		process_image(buffers[buf.index].start);
		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");
		break;
	case IO_METHOD_USERPTR:
		CLEAR(buf);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_USERPTR;
		if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
			switch (errno) {
			case EAGAIN:
				return 0;
			case EIO:
				/* Could ignore EIO, see spec. */
				/* fall through */
			default:
				errno_exit("VIDIOC_DQBUF");
			}
		}
		for (i = 0; i < n_buffers; ++i)
			if (buf.m.userptr == (unsigned long)buffers[i].start
				&& buf.length == buffers[i].length)
				break;
		assert(i < n_buffers);
		process_image((void *)buf.m.userptr);

		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");
		break;
	}
	return 1;
}

static void mainloop(int fd)
{
	SDL_Event event;

	for (;;) {

		while (SDL_PollEvent(&event))
			if (event.type == SDL_QUIT)
				return;
		for (;;) {
			fd_set fds;
			struct timeval tv;
			int r;

			FD_ZERO(&fds);
			FD_SET(fd, &fds);

			/* Timeout. */
			tv.tv_sec = 2;
			tv.tv_usec = 0;

			r = select(fd + 1, &fds, NULL, NULL, &tv);

			if (-1 == r) {
				if (EINTR == errno)
					continue;
				errno_exit("select");
			}

			if (0 == r) {
				fprintf(stderr, "select timeout\n");
				exit(EXIT_FAILURE);
			}

			if (read_frame(fd))
				break;
			/* EAGAIN - continue select loop. */
		}
	}
}

static void stop_capturing(int fd)
{
	enum v4l2_buf_type type;

	switch (io) {
	case IO_METHOD_READ:
		/* Nothing to do. */
		break;
	case IO_METHOD_MMAP:
	case IO_METHOD_USERPTR:
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
			errno_exit("VIDIOC_STREAMOFF");
		break;
	}
}

static void start_capturing(int fd)
{
	unsigned int i;
	enum v4l2_buf_type type;

	switch (io) {
	case IO_METHOD_READ:
		/* Nothing to do. */
		break;
	case IO_METHOD_MMAP:
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = i;

			if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
				errno_exit("VIDIOC_QBUF");
		}
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
			errno_exit("VIDIOC_STREAMON");
		break;
	case IO_METHOD_USERPTR:
		for (i = 0; i < n_buffers; ++i) {
			struct v4l2_buffer buf;

			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_USERPTR;
			buf.index = i;
			buf.m.userptr = (unsigned long)buffers[i].start;
			buf.length = buffers[i].length;

			if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
				errno_exit("VIDIOC_QBUF");
		}
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
			errno_exit("VIDIOC_STREAMON");
		break;
	}
}

static void uninit_device(void)
{
	unsigned int i;

	switch (io) {
	case IO_METHOD_READ:
		free(buffers[0].start);
		break;
	case IO_METHOD_MMAP:
		for (i = 0; i < n_buffers; ++i)
			if (-1 == munmap(buffers[i].start, buffers[i].length))
				errno_exit("munmap");
		break;
	case IO_METHOD_USERPTR:
		for (i = 0; i < n_buffers; ++i)
			free(buffers[i].start);
		break;
	}
	free(buffers);
}

static void init_read(unsigned int buffer_size)
{
	buffers = calloc(1, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	buffers[0].length = buffer_size;
	buffers[0].start = malloc(buffer_size);

	if (!buffers[0].start) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}
}

static void init_mmap(int fd, char *dev_name)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support memory mapping\n", dev_name);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2) {
		fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
		exit(EXIT_FAILURE);
	}

	buffers = calloc(req.count, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
		struct v4l2_buffer buf;

		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n_buffers;

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_exit("VIDIOC_QUERYBUF");

		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start = mmap(NULL /* start anywhere */ ,
		buf.length, PROT_READ | PROT_WRITE /* required */, MAP_SHARED /* recommended */, fd, buf.m.offset);

		if (MAP_FAILED == buffers[n_buffers].start)
			errno_exit("mmap");
	}
}

static void init_userp(int fd, char *dev_name, unsigned int buffer_size)
{
	struct v4l2_requestbuffers req;
	long page_size;

	page_size = sysconf(_SC_PAGESIZE);
	buffer_size = (buffer_size + page_size - 1) & ~(page_size - 1);

	CLEAR(req);

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support user pointer i/o\n", dev_name);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	buffers = calloc(4, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
		buffers[n_buffers].length = buffer_size;
		buffers[n_buffers].start = memalign( /* boundary */ page_size,
		buffer_size);

		if (!buffers[n_buffers].start) {
			fprintf(stderr, "Out of memory\n");
			exit(EXIT_FAILURE);
		}
	}
}

static void init_device(int fd, char *dev_name)
{
	struct v4l2_capability cap;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	struct v4l2_format fmt;
	unsigned int min;

	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s is no V4L2 device\n", dev_name);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		fprintf(stderr, "%s is no video capture device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	switch (io) {
	case IO_METHOD_READ:
		if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
			fprintf(stderr, "%s does not support read i/o\n", dev_name);
			exit(EXIT_FAILURE);
		}
		break;
	case IO_METHOD_MMAP:
	case IO_METHOD_USERPTR:
		if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
			fprintf(stderr, "%s does not support streaming i/o\n", dev_name);
			exit(EXIT_FAILURE);
		}
		break;
	}


	/* Select video input, video standard and tune here. */

	CLEAR(cropcap);
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect;   /* reset to default */

		if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
			switch (errno) {
			case EINVAL:
				/* Cropping not supported. */
				break;
			default:
				/* Errors ignored. */
				break;
			}
		}
	} else {
		/* Errors ignored. */
	}

	CLEAR(fmt);

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = WIDTH;
	fmt.fmt.pix.height = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_ALTERNATE; //V4L2_FIELD_INTERLACED;

	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
		errno_exit("VIDIOC_S_FMT");

	/* Note VIDIOC_S_FMT may change width and height. */

	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;

	if (fmt.fmt.pix.width != WIDTH)
		WIDTH = fmt.fmt.pix.width;

	if (fmt.fmt.pix.height != HEIGHT)
		HEIGHT = fmt.fmt.pix.height;

	switch (io) {
	case IO_METHOD_READ:
		init_read(fmt.fmt.pix.sizeimage);
		break;
	case IO_METHOD_MMAP:
		init_mmap(fd, dev_name);
		break;
	case IO_METHOD_USERPTR:
		init_userp(fd, dev_name, fmt.fmt.pix.sizeimage);
		break;
	}
}

static void close_device(int *fd)
{
	if (-1 == close(*fd))
		errno_exit("close");
	*fd = -1;
}

static int open_device(char *dev_name)
{
	struct stat st;
	int fd;

	if (-1 == stat(dev_name, &st)) {
		fprintf(stderr, "Cannot identify '%s': %d, %s\n", dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is no device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	fd = open(dev_name, O_RDWR /* required */  | O_NONBLOCK, 0);

	if (-1 == fd) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n", dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return fd;
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
			io = IO_METHOD_MMAP;
			break;
		case 'r':
			io = IO_METHOD_READ;
			break;
		case 'u':
			io = IO_METHOD_USERPTR;
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

	generate_YCbCr_to_RGB_lookup();

	fd = open_device(dev_name);
	init_device(fd, dev_name);

	atexit(SDL_Quit);
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		return 1;

	SDL_WM_SetCaption("SDL Video viewer", NULL);

	buffer_sdl = (uint8_t*)malloc(WIDTH*HEIGHT*3);

	SDL_SetVideoMode(WIDTH, HEIGHT, 24, SDL_HWSURFACE);

	data_sf = SDL_CreateRGBSurfaceFrom(buffer_sdl, WIDTH, HEIGHT, 24, WIDTH * 3, mask32(0), mask32(1), mask32(2), 0);

	SDL_SetEventFilter(sdl_filter);

	start_capturing(fd);
	mainloop(fd);
	stop_capturing(fd);

	uninit_device();
	close_device(&fd);

	SDL_FreeSurface(data_sf);
	free(buffer_sdl);

	exit(EXIT_SUCCESS);

	return 0;
}
