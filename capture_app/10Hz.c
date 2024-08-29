// make clean_10Hz && make 10Hz && sudo truncate -s 0 /var/log/syslog && ./10Hz && uname -a > 10hz_syslog.txt && sudo grep -F "[10Hz]" /var/log/syslog >> 10hz_syslog.txt

// Linux raspberrypi 6.6.31+rpt-rpi-v8 #1 SMP PREEMPT Debian 1:6.6.31-1+rpt1 (2024-05-29) aarch64 GNU/Linux

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <time.h>
#include <limits.h>
#include <libgen.h>
#include <syslog.h>

// Macros to clear memory, set resolution, and define frame capture limits
#define CLEAR(x) memset(&(x), 0, sizeof(x))

#define HRES 640
#define VRES 480
#define HRES_STR "640"
#define VRES_STR "480"

// Frame capture configuration parameters
#define START_UP_FRAMES 8
#define LAST_FRAMES 1
#define CAPTURE_FRAMES (1800 + LAST_FRAMES)
#define FRAMES_TO_ACQUIRE (CAPTURE_FRAMES + START_UP_FRAMES + LAST_FRAMES)

#define FRAMES_PER_SEC 10   // Set to 10 FPS for the assignment requirements

#define DUMP_FRAMES

// Struct to hold video format details
static struct v4l2_format fmt;

// Enum for different I/O methods (read, memory-mapped, user pointer)
enum io_method { IO_METHOD_READ, IO_METHOD_MMAP, IO_METHOD_USERPTR };

// Struct to represent a buffer in memory for frame data
struct buffer {
    void   *start;
    size_t  length;
};

// Global variables for device name, file descriptors, buffers, and frame counts
static char *dev_name;
static enum io_method io = IO_METHOD_MMAP;
static int fd = -1;
static struct buffer *buffers;
static unsigned int n_buffers;
static int out_buf;
static int force_format = 1;
static int frame_count = FRAMES_TO_ACQUIRE;

// Timing-related variables for frame processing
static double fnow = 0.0, fstart = 0.0, fstop = 0.0;
static struct timespec time_now, time_start, time_stop;

// Frame counter and buffer for holding the frame data
int framecnt = -8;
unsigned char bigbuffer[(1280 * 960)];

