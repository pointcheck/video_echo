/*
 *  V4L2 video capture example
 *
 *  This program can be used and distributed without restrictions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/fb.h>

#include <asm/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>
#include <jpeglib.h>


#include "jpeg_mem.h"

#define CLEAR(x) memset (&(x), 0, sizeof (x))

static int con_fd, fb_fd=0, last_vt = -1;
static struct fb_fix_screeninfo fix;
static struct fb_var_screeninfo var;
unsigned char **line_addr;
static unsigned colormap [256];
__u32 xres, yres;
int bytes_per_pixel;
unsigned char* fbuffer;

static char *defaultfbdevice = "/dev/fb0";
static char *defaultconsoledevice = "/dev/tty";
static char *fbdevice = NULL;
static char *consoledevice = NULL;

struct v4l2_format fmt;

union multiptr {
        unsigned char *p8;
        unsigned short *p16;
        unsigned long *p32;
};



typedef enum {
	IO_METHOD_READ,
	IO_METHOD_MMAP,
	IO_METHOD_USERPTR,
} io_method;

struct capture_buffer {
        void *                  start;
        size_t                  length;
};

struct capture_buffer*	capture_buffers = NULL;
int capture_n_buffers = 2;
struct jpeg_decompress_struct jpeg_cinfo;
struct jpeg_error_mgr jerr;
JSAMPARRAY jpeg_buffer;


inline void pixel32 (int x, int y, unsigned color) {

        union multiptr loc;
        loc.p8 = line_addr [y] + x * bytes_per_pixel;

/*
        int red = (color >> 16) & 0xff;
        int green = (color >> 8) & 0xff;
        int blue = color & 0xff;
        int res = ((red >> (8 - 5)) << 11) |
                      ((green >> (8 - 6)) << 5) |
                      ((blue >> (8 - 5)) << 0);
*/

        *loc.p32 = color;
}

int open_framebuffer(void)
{
        struct vt_stat vts;
        char vtname[128];
        int fd, nr;
        unsigned y, addr;

        if ((fbdevice = getenv ("TSLIB_FBDEVICE")) == NULL)
                fbdevice = defaultfbdevice;

        if ((consoledevice = getenv ("TSLIB_CONSOLEDEVICE")) == NULL)
                consoledevice = defaultconsoledevice;

        if (strcmp (consoledevice, "none") != 0) {
                sprintf (vtname,"%s%d", consoledevice, 1);
                fd = open (vtname, O_WRONLY);
                if (fd < 0) {
                        perror("open consoledevice");
                        return -1;
                }

                if (ioctl(fd, VT_OPENQRY, &nr) < 0) {
                        perror("ioctl VT_OPENQRY");
                        return -1;
                }
                close(fd);

                sprintf(vtname, "%s%d", consoledevice, nr);

                con_fd = open(vtname, O_RDWR | O_NDELAY);
                if (con_fd < 0) {
                        perror("open tty");
                        return -1;
                }

                if (ioctl(con_fd, VT_GETSTATE, &vts) == 0)
                        last_vt = vts.v_active;

                if (ioctl(con_fd, VT_ACTIVATE, nr) < 0) {
                        perror("VT_ACTIVATE");
                        close(con_fd);
                        return -1;
                }

                if (ioctl(con_fd, VT_WAITACTIVE, nr) < 0) {
                        perror("VT_WAITACTIVE");
                        close(con_fd);
                        return -1;
                }

                if (ioctl(con_fd, KDSETMODE, KD_GRAPHICS) < 0) {
                        perror("KDSETMODE");
                        close(con_fd);
                        return -1;
                }

        }

        fb_fd = open(fbdevice, O_RDWR);
        if (fb_fd == -1) {
                perror("open fbdevice");
                return -1;
        }

        if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
                perror("ioctl FBIOGET_FSCREENINFO");
                close(fb_fd);
                return -1;
        }

        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0) {
                perror("ioctl FBIOGET_VSCREENINFO");
                close(fb_fd);
                return -1;
        }
        xres = var.xres;
        yres = var.yres;

        fbuffer = (unsigned char*) mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, fb_fd, 0);
        if (fbuffer == (unsigned char *)-1) {
                perror("mmap framebuffer");
                close(fb_fd);
                return -1;
        }
        memset(fbuffer,0,fix.smem_len);

        bytes_per_pixel = (var.bits_per_pixel + 7) / 8;
        line_addr = (unsigned char**) malloc (sizeof (__u32) * var.yres_virtual);
        addr = 0;
        for (y = 0; y < var.yres_virtual; y++, addr += fix.line_length)
                line_addr [y] = fbuffer + addr;


        printf("open_framebuffer() display resolution: %d x %d x %d\n", xres, yres, bytes_per_pixel * 8);
        printf("open_framebuffer() color scheme:\n");
        printf("var.red.length = %d, var.red.offset = %d\n", var.red.length, var.red.offset);
        printf("var.green.length = %d, var.green.offset = %d\n", var.green.length, var.green.offset);
        printf("var.blue.length = %d, var.blue.offset = %d\n", var.blue.length, var.blue.offset);


        return 0;
}




