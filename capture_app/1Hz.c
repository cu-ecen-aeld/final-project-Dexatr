// make clean_1Hz && make 1Hz && sudo truncate -s 0 /var/log/syslog && ./1Hz && uname -a > 1hz_syslog.txt && sudo grep -F "[1Hz]" /var/log/syslog >> 1hz_syslog.txt

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

// Macro to clear memory of a given variable or structure
#define CLEAR(x) memset(&(x), 0, sizeof(x))

// Resolution for the video frames to be captured
#define HRES 640
#define VRES 480
#define HRES_STR "640"
#define VRES_STR "480"

// Frame capture settings
#define START_UP_FRAMES 8  // Initial frames to skip
#define LAST_FRAMES 1  // Last frame to capture
#define CAPTURE_FRAMES (180 + LAST_FRAMES) // Total frames for 1Hz capture (180 seconds + 1 last frame)
#define FRAMES_TO_ACQUIRE (CAPTURE_FRAMES + START_UP_FRAMES + LAST_FRAMES)  // Total frames to acquire, including startup and last frames

#define FRAMES_PER_SEC 1   // Frame rate set to 1 FPS

#define DUMP_FRAMES  // Enable frame dumping (saving)

static struct v4l2_format fmt;  // Structure to store video format information

// Enumeration to define I/O methods: Read, Memory Map, or User Pointer
enum io_method { IO_METHOD_READ, IO_METHOD_MMAP, IO_METHOD_USERPTR };

// Structure to represent a buffer used for frame data
struct buffer {
    void   *start;  // Start of the buffer memory
    size_t  length; // Length of the buffer
};

// Global variables
static char *dev_name;  // Device name (e.g., /dev/video0)
static enum io_method io = IO_METHOD_MMAP;  // Default I/O method (memory-mapped I/O)
static int fd = -1;  // File descriptor for the video device
static struct buffer *buffers;  // Array of buffers for frame data
static unsigned int n_buffers;  // Number of buffers
static int out_buf;  // Output buffer flag
static int force_format = 1;  // Force format flag
static int frame_count = FRAMES_TO_ACQUIRE;  // Total number of frames to acquire

// Timing variables for frame capture
static double fnow = 0.0, fstart = 0.0, fstop = 0.0;
static struct timespec time_now, time_start, time_stop;
static int framecnt = -8;  // Frame counter, starting before zero to skip initial frames

// Function to handle and log errors, then exit the program
static void errno_exit(const char *s) {
    syslog(LOG_ERR, "%s error %d, %s [1Hz]\n", s, errno, strerror(errno));
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    fflush(stderr);
    exit(EXIT_FAILURE);
}

// Function to make ioctl system calls, handling any interruptions
static int xioctl(int fh, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);
    return r;
}

// Header templates for PPM (color) and PGM (grayscale) image files
char ppm_header[] = "P6\n#9999999999 sec 9999999999 msec \n"HRES_STR" "VRES_STR"\n255\n";
char ppm_dumpname[PATH_MAX];
char pgm_header[] = "P5\n#9999999999 sec 9999999999 msec \n"HRES_STR" "VRES_STR"\n255\n";
char pgm_dumpname[PATH_MAX];

// Function to create a directory if it doesn't exist
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

// Function to set the output directory for saving captured frames
void set_output_directory(const char *dir) {
    snprintf(ppm_dumpname, PATH_MAX, "%s/test0000.ppm", dir);
    snprintf(pgm_dumpname, PATH_MAX, "%s/test0000.pgm", dir);
}

// Function to save a frame as a PPM (color) file
static void dump_ppm(const void *p, int size, unsigned int tag, struct timespec *time) {
    int written, total, dumpfd;
    snprintf(&ppm_dumpname[strlen(ppm_dumpname) - 8], 9, "%04d.ppm", tag);
    dumpfd = open(ppm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 0644);

    if (dumpfd == -1) {
        syslog(LOG_ERR, "Failed to open ppm file %s: %s [1Hz]\n", ppm_dumpname, strerror(errno));
        perror("Failed to open ppm file");
        return;
    }

    // Add timestamp to the PPM header
    snprintf(&ppm_header[4], 11, "%010d", (int)time->tv_sec);
    snprintf(&ppm_header[19], 11, "%010d", (int)((time->tv_nsec)/1000000));

    // Write the PPM header to the file
    written = write(dumpfd, ppm_header, sizeof(ppm_header) - 1);
    if (written == -1) {
        syslog(LOG_ERR, "Failed to write ppm header: %s [1Hz]\n", strerror(errno));
        perror("Failed to write ppm header");
        close(dumpfd);
        return;
    }

    // Write the frame data to the file
    total = 0;
    do {
        written = write(dumpfd, p, size);
        if (written == -1) {
            syslog(LOG_ERR, "Failed to write ppm data: %s [1Hz]\n", strerror(errno));
            perror("Failed to write ppm data");
            close(dumpfd);
            return;
        }
        total += written;
    } while (total < size);

    // Log the time when the frame was written
    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
    syslog(LOG_INFO, "[Course #:4] [Final Project] [Frame Count: %d] [Image Capture Start Time: %lf seconds] PPM frame written to %s at %lf, %d bytes [1Hz grep]\n", framecnt, fnow - fstart, ppm_dumpname, (fnow - fstart), total);

    close(dumpfd);
}