// Function to handle errors and exit the program with an error message
static void errno_exit(const char *s) {
    syslog(LOG_ERR, "%s error %d, %s [10Hz]\n", s, errno, strerror(errno));
    fprintf(stderr, "%s error %d, %s [10Hz]\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

// Wrapper for the ioctl system call, which allows low-level control of the video device
static int xioctl(int fh, int request, void *arg) {
    int r;
    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);
    return r;
}

// Headers and file name templates for saving frames as PPM or PGM files
char ppm_header[] = "P6\n#9999999999 sec 9999999999 msec \n"HRES_STR" "VRES_STR"\n255\n";
char ppm_dumpname[PATH_MAX];
char pgm_header[] = "P5\n#9999999999 sec 9999999999 msec \n"HRES_STR" "VRES_STR"\n255\n";
char pgm_dumpname[PATH_MAX];

// Function to create a directory for saving frame images
int create_directory(const char *path) {
    struct stat st = {0};

    if (stat(path, &st) == -1) {
        if (mkdir(path, 0700) != 0) {
            perror("Failed to create frames directory");
            return -1;
        }
    }

    return 0;
}

// Function to set the output directory for saving frame images
void set_output_directory(const char *dir) {
    snprintf(ppm_dumpname, PATH_MAX, "%s/test0000.ppm", dir);
    snprintf(pgm_dumpname, PATH_MAX, "%s/test0000.pgm", dir);
}

// Function to save a frame in PPM format (used for RGB images)
static void dump_ppm(const void *p, int size, unsigned int tag, struct timespec *time) {
    int written, total, dumpfd;
    snprintf(&ppm_dumpname[strlen(ppm_dumpname) - 8], 9, "%04d.ppm", tag);
    dumpfd = open(ppm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 0644);

    if (dumpfd == -1) {
        syslog(LOG_ERR, "Failed to open ppm file %s: %s [10Hz]\n", ppm_dumpname, strerror(errno));
        perror("Failed to open ppm file [10Hz]");
        return;
    }

    // Add the timestamp to the PPM header
    snprintf(&ppm_header[4], 11, "%010d", (int)time->tv_sec);
    snprintf(&ppm_header[19], 11, "%010d", (int)((time->tv_nsec)/1000000));

    // Write the PPM header to the file
    written = write(dumpfd, ppm_header, sizeof(ppm_header) - 1);
    if (written == -1) {
        syslog(LOG_ERR, "Failed to write ppm header: %s [10Hz]\n", strerror(errno));
        perror("Failed to write ppm header [10Hz]");
        close(dumpfd);
        return;
    }

    // Write the frame data to the file
    total = 0;
    do {
        written = write(dumpfd, p, size);
        if (written == -1) {
            syslog(LOG_ERR, "Failed to write ppm data: %s [10Hz]\n", strerror(errno));
            perror("Failed to write ppm data [10Hz]");
            close(dumpfd);
            return;
        }
        total += written;
    } while (total < size);

    // Log the time at which the frame was written
    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
    syslog(LOG_INFO, "[Course #:4] [Final Project] [Frame Count: %d] [Image Capture Start Time: %lf seconds] PPM frame written to %s at %lf, %d bytes [10Hzgrep]\n", framecnt, fnow - fstart, ppm_dumpname, (fnow - fstart), total);

    close(dumpfd);
}

// Function to save a frame in PGM format (used for grayscale images)
static void dump_pgm(const void *p, int size, unsigned int tag, struct timespec *time) {
    int written, total, dumpfd;
    snprintf(&pgm_dumpname[strlen(pgm_dumpname) - 8], 9, "%04d.pgm", tag);
    dumpfd = open(pgm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 0644);

    if (dumpfd == -1) {
        syslog(LOG_ERR, "Failed to open pgm file %s: %s [10Hz]\n", pgm_dumpname, strerror(errno));
        perror("Failed to open pgm file [10Hz]");
        return;
    }

    // Add the timestamp to the PGM header
    snprintf(&pgm_header[4], 11, "%010d", (int)time->tv_sec);
    snprintf(&pgm_header[19], 11, "%010d", (int)((time->tv_nsec)/1000000));

    // Write the PGM header to the file
    written = write(dumpfd, pgm_header, sizeof(pgm_header) - 1);
    if (written == -1) {
        syslog(LOG_ERR, "Failed to write pgm header: %s [10Hz]\n", strerror(errno));
        perror("Failed to write pgm header [10Hz]");
        close(dumpfd);
        return;
    }

    // Write the frame data to the file
    total = 0;
    do {
        written = write(dumpfd, p, size);
        if (written == -1) {
            syslog(LOG_ERR, "Failed to write pgm data: %s [10Hz]\n", strerror(errno));
            perror("Failed to write pgm data [10Hz]");
            close(dumpfd);
            return;
        }
        total += written;
    } while (total < size);

    // Log the time at which the frame was written
    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
    syslog(LOG_INFO, "[Course #:4] [Final Project] [Frame Count: %d] [Image Capture Start Time: %lf seconds] PGM frame written to %s at %lf, %d bytes [10Hz]\n", framecnt, fnow - fstart, pgm_dumpname, (fnow - fstart), total);

    close(dumpfd);
}

// Function to convert YUV format to RGB (used for processing frames from the camera)
void yuv2rgb(int y, int u, int v, unsigned char *r, unsigned char *g, unsigned char *b) {
   int r1, g1, b1;
   int c = y - 16, d = u - 128, e = v - 128;

   r1 = (298 * c           + 409 * e + 128) >> 8;
   g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
   b1 = (298 * c + 516 * d           + 128) >> 8;

   *r = (r1 > 255) ? 255 : (r1 < 0) ? 0 : r1;
   *g = (g1 > 255) ? 255 : (g1 < 0) ? 0 : g1;
   *b = (b1 > 255) ? 255 : (b1 < 0) ? 0 : b1;
}

// Function to process each captured frame, including saving to file and converting formats
static void process_image(const void *p, int size) {
    int i, newi;
    struct timespec frame_time;
    unsigned char *pptr = (unsigned char *)p;

    clock_gettime(CLOCK_REALTIME, &frame_time);    

    framecnt++;
    syslog(LOG_INFO, "Processing frame %d with size %d [10Hz]\n", framecnt, size);

    if (framecnt == 0) {
        clock_gettime(CLOCK_MONOTONIC, &time_start);
        fstart = (double)time_start.tv_sec + (double)time_start.tv_nsec / 1000000000.0;
    }

#ifdef DUMP_FRAMES
    // Save frames based on their format (GRAY, YUYV, RGB)
    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY) {
        dump_pgm(p, size, framecnt, &frame_time);
    } else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV) {
        for (i = 0, newi = 0; i < size; i += 4, newi += 2) {
            bigbuffer[newi] = pptr[i];
            bigbuffer[newi + 1] = pptr[i + 2];
        }
        if (framecnt > -1) {
            dump_pgm(bigbuffer, (size / 2), framecnt, &frame_time);
        }
    } else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24) {
        dump_ppm(p, size, framecnt, &frame_time);
    } else {
        syslog(LOG_ERR, "ERROR - unknown dump format [10Hz]\n");
    }