int xioctl (int fd, int request, void *arg) {
        int r;

        do r = ioctl (fd, request, arg);
        while (-1 == r && EINTR == errno);

        return r;
}

void process_image_SBGGR8(unsigned char* p, int len)
{
	int max_width = 352;
	int max_height = 288;
	int dX = 0, dY = 0;
	unsigned int x,y,c,r,g,b;
	for(y=0;y<max_height-1;y++)
		for(x=0;x<max_width-1;x++) {
			if((y % 2) == dY) { // second line
				if((x % 2) == dX) {
					g = p[y*max_width+x];// Green
					r = (p[(y-1)*max_width+x] + p[(y+1)*max_width+x])/2;
					b = (p[y*max_width+x-1] + p[y*max_width+x+1])/2;
				} else {
					b = p[y*max_width+x];; // Blue
					g = (p[y*max_width+x-1] + p[y*max_width+x+1] + p[(y-1)*max_width+x] + p[(y+1)*max_width+x]) / 4;
					r = (p[(y+1)*max_width+x+1] + p[(y+1)*max_width+x-1] + p[(y-1)*max_width+x-1] + p[(y-1)*max_width+x+1]) / 4;
				}
			} else { // first line
				if((x % 2) == dX) {
					r = p[y*max_width+x];; // Red
					b = (p[(y+1)*max_width+x+1] + p[(y+1)*max_width+x-1] + p[(y-1)*max_width+x-1] + p[(y-1)*max_width+x+1]) / 4;
					g = (p[(y+1)*max_width+x] + p[(y-1)*max_width+x] + p[y*max_width+x-1] + p[y*max_width+x+1]) / 4;
				} else {
					g = p[y*max_width+x]; // Green
					r = (p[y*max_width+x-1] + p[y*max_width+x+1]) / 2;
					b = (p[(y-1)*max_width+x] + p[(y+1)*max_width+x])/2;
				}
			}

			r = ((int)(r * 1.5));
			g = ((int)(g * 1.5 * 0.9));
			b = ((int)(b * 1.5 * 1.2));

			#define SATURATE8(x) (x > 255 ? 255 : x)

			pixel32(x, y, (SATURATE8(r) << 16) | (SATURATE8(g) << 8) | SATURATE8(b));
		}

}

int read_frame(int fd)
{
        struct v4l2_buffer buf;
	unsigned int i;

	struct timeval t1, t2;

	gettimeofday(&t1, NULL);

	CLEAR (buf);

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;


	if (xioctl (fd, VIDIOC_DQBUF, &buf) == -1) {
		fprintf(stderr, "ioctl error (VIDIOC_DQBUF)\n");
		return -1;
	}


	if(buf.index >= capture_n_buffers) {
		fprintf(stderr, "Weird buffer index: %d, capture_n_buffers: %d\n", buf.index, capture_n_buffers);
		return -1;
	}


	if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_SBGGR8)
    			process_image_SBGGR8 (capture_buffers[0].start, 0);

	if (xioctl (fd, VIDIOC_QBUF, &buf) == -1) {
		fprintf(stderr, "Failed to enqueue capture buffer, index: %d\n", buf.index);
		return -1;
	}


	gettimeofday(&t2, NULL);

	printf("Buf: i = %d, start = %p, len = %d, time = %ld\n", buf.index, capture_buffers[buf.index].start, buf.bytesused, t2.tv_usec - t1.tv_usec);

	return 1;
}