// Function to save a frame as a PGM (grayscale) file
static void dump_pgm(const void *p, int size, unsigned int tag, struct timespec *time) {
    int written, total, dumpfd;
    snprintf(&pgm_dumpname[strlen(pgm_dumpname) - 8], 9, "%04d.pgm", tag);
    dumpfd = open(pgm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 0644);

    if (dumpfd == -1) {
        syslog(LOG_ERR, "Failed to open pgm file %s: %s [1Hz]\n", pgm_dumpname, strerror(errno));
        perror("Failed to open pgm file");
        return;
    }

    // Add timestamp to the PGM header
    snprintf(&pgm_header[4], 11, "%010d", (int)time->tv_sec);
    snprintf(&pgm_header[19], 11, "%010d", (int)((time->tv_nsec)/1000000));

    // Write the PGM header to the file
    written = write(dumpfd, pgm_header, sizeof(pgm_header) - 1);
    if (written == -1) {
        syslog(LOG_ERR, "Failed to write pgm header: %s [1Hz]\n", strerror(errno));
        perror("Failed to write pgm header");
        close(dumpfd);
        return;
    }

    // Write the frame data to the file
    total = 0;
    do {
        written = write(dumpfd, p, size);
        if (written == -1) {
            syslog(LOG_ERR, "Failed to write pgm data: %s [1Hz]\n", strerror(errno));
            perror("Failed to write pgm data");
            close(dumpfd);
            return;
        }
        total += written;
    } while (total < size);

    // Log the time when the frame was written
    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
    syslog(LOG_INFO, "[Course #:4] [Final Project] [Frame Count: %d] [Image Capture Start Time: %lf seconds] PGM frame written to %s at %lf, %d bytes [1Hz]\n", framecnt, fnow - fstart, pgm_dumpname, (fnow - fstart), total);

    close(dumpfd);
}

// Function to convert YUV format to RGB format
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

// Buffer to store larger frames for processing
unsigned char bigbuffer[(1280 * 960)];

// Function to process captured frames and save them to a file
static void process_image(const void *p, int size) {
    int i, newi;
    struct timespec frame_time;
    unsigned char *pptr = (unsigned char *)p;

    clock_gettime(CLOCK_REALTIME, &frame_time);    

    framecnt++;  // Increment frame counter
    syslog(LOG_INFO, "Processing frame %d with size %d [1Hz]\n", framecnt, size);

    // Record the start time on the first frame
    if (framecnt == 0) {
        clock_gettime(CLOCK_MONOTONIC, &time_start);
        fstart = (double)time_start.tv_sec + (double)time_start.tv_nsec / 1000000000.0;
    }

#ifdef DUMP_FRAMES
    // Check the pixel format and save the frame accordingly
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
        syslog(LOG_ERR, "ERROR - unknown dump format [1Hz]\n");
    }
#endif
}

// Function to read a single frame from the video device
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
                        syslog(LOG_ERR, "mmap failure [1Hz]\n");
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

// Main loop to capture and process frames
static void mainloop(void) {
    unsigned int count;
    struct timespec read_delay;
    struct timespec time_error;

    printf("Running at 1 frame/sec\n");
    read_delay.tv_sec = 1;
    read_delay.tv_nsec = 0; // 1 Hz delay between frames

    count = frame_count;

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
                syslog(LOG_ERR, "select timeout [1Hz]\n");
                exit(EXIT_FAILURE);
            }

            if (read_frame()) {
                if (nanosleep(&read_delay, &time_error) != 0)
                    perror("nanosleep");
                else {
                    if (framecnt > 1) {
                        clock_gettime(CLOCK_MONOTONIC, &time_now);
                        fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
                        syslog(LOG_INFO, "Frame %d read at %lf, @ %lf FPS [1Hz]\n", framecnt, (fnow - fstart), (double)(framecnt + 1) / (fnow - fstart));
                    } else {
                        syslog(LOG_INFO, "Initial frame read at %lf [1Hz]\n", fnow);
                    }
                }

                count--;
                break;
            }

            if (count <= 0) break;
        }

        if (count <= 0) break;
    }

    // Record and log the end time after capturing all frames
    clock_gettime(CLOCK_MONOTONIC, &time_stop);
    fstop = (double)time_stop.tv_sec + (double)time_stop.tv_nsec / 1000000000.0;
    syslog(LOG_INFO, "Capture ended, total capture time=%lf, for %d frames, %lf FPS [1Hz]\n", (fstop - fstart), CAPTURE_FRAMES + 1, ((double)CAPTURE_FRAMES / (fstop - fstart)));
}