#endif
}

// Function to read a single frame of video data and process it
static int read_frame(void) {
    struct v4l2_buffer buf;
    unsigned int i;

    switch (io) {
        case IO_METHOD_READ:
            if (-1 == read(fd, buffers[0].start, buffers[0].length)) {
                switch (errno) {
                    case EAGAIN:
                        return 0;
                    case EIO:
                    default:
                        errno_exit("read");
                }
            }
            process_image(buffers[0].start, buffers[0].length);
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
                    default:
                        syslog(LOG_ERR, "mmap failure [10Hz]\n");
                        errno_exit("VIDIOC_DQBUF");
                }
            }

            assert(buf.index < n_buffers);
            process_image(buffers[buf.index].start, buf.bytesused);

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
                    default:
                        errno_exit("VIDIOC_DQBUF");
                }
            }

            for (i = 0; i < n_buffers; ++i)
                if (buf.m.userptr == (unsigned long)buffers[i].start && buf.length == buffers[i].length)
                    break;

            assert(i < n_buffers);
            process_image((void *)buf.m.userptr, buf.bytesused);

            if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                errno_exit("VIDIOC_QBUF");
            break;
    }

    return 1;
}

// Main loop for capturing and processing frames
static void mainloop(void) {
    unsigned int count;
    struct timespec read_delay;
    struct timespec time_error;

    // Configure the read delay based on the desired frames per second
    printf("Running at 10 frames/sec\n");
    read_delay.tv_sec = 0;
    read_delay.tv_nsec = 100000000; // 10 Hz

    count = frame_count;

    // Main loop for capturing frames and handling I/O
    while (count > 0) {
        for (;;) {
            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r) {
                if (EINTR == errno)
                    continue;
                errno_exit("select");
            }

            if (0 == r) {
                syslog(LOG_ERR, "select timeout [10Hz]\n");
                exit(EXIT_FAILURE);
            }

            if (read_frame()) {
                if (nanosleep(&read_delay, &time_error) != 0)
                    perror("nanosleep");
                else {
                    if (framecnt > 1) {
                        clock_gettime(CLOCK_MONOTONIC, &time_now);
                        fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
                        syslog(LOG_INFO, "Frame %d read at %lf, @ %lf FPS [10Hz]\n", framecnt, (fnow - fstart), (double)(framecnt + 1) / (fnow - fstart));
                    } else {
                        syslog(LOG_INFO, "Initial frame read at %lf [10Hz]\n", fnow);
                    }
                }

                count--;
                break;
            }

            if (count <= 0) break;
        }

        if (count <= 0) break;
    }

    // Record the end time for the capture
    clock_gettime(CLOCK_MONOTONIC, &time_stop);
    fstop = (double)time_stop.tv_sec + (double)time_stop.tv_nsec / 1000000000.0;
    syslog(LOG_INFO, "Capture ended, total capture time=%lf, for %d frames, %lf FPS [10Hz]\n", (fstop - fstart), CAPTURE_FRAMES + 1, ((double)CAPTURE_FRAMES / (fstop - fstart)));
}

// Function to stop video capturing by turning off the video stream
static void stop_capturing(void) {
    enum v4l2_buf_type type;

    switch (io) {
        case IO_METHOD_READ:
            break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
                errno_exit("VIDIOC_STREAMOFF");
            break;
    }
}