void mainloop(int fd) {

	unsigned int count;
	struct timeval t1, t2;

        count = 10000;

        while (count-- > 0) {
                for (;;) {
                        fd_set fds;
                        struct timeval tv;
                        int r;

                        FD_ZERO (&fds);
                        FD_SET (fd, &fds);

                        /* Timeout. */
                        tv.tv_sec = 2;
                        tv.tv_usec = 0;

			gettimeofday(&t1, NULL);

                        r = select (fd + 1, &fds, NULL, NULL, &tv);

			gettimeofday(&t2, NULL);

			printf("Select interval: %ld\n", t2.tv_usec - t1.tv_usec);

                        if (-1 == r) {
                                if (EINTR == errno)
                                        continue;

				fprintf(stderr, "Select error\n");
				break;
                        }

                        if (0 == r) {
                                fprintf (stderr, "Select timed out\n");
				continue;
                        }

			gettimeofday(&t1, NULL);

			if (read_frame(fd)) {
				gettimeofday(&t2, NULL);
				printf("Frame read time: %ld\n", t2.tv_usec - t1.tv_usec);
                    		break;
			}

	
			/* EAGAIN - continue select loop. */
                }
        }
}

void capture_stop(int fd) {
        enum v4l2_buf_type type;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl (fd, VIDIOC_STREAMOFF, &type);
}

int capture_start(int fd) {
        unsigned int i;
        enum v4l2_buf_type type;

	for (i = 0; i < capture_n_buffers; ++i) {
		struct v4l2_buffer buf;

		CLEAR (buf);

		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_MMAP;
		buf.index       = i;

		if (xioctl (fd, VIDIOC_QBUF, &buf) == -1) {
			fprintf(stderr, "Capture failed enqueue buffer, errno = %d\n", errno);
			return -1;
		}
		
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (xioctl (fd, VIDIOC_STREAMON, &type) == -1) {
		fprintf(stderr, "Capture failed to start streaming, capture_n_buffers = %d, fd = %d, errno = %d\n", capture_n_buffers, fd, errno);
		return -1;
	}

	fprintf(stderr, "Capture started on fd = %d\n", fd);

	return 0;
}

void capture_uninit_device(int fd) {
        unsigned int i;

	for (i = 0; i < capture_n_buffers; ++i)
		munmap (capture_buffers[i].start, capture_buffers[i].length);

	free(capture_buffers);

	close(fd);
}

int capture_init_device(char *dev_name, int format, int width, int height) {

        struct v4l2_capability cap;
        struct v4l2_cropcap cropcap;
        struct v4l2_crop crop;
	unsigned int min;

        struct stat st; 

        if (-1 == stat (dev_name, &st)) {
                fprintf (stderr, "Capture cannot identify '%s': %d, %s\n", dev_name, errno, strerror (errno));
		return -1;
        }

        if (!S_ISCHR (st.st_mode)) {
                fprintf (stderr, "%s is no device\n", dev_name);
		return -1;
        }

        int fd = open (dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

        if (-1 == fd) {
                fprintf (stderr, "Cannot open '%s': %d, %s\n", dev_name, errno, strerror (errno));
		return -1;
        }


	if (xioctl (fd, VIDIOC_QUERYCAP, &cap) == -1) {
		fprintf (stderr, "%s is no V4L2 device, errno = %d\n", dev_name, errno);
		return -1;
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
                fprintf (stderr, "%s is no video capture device\n", dev_name);
		return -1;
        }

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		fprintf (stderr, "%s does not support streaming i/o\n", dev_name);
		return -1;
	}


        /* Select video input, video standard and tune here. */


	CLEAR (cropcap);

        cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (0 == xioctl (fd, VIDIOC_CROPCAP, &cropcap)) {
                crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                crop.c = cropcap.defrect; /* reset to default */

                if (xioctl (fd, VIDIOC_S_CROP, &crop) == -1) {
                	fprintf (stderr, "Video capturing is not supported on %s\n", dev_name);
			return -1;
                }
        } else {	
                /* Errors ignored. */
        }


        CLEAR (fmt);

        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = width; 
        fmt.fmt.pix.height      = height;
        fmt.fmt.pix.pixelformat = format;
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        //fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

        if (xioctl (fd, VIDIOC_S_FMT, &fmt) == -1) {
               	fprintf (stderr, "Unsupported video settings, width = %d, height = %d, format = %08X\n", width, height, format);
		return -1;
	}

        /* Note VIDIOC_S_FMT may change width and height. */

	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;


	/* Init mmap */

	struct v4l2_requestbuffers req;

        CLEAR (req);

        req.count               = 4;
        req.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory              = V4L2_MEMORY_MMAP;

	if (xioctl (fd, VIDIOC_REQBUFS, &req) == -1) {
		fprintf (stderr, "%s does not support memory mapping\n", dev_name);
		return -1;
        }

        if (req.count < 2) {
                fprintf (stderr, "Insufficient buffer memory on %s\n", dev_name);
		return -1;
        }

        capture_buffers = calloc (req.count, sizeof (*capture_buffers));

        if (!capture_buffers) {
                fprintf (stderr, "Out of memory\n");
		return -1;
        }


	int n_buffers;

        for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
                struct v4l2_buffer buf;

                CLEAR (buf);

                buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory      = V4L2_MEMORY_MMAP;
                buf.index       = n_buffers;

                if (xioctl (fd, VIDIOC_QUERYBUF, &buf) == -1) {
                	fprintf(stderr, "Capture failed to query buffer info\n");
			return -1;
		}

                capture_buffers[n_buffers].length = buf.length;
                capture_buffers[n_buffers].start =
                        mmap (NULL /* start anywhere */,
                              buf.length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED /* recommended */,
                              fd, buf.m.offset);

                if (MAP_FAILED == capture_buffers[n_buffers].start) {
			fprintf(stderr, "Capture mmap error: %d\n", errno);
			return -1;
		}
        }

	/* Initialize JPEG stuff */

        jpeg_cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&jpeg_cinfo);



	return fd;
}