// Function to stop video capturing
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

// Function to start video capturing
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

// Function to uninitialize and release the video device resources
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
        syslog(LOG_ERR, "Out of memory [1Hz]\n");
        exit(EXIT_FAILURE);
    }

    buffers[0].length = buffer_size;
    buffers[0].start = malloc(buffer_size);

    if (!buffers[0].start) {
        syslog(LOG_ERR, "Out of memory [1Hz]\n");
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
            syslog(LOG_ERR, "%s does not support memory mapping [1Hz]\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2) {
        syslog(LOG_ERR, "Insufficient buffer memory on %s [1Hz]\n", dev_name);
        exit(EXIT_FAILURE);
    }

    buffers = calloc(req.count, sizeof(*buffers));

    if (!buffers) {
        syslog(LOG_ERR, "Out of memory [1Hz]\n");
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
            syslog(LOG_ERR, "%s does not support user pointer I/O [1Hz]\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    buffers = calloc(4, sizeof(*buffers));

    if (!buffers) {
        syslog(LOG_ERR, "Out of memory [1Hz]\n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
        buffers[n_buffers].length = buffer_size;
        buffers[n_buffers].start = malloc(buffer_size);

        if (!buffers[n_buffers].start) {
            syslog(LOG_ERR, "Out of memory [1Hz]\n");
            exit(EXIT_FAILURE);
        }
    }
}

// Function to initialize the video capture device, including setting the format and buffer allocation
static void init_device(void) {
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    unsigned int min;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            syslog(LOG_ERR, "%s is no V4L2 device [1Hz]\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        syslog(LOG_ERR, "%s is no video capture device [1Hz]\n", dev_name);
        exit(EXIT_FAILURE);
    }

    switch (io) {
        case IO_METHOD_READ:
            if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
                syslog(LOG_ERR, "%s does not support read I/O [1Hz]\n", dev_name);
                exit(EXIT_FAILURE);
            }
            break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
            if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                syslog(LOG_ERR, "%s does not support streaming I/O [1Hz]\n", dev_name);
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
        syslog(LOG_INFO, "FORCING FORMAT [1Hz]\n");
        fmt.fmt.pix.width = HRES;
        fmt.fmt.pix.height = VRES;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
            errno_exit("VIDIOC_S_FMT");
    } else {
        syslog(LOG_INFO, "ASSUMING FORMAT [1Hz]\n");
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
        syslog(LOG_ERR, "Cannot identify '%s': %d, %s [1Hz]\n", dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode)) {
        syslog(LOG_ERR, "%s is no device [1Hz]\n", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);

    if (-1 == fd) {
        syslog(LOG_ERR, "Cannot open '%s': %d, %s [1Hz]\n", dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

// Function to print the usage information for the program
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

    FILE *log_file = fopen("1hz_syslog.txt", "a");
    if (log_file) {
        int log_fd = fileno(log_file);
        dup2(log_fd, STDERR_FILENO); // Redirect syslog output to file
    } else {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Starting capture application [1Hz]\n");

    // Determine the directory where the executable is located and set the output directory for frames
    if (readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1) != -1) {
        exec_dir = dirname(exec_path);
        snprintf(frames_dir, sizeof(frames_dir), "%s/frames1hz", exec_dir);
        set_output_directory(frames_dir);
        syslog(LOG_INFO, "Output directory set to %s [1Hz]\n", frames_dir);
    } else {
        syslog(LOG_ERR, "Failed to determine executable path [1Hz]\n");
        exit(EXIT_FAILURE);
    }

    // Create the directory for saving frames
    if (create_directory(frames_dir) != 0) {
        syslog(LOG_ERR, "Failed to create directory %s [1Hz]\n", frames_dir);
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

    // Capture and log system information using uname
    char uname_buffer[256];
    FILE *uname_pipe = popen("uname -a", "r");
    if (uname_pipe != NULL) {
        fgets(uname_buffer, sizeof(uname_buffer), uname_pipe);
        pclose(uname_pipe);
        syslog(LOG_INFO, "%s [1Hz]", uname_buffer);
    }

    // Initialize the device, start capturing, and run the main loop
    open_device();
    init_device();

    start_capturing();
    mainloop();

    stop_capturing();

    // Print the total capture time and frames per second (FPS)
    syslog(LOG_INFO, "Total capture time=%lf, for %d frames, %lf FPS [1Hz]\n", (fstop - fstart), CAPTURE_FRAMES + 1, ((double)CAPTURE_FRAMES / (fstop - fstart)));
    printf("Total capture time=%lf, for %d frames, %lf FPS\n", (fstop - fstart), CAPTURE_FRAMES + 1, ((double)CAPTURE_FRAMES / (fstop - fstart)));

    // Uninitialize and close the device
    uninit_device();
    close_device();
    fprintf(stderr, "\n");

    // Close syslog
    syslog(LOG_INFO, "Capture application finished [1Hz]\n");
    fclose(log_file);
    closelog();

    return 0;
}