// Function to start video capturing by turning on the video stream
static void start_capturing(void) {
    unsigned int i;
    enum v4l2_buf_type type;

    switch (io) {
        case IO_METHOD_READ:
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

// Function to uninitialize and release the device resources
static void uninit_device(void) {
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

// Function to initialize the device in read mode
static void init_read(unsigned int buffer_size) {
    buffers = calloc(1, sizeof(*buffers));

    if (!buffers) {
        syslog(LOG_ERR, "Out of memory [10Hz]\n");
        exit(EXIT_FAILURE);
    }

    buffers[0].length = buffer_size;
    buffers[0].start = malloc(buffer_size);

    if (!buffers[0].start) {
        syslog(LOG_ERR, "Out of memory [10Hz]\n");
        exit(EXIT_FAILURE);
    }
}

// Function to initialize the device in memory-mapped mode
static void init_mmap(void) {
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            syslog(LOG_ERR, "%s does not support memory mapping [10Hz]\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2) {
        syslog(LOG_ERR, "Insufficient buffer memory on %s [10Hz]\n", dev_name);
        exit(EXIT_FAILURE);
    }

    buffers = calloc(req.count, sizeof(*buffers));

    if (!buffers) {
        syslog(LOG_ERR, "Out of memory [10Hz]\n");
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
        buffers[n_buffers].start =
            mmap(NULL /* start anywhere */,
                 buf.length,
                 PROT_READ | PROT_WRITE /* required */,
                 MAP_SHARED /* recommended */,
                 fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
            errno_exit("mmap");
    }
}

// Function to initialize the device in user pointer mode
static void init_userp(unsigned int buffer_size) {
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            syslog(LOG_ERR, "%s does not support user pointer I/O [10Hz]\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    buffers = calloc(4, sizeof(*buffers));

    if (!buffers) {
        syslog(LOG_ERR, "Out of memory [10Hz]\n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
        buffers[n_buffers].length = buffer_size;
        buffers[n_buffers].start = malloc(buffer_size);

        if (!buffers[n_buffers].start) {
            syslog(LOG_ERR, "Out of memory [10Hz]\n");
            exit(EXIT_FAILURE);
        }
    }
}

// Function to initialize the video capture device, including setting format and buffer allocation
static void init_device(void) {
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    unsigned int min;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            syslog(LOG_ERR, "%s is no V4L2 device [10Hz]\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        syslog(LOG_ERR, "%s is no video capture device [10Hz]\n", dev_name);
        exit(EXIT_FAILURE);
    }

    switch (io) {
        case IO_METHOD_READ:
            if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
                syslog(LOG_ERR, "%s does not support read I/O [10Hz]\n", dev_name);
                exit(EXIT_FAILURE);
            }
            break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
            if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                syslog(LOG_ERR, "%s does not support streaming I/O [10Hz]\n", dev_name);
                exit(EXIT_FAILURE);
            }
            break;
    }

    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect;

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
            switch (errno) {
                case EINVAL:
                    break;
                default:
                    break;
            }
        }
    }

    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (force_format) {
        syslog(LOG_INFO, "FORCING FORMAT [10Hz]\n");
        fmt.fmt.pix.width = HRES;
        fmt.fmt.pix.height = VRES;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
            errno_exit("VIDIOC_S_FMT");
    } else {
        syslog(LOG_INFO, "ASSUMING FORMAT [10Hz]\n");
        if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
            errno_exit("VIDIOC_G_FMT");
    }

    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    switch (io) {
        case IO_METHOD_READ:
            init_read(fmt.fmt.pix.sizeimage);
            break;

        case IO_METHOD_MMAP:
            init_mmap();
            break;

        case IO_METHOD_USERPTR:
            init_userp(fmt.fmt.pix.sizeimage);
            break;
    }
}

// Function to close the video capture device
static void close_device(void) {
    if (-1 == close(fd))
        errno_exit("close");

    fd = -1;
}

// Function to open the video capture device
static void open_device(void) {
    struct stat st;

    if (-1 == stat(dev_name, &st)) {
        syslog(LOG_ERR, "Cannot identify '%s': %d, %s [10Hz]\n", dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode)) {
        syslog(LOG_ERR, "%s is no device [10Hz]\n", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);

    if (-1 == fd) {
        syslog(LOG_ERR, "Cannot open '%s': %d, %s [10Hz]\n", dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

// Function to print usage information for the program
static void usage(FILE *fp, int argc, char **argv) {
    fprintf(fp,
             "Usage: %s [options]\n\n"
             "Version 1.3\n"
             "Options:\n"
             "-d | --device name   Video device name [%s]\n"
             "-h | --help          Print this message\n"
             "-m | --mmap          Use memory-mapped buffers [default]\n"
             "-r | --read          Use read() calls\n"
             "-u | --userp         Use application-allocated buffers\n"
             "-o | --output        Outputs stream to stdout\n"
             "-f | --format        Force format to 640x480 GREY\n"
             "-c | --count         Number of frames to grab [%i]\n",
             argv[0], dev_name, frame_count);
}

// Options for the program, defining short and long options
static const char short_options[] = "d:hmruofc:";
static const struct option long_options[] = {
    { "device", required_argument, NULL, 'd' },
    { "help",   no_argument,       NULL, 'h' },
    { "mmap",   no_argument,       NULL, 'm' },
    { "read",   no_argument,       NULL, 'r' },
    { "userp",  no_argument,       NULL, 'u' },
    { "output", no_argument,       NULL, 'o' },
    { "format", no_argument,       NULL, 'f' },
    { "count",  required_argument, NULL, 'c' },
    { 0, 0, 0, 0 }
};

// Main function to parse command-line arguments, initialize and run the video capture
int main(int argc, char **argv) {
    char exec_path[PATH_MAX];
    char *exec_dir;
    char frames_dir[PATH_MAX];

    // Open syslog for debugging and set log file
    openlog("capture_app", LOG_PID | LOG_CONS, LOG_USER);
    setlogmask(LOG_UPTO(LOG_INFO));

    FILE *log_file = fopen("10hz_syslog.txt", "w");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }
    dup2(fileno(log_file), fileno(stdout));
    dup2(fileno(log_file), fileno(stderr));

    syslog(LOG_INFO, "Starting capture application [10Hz]\n");

    // Log uname output as required
    char uname_buffer[256];
    FILE *uname_pipe = popen("uname -a", "r");
    if (uname_pipe != NULL) {
        fgets(uname_buffer, sizeof(uname_buffer), uname_pipe);
        pclose(uname_pipe);
        syslog(LOG_INFO, "%s [10Hz]\n", uname_buffer);
    }

    // Determine the directory where the executable is located and set the output directory for frames
    if (readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1) != -1) {
        exec_dir = dirname(exec_path);
        snprintf(frames_dir, sizeof(frames_dir), "%s/frames10hz", exec_dir);
        set_output_directory(frames_dir);
        syslog(LOG_INFO, "Output directory set to %s [10Hz]\n", frames_dir);
    } else {
        syslog(LOG_ERR, "Failed to determine executable path [10Hz]\n");
        exit(EXIT_FAILURE);
    }

    // Create the directory for saving frames
    if (create_directory(frames_dir) != 0) {
        syslog(LOG_ERR, "Failed to create directory %s [10Hz]\n", frames_dir);
        exit(EXIT_FAILURE);
    }

    // Parse command-line arguments to configure the device and capture settings
    if (argc > 1)
        dev_name = argv[1];
    else
        dev_name = "/dev/video0";

    for (;;) {
        int idx;
        int c;

        c = getopt_long(argc, argv, short_options, long_options, &idx);

        if (-1 == c)
            break;

        switch (c) {
            case 0:
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

            case 'o':
                out_buf++;
                break;

            case 'f':
                force_format++;
                break;

            case 'c':
                errno = 0;
                frame_count = strtol(optarg, NULL, 0);
                if (errno)
                    errno_exit(optarg);
                break;

            default:
                usage(stderr, argc, argv);
                exit(EXIT_FAILURE);
        }
    }

    // Initialize the device, start capturing, and run the main loop
    open_device();
    init_device();

    start_capturing();
    mainloop();

    stop_capturing();

    // Print the total capture time and frames per second (FPS)
    syslog(LOG_INFO, "Total capture time=%lf, for %d frames, %lf FPS [10Hz]\n", (fstop - fstart), CAPTURE_FRAMES + 1, ((double)CAPTURE_FRAMES / (fstop - fstart)));
    printf("Total capture time=%lf, for %d frames, %lf FPS [10Hz]\n", (fstop - fstart), CAPTURE_FRAMES + 1, ((double)CAPTURE_FRAMES / (fstop - fstart)));

    // Uninitialize and close the device
    uninit_device();
    close_device();
    fprintf(stderr, "\n");

    // Close syslog
    syslog(LOG_INFO, "Capture application finished [10Hz]\n");
    closelog();
    fclose(log_file);

    return 0;
}