static void
usage                           (FILE *                 fp,
                                 int                    argc,
                                 char **                argv)
{
        fprintf (fp,
                 "Usage: %s [options]\n\n"
                 "Options:\n"
                 "-d | --device name   Video device name [/dev/video]\n"
                 "-h | --help          Print this message\n"
                 "-m | --mmap          Use memory mapped buffers\n"
                 "-r | --read          Use read() calls\n"
                 "-u | --userp         Use application allocated buffers\n"
                 "",
		 argv[0]);
}

static const char short_options [] = "d:hmru";

static const struct option
long_options [] = {
        { "device",     required_argument,      NULL,           'd' },
        { "help",       no_argument,            NULL,           'h' },
        { "mmap",       no_argument,            NULL,           'm' },
        { "read",       no_argument,            NULL,           'r' },
        { "userp",      no_argument,            NULL,           'u' },
        { 0, 0, 0, 0 }
};


int set_framerate(int dev, int rate) {
        struct v4l2_streamparm parm;
        int ret;

        memset(&parm, 0, sizeof parm);
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        ret = ioctl(dev, VIDIOC_G_PARM, &parm);
        if (ret < 0) {
                fprintf(stderr, "Unable to get frame rate: %d.\n", errno);
                return ret;
        }

        fprintf(stderr, "Current frame rate: %u/%u\n",
                parm.parm.capture.timeperframe.numerator,
                parm.parm.capture.timeperframe.denominator);

        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = rate;

        ret = ioctl(dev, VIDIOC_S_PARM, &parm);
        if (ret < 0) {
                fprintf(stderr, "Unable to set frame rate: %d.\n", errno);
                return ret;
        }

        ret = ioctl(dev, VIDIOC_G_PARM, &parm);
        if (ret < 0) {
                fprintf(stderr, "Unable to get frame rate: %d.\n", errno);
                return ret;
        }

        fprintf(stderr, "Frame rate set: %u/%u\n",
                parm.parm.capture.timeperframe.numerator,
                parm.parm.capture.timeperframe.denominator);
        return 0;
}

int main (int argc, char ** argv)
{
        char *dev_name = "/dev/video";
	int fd = -1;

        for (;;) {
                int index;
                int c;
                
                c = getopt_long (argc, argv,
                                 short_options, long_options,
                                 &index);

                if (-1 == c)
                        break;

                switch (c) {
                case 0: /* getopt_long() flag */
                        break;

                case 'd':
                        dev_name = optarg;
                        break;

                case 'h':
                        usage (stdout, argc, argv);
                        exit (EXIT_SUCCESS);

                default:
                        usage (stderr, argc, argv);
                        exit (EXIT_FAILURE);
                }
        }

	open_framebuffer();

	if((fd = capture_init_device(dev_name, V4L2_PIX_FMT_SBGGR8, 352, 288)) < 0)
	//if((fd = capture_init_device(dev_name, V4L2_PIX_FMT_MJPEG, 320, 240)) < 0)
		return -1;

	set_framerate(fd, 15);

	if(capture_start(fd) < 0)
		return -1;

        mainloop(fd);

	capture_stop(fd);

        capture_uninit_device(fd);

        return 0;
}
