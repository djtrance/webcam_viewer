#ifndef CAMERA_CAPTURE_H__
#define CAMERA_CAPTURE_H__
/*
 * Copyright (C) 2012 by Tomasz Moń <desowin@gmail.com>
 * Copyright (C) 2021 by Stephen M. Cameron <stephenmcameron@gmail.com>
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

enum camera_io_method {
	CAMERA_IO_METHOD_READ,
	CAMERA_IO_METHOD_MMAP,
	CAMERA_IO_METHOD_USERPTR,
};

typedef void (*camera_render_frame_fn)(void *cookie);

int camera_open_device(char *dev_name);
void camera_init_device(int fd, char *dev_name, enum camera_io_method io_method);
void camera_start_capturing(int fd, uint8_t *buffer_rgb, camera_render_frame_fn render_fn, void *cookie);
void camera_capture_frame(int fd);
void camera_stop_capturing(int fd);
void camera_uninit_device(void);
void camera_close_device(int *fd);

#endif

