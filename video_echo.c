/*
 *      test.c  --  USB Video Class test application
 *
 *      Copyright (C) 2005-2008
 *          Laurent Pinchart (laurent.pinchart@skynet.be)
 *
 *      Copyright (C) 2018-2022 Fabmicro, LLC.
 *          Ruslan Zalata (rz@fabmicro.ru)
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 */

/*
 * WARNING: This is just a test application. Don't file bug reports, flame me,
 * curse me on 7 generations :-).
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <jpeglib.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include "memcpy_neon.h"

#define SATURATE8(x) ((unsigned int) x <= 255 ? x : (x < 0 ? 0: 255))

int insert_huffman(unsigned char* from, int fromLen, unsigned char *to, int *toLen);
int insert_huffman2(unsigned char* from, int fromLen, unsigned char *to, int *toLen);
int insert_huffman3(unsigned char* from, int fromLen, unsigned char *to, int *toLen);
int insert_huffman4(unsigned char* from, int fromLen, unsigned char *to, int *toLen);

#define FB_FILE "/dev/fb0"

char *buf_types[] = { 
	"NONE",
	"V4L2_BUF_TYPE_VIDEO_CAPTURE (1)",
	"V4L2_BUF_TYPE_VIDEO_OUTPUT (2)",
	"V4L2_BUF_TYPE_VIDEO_OVERLAY (3)",
	"V4L2_BUF_TYPE_VBI_CAPTURE (4)",
	"V4L2_BUF_TYPE_VBI_OUTPUT (5)",
	"V4L2_BUF_TYPE_SLICED_VBI_CAPTURE (6)",
	"V4L2_BUF_TYPE_SLICED_VBI_OUTPUT (7)",
	"V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY (8)",
	"V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE (9)",
	"V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE (10)",
	"V4L2_BUF_TYPE_SDR_CAPTURE (11)",
	"V4L2_BUF_TYPE_SDR_OUTPUT (12)",
	"V4L2_BUF_TYPE_META_CAPTURE (13)",
	"V4L2_BUF_TYPE_META_OUTPUT (14)",
};


char *fb_file = FB_FILE;

int same_file = 0;

int MIN(int A, int B)
{
	if(A < B)
		return A;
	else
		return B;
}

typedef struct  _fb_v4l
{
        int fbfd ;                                                                     
	char *fbp;
        struct fb_var_screeninfo vinfo;                                                 
        struct fb_fix_screeninfo finfo;                                              
}fb_v41;

void rgb_to_framebuffer(fb_v41 *vd, int width, int height, int xoffset, int yoffset, JSAMPARRAY buffer)
{
        int x, y, location;
        unsigned short *loca_ptr, data;
        unsigned char Red, Green, Blue;

	for(y = 0; y < height; y++){

		location = xoffset * 2 + (y + yoffset) * vd->finfo.line_length;
		loca_ptr = (unsigned short *) (vd->fbp + location);

        	for(x = 0; x < width; x++){
                	Red = buffer[y][x * 3] >> 3;
	                Green = buffer[y][x * 3 + 1] >> 2;
        	        Blue = buffer[y][x * 3 + 2] >> 3;
                	data = (Red << 11) + (Green << 5) + Blue;
	                *(loca_ptr + x) = data;
        	}
	}
}

int open_framebuffer(char *ptr,fb_v41 *vd)
{
	int fbfd,screensize;
	fbfd = open( ptr, O_RDWR);
	if (fbfd < 0) {
		printf("Error: cannot open framebuffer device.%x\n",fbfd);
		return 0;
	}
	printf("The framebuffer device was opened successfully.\n");
		
	vd->fbfd = fbfd;
	
	if (ioctl(fbfd, FBIOGET_FSCREENINFO, &vd->finfo)) {
		printf("Error reading fixed information.\n");
		return 0;
	}

	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vd->vinfo)) {
		printf("Error reading variable information.\n");
		return 0;
	}

	printf("%dx%d, %dbpp, xoffset=%d ,yoffset=%d \n", vd->vinfo.xres, vd->vinfo.yres, vd->vinfo.bits_per_pixel,vd->vinfo.xoffset,vd->vinfo.yoffset );

	screensize = vd->vinfo.xres * vd->vinfo.yres * vd->vinfo.bits_per_pixel / 8;

	vd->fbp = (char *)mmap(0,vd->finfo.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);
	if ((int)vd->fbp == -1) {
		printf("Error: failed to map framebuffer device to memory.\n");
		return 0;
	}

	printf("The framebuffer device was mapped to memory successfully.\n");
	return  1;
}

static int video_open(const char *devname)
{
	struct v4l2_capability cap;
	int dev, ret;

	dev = open(devname, O_RDWR);
	if (dev < 0) {
		printf("Error opening device %s: %d.\n", devname, errno);
		return dev;
	}

	memset(&cap, 0, sizeof cap);
	ret = ioctl(dev, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		printf("Error ioctl(VIDIOC_QUERYCAP) on device %s: %s.\n",
			devname, strerror(errno));
		close(dev);
		return ret;
	}

	if ((cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) == 0) {
		printf("Error on device %s: single-plane video capture not supported. Supported caps are: 0x%08x\n",
			devname, cap.capabilities);
		close(dev);
		return -EINVAL;
	}

	printf("Device %s opened, card: %s.\n", devname, cap.card);
	return dev;
}

static int uvc_set_control(int dev, unsigned int id, int value)
{
	struct v4l2_control ctrl;
	int ret;

	ctrl.id = id;
	ctrl.value = value;

	ret = ioctl(dev, VIDIOC_S_CTRL, &ctrl);
	if (ret < 0) {
		printf("unable to set control(%d): %s (%d).\n",
			id, strerror(errno), errno);
	}
	return ret;
}


static int video_set_format(int dev, unsigned int *w, unsigned int *h, unsigned int format, unsigned int type)
{
	struct v4l2_format fmt;
	int ret;

	printf("video_set_format: trying format width: %d height: %d, format = %4s, buf_type = %s\n", *w, *h, &format, buf_types[type]);

	memset(&fmt, 0, sizeof fmt);
	fmt.type = type;
	fmt.fmt.pix.width = *w;
	fmt.fmt.pix.height = *h;
	fmt.fmt.pix.pixelformat = format;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	ret = ioctl(dev, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		printf("Unable to set format of buf type %s: %s.\n", buf_types[type], strerror(errno));
		return ret;
	}

	printf("video_set_format: settled format: width: %u height: %u buffer size: %u, format = %4s\n",
		fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage, &fmt.fmt.pix.pixelformat);

	*w = fmt.fmt.pix.width;
	*h = fmt.fmt.pix.height;

	return 0;
}

static int video_set_framerate(int dev, int fps, unsigned int type)
{
	struct v4l2_streamparm parm;
	int ret;

	printf("video_set_framerate\n");

	memset(&parm, 0, sizeof parm);
	parm.type = type;

	ret = ioctl(dev, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		printf("Unable to VIDIOC_G_PARM: %s.\n", strerror(errno));
		return 0;
	}

	printf("Current frame rate: %u/%u\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);

	memset(&parm, 0, sizeof parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;

	ret = ioctl(dev, VIDIOC_S_PARM, &parm);
	if (ret < 0) {
		printf("Unable to VIDIOC_S_PARM: %s.\n", strerror(errno));
		return ret;
	}

	ret = ioctl(dev, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		printf("Unable to VIDIOC_G_PARM: %s.\n", strerror(errno));
		return ret;
	}

	printf("Frame rate set: %u/%u\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);
	return 0;
}

static int video_reqbufs(int dev, int nbufs, unsigned int type)
{
	printf("video_reqbufs: allocating %d video buffers of buf type %d\n", nbufs, type);

	struct v4l2_requestbuffers rb;
	int ret;

	memset(&rb, 0, sizeof rb);
	rb.count = nbufs;
	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(dev, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		printf("Unable to allocate buffers: %s\n", strerror(errno));
		return ret;
	}

	printf("video_reqbufs: %u buffers allocated, type = 0x%08x\n", rb.count, rb.type);

	return rb.count;
}

static int video_enable(int dev, int enable, unsigned int type)
{
	int ret;

	ret = ioctl(dev, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		printf("Unable to %s capture: %d.\n",
			enable ? "start" : "stop", errno);
		return ret;
	}

	return 0;
}

static void video_query_menu(int dev, unsigned int id)
{
	struct v4l2_querymenu menu;
	int ret;

	menu.index = 0;
	while (1) {
		menu.id = id;
		ret = ioctl(dev, VIDIOC_QUERYMENU, &menu);
		if (ret < 0)
			break;

		printf("  %u: %.32s\n", menu.index, menu.name);
		menu.index++;
	};
}

static void video_list_controls(int dev)
{
	struct v4l2_queryctrl query;
	struct v4l2_control ctrl;
	char value[12];
	int ret;

#ifndef V4L2_CTRL_FLAG_NEXT_CTRL
	unsigned int i;

	for (i = V4L2_CID_BASE; i <= V4L2_CID_LASTP1; ++i) {
		query.id = i;
#else
	query.id = 0;
	while (1) {
		query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
#endif
		ret = ioctl(dev, VIDIOC_QUERYCTRL, &query);
		if (ret < 0)
			break;

		if (query.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;

		ctrl.id = query.id;
		ret = ioctl(dev, VIDIOC_G_CTRL, &ctrl);
		if (ret < 0)
			strcpy(value, "n/a");
		else
			sprintf(value, "%d", ctrl.value);

		printf("control 0x%08x %s min %d max %d step %d default %d current %s.\n",
			query.id, query.name, query.minimum, query.maximum,
			query.step, query.default_value, value);

		if (query.type == V4L2_CTRL_TYPE_MENU)
			video_query_menu(dev, query.id);

	}
}

int enum_frame_intervals(int dev, __u32 pixfmt, __u32 width, __u32 height)
{
        int ret;
        struct v4l2_frmivalenum fival;

        memset(&fival, 0, sizeof(fival));
        fival.index = 0;
        fival.pixel_format = pixfmt;
        fival.width = width;
        fival.height = height;
        printf("\tTime interval between frame: ");
        while ((ret = ioctl(dev, VIDIOC_ENUM_FRAMEINTERVALS, &fival)) == 0) {
                if (fival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                                printf("%u/%u, ",
                                                fival.discrete.numerator, fival.discrete.denominator);
                } else if (fival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
                                printf("{min { %u/%u } .. max { %u/%u } }, ",
                                                fival.stepwise.min.numerator, fival.stepwise.min.numerator,
                                                fival.stepwise.max.denominator, fival.stepwise.max.denominator);
                                break;
                } else if (fival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
                                printf("{min { %u/%u } .. max { %u/%u } / "
                                                "stepsize { %u/%u } }, ",
                                                fival.stepwise.min.numerator, fival.stepwise.min.denominator,
                                                fival.stepwise.max.numerator, fival.stepwise.max.denominator,
                                                fival.stepwise.step.numerator, fival.stepwise.step.denominator);
                                break;
                }
                fival.index++;
        }
        printf("\n");
        if (ret != 0 && errno != EINVAL) {
                perror("ERROR enumerating frame intervals");
                return errno;
        }

        return 0;
}

static int enum_frame_sizes(int dev, __u32 pixfmt)
{
        int ret;
        struct v4l2_frmsizeenum fsize;

        memset(&fsize, 0, sizeof(fsize));
        fsize.index = 0;
        fsize.pixel_format = pixfmt;
        while ((ret = ioctl(dev, VIDIOC_ENUM_FRAMESIZES, &fsize)) == 0) {
                if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                        printf("{ discrete: width = %u, height = %u }\n",
                                        fsize.discrete.width, fsize.discrete.height);
                        ret = enum_frame_intervals(dev, pixfmt,
                                        fsize.discrete.width, fsize.discrete.height);
                        if (ret != 0)
                                printf("  Unable to enumerate frame sizes.\n");
                } else if (fsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                        printf("{ continuous: min { width = %u, height = %u } .. "
                                        "max { width = %u, height = %u } }\n",
                                        fsize.stepwise.min_width, fsize.stepwise.min_height,
                                        fsize.stepwise.max_width, fsize.stepwise.max_height);
                        printf("  Refusing to enumerate frame intervals.\n");
                        break;
                } else if (fsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                        printf("{ stepwise: min { width = %u, height = %u } .. "
                                        "max { width = %u, height = %u } / "
                                        "stepsize { width = %u, height = %u } }\n",
                                        fsize.stepwise.min_width, fsize.stepwise.min_height,
                                        fsize.stepwise.max_width, fsize.stepwise.max_height,
                                        fsize.stepwise.step_width, fsize.stepwise.step_height);
                        printf("  Refusing to enumerate frame intervals.\n");
                        break;
                }
                fsize.index++;
        }
        if (ret != 0 && errno != EINVAL) {
                perror("ERROR enumerating frame sizes");
                return errno;
        }

        return 0;
}

static void video_list_formats(int dev)
{
        struct v4l2_fmtdesc fmt;
	int ret;

        memset(&fmt, 0, sizeof(fmt));
        fmt.index = 0;
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // will enum any buf type format

	printf("Supported formats:\n");

        while ((ret = ioctl(dev, VIDIOC_ENUM_FMT, &fmt)) == 0) {
                printf("{ index = %d, pixelformat = '%c%c%c%c', description = '%s' }\n",
			fmt.index,
                        fmt.pixelformat & 0xFF, (fmt.pixelformat >> 8) & 0xFF,
                        (fmt.pixelformat >> 16) & 0xFF, (fmt.pixelformat >> 24) & 0xFF,
                        fmt.description);
                        ret = enum_frame_sizes(dev, fmt.pixelformat);
                        if(ret != 0)
                                printf("  Unable to enumerate frame sizes.\n");

                fmt.index++;
        }

        if (errno != EINVAL) {
                perror("ERROR enumerating frame formats");
        }
}

static void video_enum_inputs(int dev)
{
	struct v4l2_input input;
	unsigned int i;
	int ret;

	printf("Supported inputs:\n");

	for (i = 0; ; ++i) {
		memset(&input, 0, sizeof input);
		input.index = i;
		ret = ioctl(dev, VIDIOC_ENUMINPUT, &input);
		if (ret < 0)
			break;

		if (i != input.index)
			printf("Warning: driver returned wrong input index "
				"%u.\n", input.index);

		printf("Input %u: %s.\n", i, input.name);
	}
}

static int video_get_input(int dev)
{
	__u32 input;
	int ret;

	ret = ioctl(dev, VIDIOC_G_INPUT, &input);
	if (ret < 0) {
		printf("Unable to get current input: %s.\n", strerror(errno));
		return ret;
	}

	return input;
}

static int video_set_input(int dev, unsigned int input)
{
	__u32 _input = input;
	int ret;

	ret = ioctl(dev, VIDIOC_S_INPUT, &_input);
	if (ret < 0)
		printf("Unable to select input %u: %s.\n", input,
			strerror(errno));

	return ret;
}

#define V4L_BUFFERS_DEFAULT	4	
#define V4L_BUFFERS_MAX		32

static void usage(const char *argv0)
{
	printf("Usage: %s [options] device\n", argv0);
	printf("Supported options:\n");
	printf("-c, --capture[nframes] 	Capture frames\n");
	printf("-d, --delay             Delay (in ms) before requeuing buffers\n");
	printf("-f, --format format	Set the video format (mjpg/yuyv/uyvy/rgb565/ba81/nv12/ym12)\n");
	printf("-h, --help		Show this help screen\n");
	printf("-i, --input input	Select the video input\n");
	printf("-l, --list-controls	List available controls\n");
	printf("-L, --list-formats	List available formats\n");
	printf("-m			Use multi-plane formate instead (default is single-plane)\n");
	printf("-n, --nbufs n		Set the number of video buffers\n");
	printf("-s, --size WxH		Set the frame size\n");
	printf("-S, --stream		Stream capturing mode\n");
	printf("-x, --stream		Store frames to same file\n");
	printf("-E			Exposure\n");
	printf("-r			Framerate (denominator)\n");
	printf("    --enum-inputs	Enumerate inputs\n");
	printf("    --skip n		Skip the first n frames\n");
}

#define OPT_ENUM_INPUTS		256
#define OPT_SKIP_FRAMES		257

static struct option opts[] = {
	{"capture", 2, 0, 'c'},
	{"delay", 1, 0, 'd'},
	{"enum-inputs", 0, 0, OPT_ENUM_INPUTS},
	{"format", 1, 0, 'f'},
	{"help", 0, 0, 'h'},
	{"input", 1, 0, 'i'},
	{"list-controls", 0, 0, 'l'},
	{"list-formats", 0, 0, 'L'},
	{"stream", 0, 0, 'S'},
	{"size", 1, 0, 's'},
	{"skip", 1, 0, OPT_SKIP_FRAMES},
	{0, 0, 0, 0}
};

int main(int argc, char *argv[])
{
	char filename[] = "quickcam-0000.jpg";
	int dev, ret;
	int buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	/* Options parsings */
	int do_enum_inputs = 0, do_capture = 0, do_stream = 0;
	int do_list_controls = 0, do_list_formats = 0, do_set_input = 0, do_white_balance = -1, do_brightness = -5, do_exposure = -5, do_framerate = 30;
	char *endptr;
	int c;

	/* Video buffers */
	void *mem[V4L_BUFFERS_MAX];
	unsigned int pixelformat = V4L2_PIX_FMT_RGB565;
	unsigned int width = 640;
	unsigned int height = 480;
	unsigned int nbufs = V4L_BUFFERS_DEFAULT;
	unsigned int input = 0;
	unsigned int skip = 0;

	/* Capture loop */
	struct timeval start, end, ts, ts2, ts3, ts4, ts5, ts6;
	unsigned int delay = 0, nframes = (unsigned int)-1;
	FILE *file;
	double fps;

	struct v4l2_buffer bufs[V4L_BUFFERS_MAX], *buf;
	unsigned int i;
	/* add by lfc */
	unsigned int count;
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
        JSAMPARRAY buffer;
        int row_stride;
	fb_v41 vd;
	/* end add */

	opterr = 0;
	while ((c = getopt_long(argc, argv, "F:c:d:f:hi:lLn:s:SxW:B:E:r:m", opts, NULL)) != -1) {

		switch (c) {
		case 'c':
			do_capture = 1;
			if (optarg)
                                nframes = atoi(optarg);
			break;
		case 'd':
			delay = atoi(optarg);
			break;
		case 'F':
			fb_file = optarg;
			break;
		case 'W':
			do_white_balance = atoi(optarg);
			break;
		case 'B':
			do_brightness = atoi(optarg);
			break;
		case 'E':
			do_exposure = atoi(optarg);
			break;
		case 'r':
			do_framerate = atoi(optarg);
			break;
		case 'x':
			same_file = 1;
			break;
		case 'm':
			buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			break;
		case 'f':
			if (strcasecmp(optarg, "MJPEG") == 0)
				pixelformat = V4L2_PIX_FMT_MJPEG;
			else if (strcasecmp(optarg, "YM12") == 0)
				pixelformat = V4L2_PIX_FMT_YUV420M;
			else if (strcasecmp(optarg, "NV12") == 0)
				pixelformat = V4L2_PIX_FMT_NV12;
			else if (strcasecmp(optarg, "YUYV") == 0)
				pixelformat = V4L2_PIX_FMT_YUYV;
			else if (strcasecmp(optarg, "UYVY") == 0)
				pixelformat = V4L2_PIX_FMT_UYVY;
			else if (strcasecmp(optarg, "RGBP") == 0)
				pixelformat = V4L2_PIX_FMT_RGB565;
			else if (strcasecmp(optarg, "BA10") == 0)
				pixelformat = V4L2_PIX_FMT_SGRBG10;
			else if (strcasecmp(optarg, "BG10") == 0)
				pixelformat = V4L2_PIX_FMT_SBGGR10;
			else if (strcasecmp(optarg, "BG12") == 0)
				pixelformat = V4L2_PIX_FMT_SBGGR12;
			else if (strcasecmp(optarg, "BA12") == 0)
				pixelformat = V4L2_PIX_FMT_SGRBG12;
			else if (strcasecmp(optarg, "BA81") == 0)
				pixelformat = V4L2_PIX_FMT_SBGGR8;
			else {
				printf("Unsupported video format '%s'\n", optarg);
				return 1;
			}
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'i':
			do_set_input = 1;
			input = atoi(optarg);
			break;
		case 'l':
			do_list_controls = 1;
			break;
		case 'L':
			do_list_formats = 1;
			break;
		case 'n':
			nbufs = atoi(optarg);
			if (nbufs > V4L_BUFFERS_MAX)
				nbufs = V4L_BUFFERS_MAX;
			break;
		case 's':
			width = strtol(optarg, &endptr, 10);
			if (*endptr != 'x' || endptr == optarg) {
				printf("Invalid size '%s'\n", optarg);
				return 1;
			}
			height = strtol(endptr + 1, &endptr, 10);
			if (*endptr != 0) {
				printf("Invalid size '%s'\n", optarg);
				return 1;
			}
			break;
		case 'S':
			do_stream = 1;
			break;
		case OPT_ENUM_INPUTS:
			do_enum_inputs = 1;
			break;
		case OPT_SKIP_FRAMES:
			skip = atoi(optarg);
			break;
		default:
			printf("Invalid option -%c\n", c);
			printf("Run %s -h for help.\n", argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}

	/* Open the video device. */
	dev = video_open(argv[optind]);
	if (dev < 0)
		return 1;

	if (do_list_controls){
		video_list_controls(dev);
		return 0;
	}	

	if (do_list_formats){
		video_list_formats(dev);
		return 0;
	}

	if (do_enum_inputs)
		video_enum_inputs(dev);

	if (do_set_input)
		video_set_input(dev, input);

	ret = video_get_input(dev);
	printf("Input %d selected\n", ret);

	ret = open_framebuffer(fb_file, &vd);
	if (ret == 0){
		printf("open framebuffer error!\n");
		return 0;	
	} 


	/* Allocate Z buffer */
	int z_buffer_size = vd.vinfo.xres * vd.vinfo.yres * vd.vinfo.bits_per_pixel / 8;
	char *z_buffer = (char*) malloc(z_buffer_size);

	if(!z_buffer) {
		printf("Failed to allocate Z buffer, size of %d (%d x %d x % d)\n", z_buffer_size, vd.vinfo.xres , vd.vinfo.yres);
	}

	printf("Setting video format of buf type %s\n", buf_types[buf_type]);

	/* Set the video format. */
	if (video_set_format(dev, &width, &height, pixelformat, buf_type) < 0) {
		close(dev);
		return 1;
	}

	printf("Format set ok\n");

	/* Set the frame rate. */
	if (video_set_framerate(dev, do_framerate, buf_type) < 0) {
		close(dev);
		return 1;
	}

	if(do_white_balance > -1) {
		int rc = uvc_set_control(dev, V4L2_CID_DO_WHITE_BALANCE, do_white_balance);
		if(rc < 0) {
			printf("White balance set error: %s\n", strerror(errno));
		} else {
			printf("White balance set to: %d\n", do_white_balance);
		}
	}

	if(do_brightness > -5) {
		int rc = uvc_set_control(dev, V4L2_CID_BRIGHTNESS, do_brightness);
		if(rc < 0) {
			printf("Brightness set error: %s\n", strerror(errno));
		} else {
			printf("Brighness set to: %d\n", do_brightness);
		}
	}

	if(do_exposure > -5) {
		int rc = uvc_set_control(dev, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
		rc += uvc_set_control(dev, V4L2_CID_EXPOSURE, do_exposure);
		if(rc < 0) {
			printf("Exposure set terror: %s\n", strerror(errno));
		} else {
			printf("Exposure set to: %d\n", do_exposure);
		}
	}

	/* Allocate buffers. */
	if ((int)(nbufs = video_reqbufs(dev, nbufs, buf_type)) < 0) {
		close(dev);
		return 1;
	}

	/* Map the buffers. */
	for (i = 0; i < nbufs; i++) {
		buf = &bufs[i];
		memset(buf, 0, sizeof(*buf));
		buf->index = i;
		buf->type = buf_type;
		buf->memory = V4L2_MEMORY_MMAP;

		printf("Querying buffer %d using ioctl(VIDIOC_QUERYBUF)\n", i);

		ret = ioctl(dev, VIDIOC_QUERYBUF, buf);
		if (ret < 0) {
			printf("Unable to query buffer %d: %s\n", i, strerror(errno));
			close(dev);
			return 1;
		}


		if(buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			mem[i] = mmap(0, buf->length, PROT_READ|PROT_WRITE, MAP_SHARED, dev, buf->m.offset);
			if (mem[i] == MAP_FAILED) {
				printf("Unable to map buffer i = %d: %s\n", i, strerror(errno));
				close(dev);
				return 1;
			}
			printf("Buffer i = %d mapped at address = %p, ", i, mem[i]);
			printf("width: %d, height: %d, length: %d offset: %d\n", width, height, buf->length, buf->m.offset);

		} else if(buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			printf("Multi-plane formats are not currently supported!\n");
			return 1;
		} else {
			printf("Format of buffer type %s is not suppored!\n", buf_types[buf_type]);
			return 1;
		}
	}


	/* Queue the buffers. */
	for (i = 0; i < nbufs; i++) {
		buf = &bufs[i];
		ret = ioctl(dev, VIDIOC_QBUF, buf);
		if (ret < 0) {
			printf("Unable to queue buffer (%d).\n", errno);
			close(dev);
			return 1;
		}

		printf("Buffer i = %d queued\n", i);
	}

	/* Start streaming. */
	video_enable(dev, 1, buf_type);

	printf("Video enabled\n");

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);

	int buf_idx = 0;

	i = 0;
	
	while(nframes) {

		buf = &bufs[buf_idx];

		memset(buf, 0, sizeof(*buf));
		buf->index = buf_idx;
		buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf->memory = V4L2_MEMORY_MMAP;

		printf("Dequeuing frame = %d, buf_idx = %d, index = %d, memory = %p\n", i, buf_idx, buf->index, buf->memory);

		buf_idx = (buf_idx + 1) % nbufs;

		gettimeofday(&ts, NULL);

		ret = ioctl(dev, VIDIOC_DQBUF, buf);
		if (ret < 0) {
			printf("Unable to dequeue buffer (%d).\n", errno);
			close(dev);
			return 1;
		}

		gettimeofday(&ts2, NULL);

		if (i == 0)
			start = ts;

		// HACK: if CSI returned data is zeroed, skip this fpame 
		if(*(unsigned int*)mem[buf->index] == 0x00000000) {
			printf("CSI returned zeros! Skipping!\n");
			goto skip_one_frame;
		}

		if(skip)
			goto skip_one_frame;

		if (do_capture) {

			if(pixelformat == V4L2_PIX_FMT_MJPEG)
				sprintf(filename, "/tmp/capture.jpg");
			else
				sprintf(filename, "/tmp/capture.raw", i);
			if(same_file)
				file = fopen(filename, "ab");
			else
				file = fopen(filename, "wb");

			if (file != NULL) {

				if(pixelformat == V4L2_PIX_FMT_MJPEG) {
					char tmp[200*1024];
					int tmp_len = 0;
					int rc = insert_huffman2(mem[buf->index], buf->bytesused, tmp, &tmp_len);
					printf("Adding huffman header: len before = %d, len after = %d, rc = %d\n", buf->bytesused, tmp_len, rc);
					printf("Written bytes: %d/%d to %s\n", fwrite(tmp, tmp_len, 1, file), tmp_len, filename);
				} else {
					printf("Written bytes: %d/%d to %s\n", fwrite(mem[buf->index], buf->bytesused, 1, file), buf->bytesused, filename);
				}
				fclose(file);
			}

			nframes--;
		}

		if (do_stream){
        		file = fopen(filename, "rb");
	        	if(file == NULL){
        	        	printf("Open file error!\n");
                		return 0;
	        	}

		        jpeg_stdio_src(&cinfo, file);
	
		        (void) jpeg_read_header(&cinfo, TRUE);

	        	jpeg_start_decompress(&cinfo);			

	        	row_stride = cinfo.output_width * cinfo.output_components;

	        	buffer = (*cinfo.mem->alloc_sarray)
        	        	((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, cinfo.output_height);

			int rowsRead = 0;
	        	while (cinfo.output_scanline < cinfo.output_height){
				
	                	rowsRead += jpeg_read_scanlines(&cinfo, &buffer[rowsRead], cinfo.output_height - rowsRead);	
			}

			//rgb_to_framebuffer(&vd, cinfo.output_width, cinfo.output_height, 0, 0, buffer);

		        (void) jpeg_finish_decompress(&cinfo);

		        fclose(file);		
		}	

		// Draw to LCD

		if(pixelformat == V4L2_PIX_FMT_RGB565) {
			//mem[buf->index], buf->bytesused;
			//screensize = vd->vinfo.xres * vd->vinfo.yres * vd->vinfo.bits_per_pixel / 8;
        		//vd->fbp = (char *)
			unsigned int *lcd_frame = (unsigned int*) vd.fbp;
			unsigned short *capture_frame = (unsigned short*) mem[buf->index];
			int x,y,i,j;
			unsigned int R, G, B, C;
			//for(y = 0; y < MIN(vd.vinfo.yres, height); y++) {
			for(y = 0; y < height; y++) {
				//j = (height - vd.vinfo.yres) / 2;
				unsigned int *p = lcd_frame;
				for(x = 0; x < MIN(vd.vinfo.xres, width); x++) {
					//i = (width - vd.vinfo.xres) / 2;
					C = capture_frame[(y) * width + (x)];
					R = (C >> 11) << 3;
					G = ((C & 0b11111100000) >> 5) << 2;
					B = (C & 0b11111) << 3;
					//lcd_frame[y * vd.vinfo.xres + x] = 0xffff;
					//lcd_frame[y * vd.vinfo.xres + x] = (0b11101 << 11);
					//lcd_frame[y * vd.vinfo.xres + x] = (0b111111 << 5);
					//lcd_frame[y * vd.vinfo.xres + x] = (0b11111 << 0);
					*p++ = ((R << 16) | (G << 8) | B);
					//lcd_frame[y * vd.finfo.line_length / 4 + x] =
						//capture_frame[(y+j) * width + (x+i)]; // & 0b1110111111111111;
					//	((R << 16) | (G << 8) | B);
				}
				lcd_frame += vd.finfo.line_length / 4;
			}

		}


		if(pixelformat == V4L2_PIX_FMT_UYVY) { // Chroma goes first !!!
			unsigned int *lcd_frame = (unsigned int*) vd.fbp;
			unsigned char *capture_frame = (unsigned char*) mem[buf->index];
			int i,j;
        		unsigned  y_start, u_start = 0, v_start = 0;
        		int r,g,b, r_prod, g_prod, b_prod;
		        int y,u,v;

			for(i = 0; i < height; i++) {
				unsigned int *p = lcd_frame;
				unsigned char *yuv = capture_frame+(i*width*2);
				for(j = 0; j < MIN(vd.vinfo.xres, width); j++) {


					if(j%2)
						v_start = *yuv++;
					else
						u_start = *yuv++;

					y_start = *yuv++;

					y = (y_start - 16)*298;
					u = u_start - 128;
					v = v_start - 128;

					r_prod = 409*v + 128;
					g_prod = 100*u + 208*v - 128;
					b_prod = 516*u;

					r =(y + r_prod)>>8;
					g =(y - g_prod)>>8;
					b =(y + b_prod)>>8;

                		        *p++ = (SATURATE8(r) << 16) | (SATURATE8(g) << 8) | SATURATE8(b);

				}
				lcd_frame += vd.finfo.line_length / 4;
			}
			
		}

		if(pixelformat == V4L2_PIX_FMT_YUYV) { // Luma goes first !!!

			unsigned int *lcd_frame = (unsigned int*) z_buffer;
			unsigned char *capture_frame = (unsigned char*) mem[buf->index];
			int i,j;
        		unsigned  y_start, u_start = 0, v_start = 0;
        		int r,g,b, r_prod, g_prod, b_prod;
		        int y,u,v;
			int height_min  = MIN(vd.vinfo.yres, height);

			for(i = 0; i < height_min; i++) {
				unsigned int *p = lcd_frame;
				unsigned int *yuv = (unsigned int*) (capture_frame+(i*width*2));
				int width_min = MIN(vd.vinfo.xres, width);

				for(j = 0; j < width_min/2; j++) {

					unsigned int YUV = *yuv++;

					// Odd pixel: Parse Y-U

					y_start = (YUV >> 0) & 0xff;
					u_start = (YUV >> 8) & 0xff;
	
					y = (y_start - 16)*298;
					u = u_start - 128;
					//v = v_start - 128;

					//r_prod = 409*v + 128;
					g_prod = 100*u + 208*v - 128;
					b_prod = 516*u;

					r =(y + r_prod)>>8;
					g =(y - g_prod)>>8;
					b =(y + b_prod)>>8;

              	 		       	*p++ = (SATURATE8(r) << 16) | (SATURATE8(g) << 8) | SATURATE8(b);


					// Even pixel: Parse Y-V

					y_start = (YUV >> 16) & 0xff;
					v_start = (YUV >> 24) & 0xff;

					y = (y_start - 16)*298;
					//u = u_start - 128;
					v = v_start - 128;

					r_prod = 409*v + 128;
					g_prod = 100*u + 208*v - 128;
					//b_prod = 516*u;

					r =(y + r_prod)>>8;
					g =(y - g_prod)>>8;
					b =(y + b_prod)>>8;

               		       		*p++ = (SATURATE8(r) << 16) | (SATURATE8(g) << 8) | SATURATE8(b);

				}


				lcd_frame += vd.finfo.line_length / 4;
			}
			
			//memmove(vd.fbp, z_buffer, z_buffer_size); 
			memcpy_neon(vd.fbp, z_buffer, z_buffer_size); 
		}

		if(pixelformat == V4L2_PIX_FMT_SBGGR8) { // 

			unsigned int *lcd_frame = (unsigned int*) z_buffer;
			unsigned char *capture_frame = (unsigned char*) mem[buf->index];
			int i,j;
        		unsigned  y_start, u_start = 0, v_start = 0;

			int height_min  = MIN(vd.vinfo.yres, height);

			for(i = 0; i < height_min; i+=2) {
				unsigned int *p = lcd_frame;
				int width_min = MIN(vd.vinfo.xres, width);

				for(j = 0; j < width_min; j+=2) {

					unsigned int b = capture_frame[i * width + j];
					unsigned int g = (capture_frame[i * width + j + 1] + 
							  capture_frame[(i+1) * width + j]) / 2;
					unsigned int r = capture_frame[(i+1) * width + j + 1];
					unsigned int c = 0xff000000 | ( r << 16) | (g<<8) | b;

               		       		*p = c;
               		       		*(p+1) = c;
               		       		*(p+width_min) = c;
               		       		*(p+width_min + 1) = c;

					p+=2;
				}


				lcd_frame += (vd.finfo.line_length / 4) * 2;
			}
			
			//memmove(vd.fbp, z_buffer, z_buffer_size); 
			memcpy_neon(vd.fbp, z_buffer, z_buffer_size); 
		}


		skip_one_frame:

		gettimeofday(&ts4, NULL);



                if (skip)
                        --skip;

                if (delay > 0)
                        usleep(delay * 1000);

		/* Requeue the buffer. */
		gettimeofday(&ts5, NULL);

		//memset(mem[buf->index], 0, buf->bytesused);
		*(unsigned int*)mem[buf->index] = 0;

		ret = ioctl(dev, VIDIOC_QBUF, buf);
		if (ret < 0) {
			printf("Unable to requeue buffer (%d).\n", errno);
			close(dev);
			return 1;
		}

		gettimeofday(&ts6, NULL);


		printf("Dequeued buffer: index = %u, i = %u, buf.memory: %p, bytesused: %u, length: %u, size: %dx%d, ts: %ld.%06ld %ld.%06ld, dequeing time: %.3f, drawing time: %.3f, requeing time: %.3f, total time: %.3f, fps: %0.1f\n\n", buf->index, i, buf->memory, buf->bytesused, buf->length, width, height, \
			buf->timestamp.tv_sec, buf->timestamp.tv_usec, ts.tv_sec, ts.tv_usec, 
			((ts2.tv_sec * 1000000LL + ts2.tv_usec)-(ts.tv_sec * 1000000LL + ts.tv_usec))/1000000.0,
			((ts4.tv_sec * 1000000LL + ts4.tv_usec)-(ts2.tv_sec * 1000000LL + ts2.tv_usec))/1000000.0,
			((ts6.tv_sec * 1000000LL + ts6.tv_usec)-(ts5.tv_sec * 1000000LL + ts5.tv_usec))/1000000.0,
			((ts6.tv_sec * 1000000LL + ts6.tv_usec)-(ts.tv_sec * 1000000LL + ts.tv_usec))/1000000.0,
			1.0 / (((ts6.tv_sec * 1000000LL + ts6.tv_usec)-(ts.tv_sec * 1000000LL + ts.tv_usec))/1000000.0)
			);

		fflush(stdout);

/*
		if(i % 10 == 0) {
			int rc;

			rc = uvc_set_control(dev, V4L2_CID_DO_WHITE_BALANCE, 4);
			if(rc < 0) {
				printf("White balance set error: %s\n", strerror(errno));
			} else {
				printf("White balance set to: %d\n", 4);
			}

			rc = uvc_set_control(dev, V4L2_CID_DO_WHITE_BALANCE, 0);
			if(rc < 0) {
				printf("White balance set error: %s\n", strerror(errno));
			} else {
				printf("White balance set to: %d\n", 0);
			}
		}
*/

		i++;
	}

	gettimeofday(&end, NULL);

	jpeg_destroy_decompress(&cinfo);

	/* Stop streaming. */
	video_enable(dev, 0, buf_type);

	end.tv_sec -= start.tv_sec;
	end.tv_usec -= start.tv_usec;
	if (end.tv_usec < 0) {
		end.tv_sec--;
		end.tv_usec += 1000000;
	}
	fps = (i-1)/(end.tv_usec+1000000.0*end.tv_sec)*1000000.0;

	printf("Captured %u frames in %lu.%06lu seconds (%f fps).\n",
		i-1, end.tv_sec, end.tv_usec, fps);

	close(dev);
	return 0;
}

