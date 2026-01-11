/*
 * fb0rfb.c — Minimal framebuffer-to-VNC (RFB) server for OpenCentauri / ECC
 *
 * What it does
 * ------------
 * - Opens the Linux framebuffer device (default: /dev/fb0) READ-ONLY.
 * - Memory-maps the framebuffer so it can copy pixels efficiently.
 * - Listens on TCP port 5900 (default) and speaks the RFB 3.8 protocol (VNC).
 * - Serves a single client at a time (listen backlog=1), sending RAW pixel data.
 *
 * Why is it written this way
 * -------------------------
 * - The Elegoo Centauri Carbon UI renders directly to /dev/fb0 (no X11/Wayland).
 * - We avoid heavy dependencies and avoid writing to the framebuffer.
 * - We cap FPS (default 3, max 15) to reduce CPU/network load and avoid impacting printing.
 *
 * What it does NOT do (current limitations)
 * -----------------------------------------
 * - No input injection (keyboard/mouse/touch). We only parse & ignore input-related messages.
 * - No authentication / encryption (SecurityType = "None").
 * - No advanced encodings (only RAW).
 * - No incremental updates / dirty-rect tracking (always sends a full-frame update).
 *
 * Notes on pixel format
 * ---------------------
 * We assume 32bpp framebuffer and expose an RFB PixelFormat that matches common little-endian
 * ARGB/XRGB layouts where R is in bits 16..23, G in 8..15, B in 0..7, depth=24.
 *
 * Centauri Carbon screen specs:
 *   virtual_size: 480,544
 *   bits_per_pixel: 32
 *   stride: 1920  (== 480 * 4 bytes)
 *
 * That matches width=480 and line_length (stride)=1920 bytes per scanline.
 *
 * If your device uses a different channel order (e.g., BGRA), colors may appear swapped.
 * You can fix that by changing the PixelFormat shifts or swizzling during copy.
 */

#define _GNU_SOURCE

/* Networking / sockets */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* Linux framebuffer ioctls */
#include <linux/fb.h>
#include <sys/ioctl.h>

/* Memory mapping & I/O primitives */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* Print perror() and exit. Used for fatal setup errors. */
static void die(const char* msg) {
    perror(msg);
    exit(1);
}

/*
 * write_all() — reliably write exactly len bytes to fd
 *
 * - Loops until all bytes are written.
 * - Handles EINTR (signal interruption) by retrying.
 * - Returns 0 on success, -1 on failure (broken pipe, etc).
 *
 * This matters because TCP writes can write fewer bytes than requested.
 */
static int write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * read_all() — reliably read exactly len bytes from fd
 *
 * - Loops until all bytes are read.
 * - Handles EINTR by retrying.
 * - Returns 0 on success, -1 on EOF / error.
 *
 * This matters because TCP reads can return fewer bytes than requested.
 */
static int read_all(int fd, void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* peer closed connection */
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * msleep() — simple millisecond sleep using select()
 *
 * select() with no fds is a common portable trick for sub-second sleeps.
 */
static void msleep(int ms) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(0, NULL, NULL, NULL, &tv);
}

int main(int argc, char** argv) {
    /* Defaults chosen to match typical VNC usage + your printer environment */
    const char* fbpath = "/dev/fb0";
    int port = 5900;
    int fps = 3;

    /*
     * Parse basic CLI options:
     *   -f /dev/fb0   framebuffer device path
     *   -p 5900       TCP port
     *   --fps 3       frames per second cap
     */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) fbpath = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else {
            fprintf(stderr, "Usage: %s [-f /dev/fb0] [-p 5900] [--fps 3]\n", argv[0]);
            return 2;
        }
    }

    /* Enforce sane bounds to keep it "resource-safe" on an embedded printer */
    if (fps < 1) fps = 1;
    if (fps > 15) fps = 15; /* hard cap to stay resource-safe */

    /*
     * Open framebuffer read-only
     *
     * IMPORTANT: Using O_RDONLY ensures we never write to the framebuffer.
     * Some systems require root or framebuffer group permissions.
     */
    int fb = open(fbpath, O_RDONLY);
    if (fb < 0) die("open fb");

    /*
     * Query framebuffer info:
     * - fb_var_screeninfo: variable params like xres/yres/bpp
     * - fb_fix_screeninfo: fixed params like line_length (stride) and memory layout
     */
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &vinfo)) die("FBIOGET_VSCREENINFO");
    if (ioctl(fb, FBIOGET_FSCREENINFO, &finfo)) die("FBIOGET_FSCREENINFO");

    /* Visible resolution */
    int width  = (int)vinfo.xres;
    int height = (int)vinfo.yres;

    /* Bits-per-pixel and bytes-per-line */
    int bpp    = (int)vinfo.bits_per_pixel;
    int stride = (int)finfo.line_length;

    /*
     * This implementation assumes 32bpp.
     *
     * Why: we send 4 bytes per pixel (RFB 32bpp) and use memcpy() line copies
     * without conversion. Supporting other bpp values would require conversion.
     */
    if (bpp != 32) {
        fprintf(stderr, "Unsupported bpp=%d (expected 32)\n", bpp);
        return 3;
    }

    /*
     * Map framebuffer into memory.
     *
     * fbsize is based on stride*height, not width*height*4, because:
     * - Some framebuffers have padding per line (stride may be > width*bytespp).
     */
    size_t fbsize = (size_t)stride * (size_t)height;
    uint8_t* fbmem = mmap(NULL, fbsize, PROT_READ, MAP_SHARED, fb, 0);
    if (fbmem == MAP_FAILED) die("mmap fb");

    /*
     * Create listening socket
     *
     * AF_INET + SOCK_STREAM = IPv4 TCP.
     * We bind to 0.0.0.0 so it listens on all interfaces (LAN Wi-Fi/Ethernet).
     */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket");

    /* Allow quick restart if the port is in TIME_WAIT */
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    /* Bind to INADDR_ANY:port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(s, 1) < 0) die("listen");

    fprintf(stderr,
            "fb0rfb: listening on 0.0.0.0:%d, fb=%s (%dx%d@32bpp, stride=%d), fps=%d\n",
            port, fbpath, width, height, stride, fps);

    /*
     * Main accept loop
     *
     * Single-client design:
     * - accept() blocks until a client connects
     * - handle the client until disconnect
     * - then return to accept()
     */
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            die("accept");
        }

        /*
         * RFB Protocol handshake (VNC)
         *
         * Reference flow (simplified):
         * 1) Server -> Client: "RFB 003.008\n"
         * 2) Client -> Server: same format version
         * 3) Server -> Client: Security types (we offer "None" only)
         * 4) Client -> Server: chosen security type
         * 5) Server -> Client: SecurityResult (0 = OK)
         * 6) Client -> Server: ClientInit (shared flag)
         * 7) Server -> Client: ServerInit (w,h,pixfmt,name)
         */

        /* 1) Send protocol version */
        const char* ver = "RFB 003.008\n";
        if (write_all(c, ver, 12)) { close(c); continue; }

        /* 2) Read client protocol version (not validated beyond length) */
        char cver[12];
        if (read_all(c, cver, 12)) { close(c); continue; }

        /*
         * 3) Security handshake: "None" only
         *
         * For RFB 3.8, server sends:
         *   [number-of-types:1][type1:1]...[typen:1]
         * type 1 == "None"
         */
        uint8_t sec_types[2] = { 1, 1 }; /* 1 type: None */
        if (write_all(c, sec_types, 2)) { close(c); continue; }

        /* 4) Client chooses the security type */
        uint8_t chosen = 0;
        if (read_all(c, &chosen, 1)) { close(c); continue; }
        if (chosen != 1) { close(c); continue; } /* client didn't accept "None" */

        /* 5) SecurityResult: 4-byte status, 0 = OK */
        uint32_t ok = htonl(0);
        if (write_all(c, &ok, 4)) { close(c); continue; }

        /* 6) ClientInit: shared-flag (we ignore it) */
        uint8_t shared = 0;
        if (read_all(c, &shared, 1)) { close(c); continue; }

        /*
         * 7) ServerInit:
         * - width (u16)
         * - height (u16)
         * - PixelFormat (16 bytes)
         * - name length (u32)
         * - name string
         */
        uint16_t w = htons((uint16_t)width);
        uint16_t h = htons((uint16_t)height);

        /*
         * PixelFormat is exactly 16 bytes per RFB spec.
         * We pack it to guarantee layout on all compilers.
         *
         * We advertise:
         * - 32 bits per pixel (4 bytes)
         * - 24-bit "depth" (meaning only 24 meaningful color bits)
         * - little-endian (big_endian_flag=0)
         * - true color (true_color_flag=1)
         * - 8-bit per channel (max=255)
         * - shifts: R=16, G=8, B=0 (common XRGB/ARGB little-endian)
         */
        struct __attribute__((packed)) PixelFormat {
            uint8_t  bits_per_pixel;
            uint8_t  depth;
            uint8_t  big_endian_flag;
            uint8_t  true_color_flag;
            uint16_t red_max, green_max, blue_max;
            uint8_t  red_shift, green_shift, blue_shift;
            uint8_t  pad[3];
        } pf;

        memset(&pf, 0, sizeof(pf));
        pf.bits_per_pixel   = 32;
        pf.depth            = 24;
        pf.big_endian_flag  = 0;
        pf.true_color_flag  = 1;
        pf.red_max          = htons(255);
        pf.green_max        = htons(255);
        pf.blue_max         = htons(255);
        pf.red_shift        = 16;
        pf.green_shift      = 8;
        pf.blue_shift       = 0;

        const char* name = "OpenCentauri fb0 (RAW)";
        uint32_t namelen = htonl((uint32_t)strlen(name));

        if (write_all(c, &w, 2) ||
            write_all(c, &h, 2) ||
            write_all(c, &pf, sizeof(pf)) ||
            write_all(c, &namelen, 4) ||
            write_all(c, name, strlen(name))) {
            close(c);
            continue;
        }

        /*
         * Allocate a single scanline buffer.
         *
         * We copy line-by-line from the framebuffer to avoid:
         * - writing directly from mmap() memory into the socket (still possible),
         * - and to ensure we only send width*4 bytes (not stride bytes).
         *
         * Note: If stride == width*4 (typical), copying can be removed and we can
         * write directly from fbmem line pointer. But copy is safer if stride differs.
         */
        uint8_t* linebuf = (uint8_t*)malloc((size_t)width * 4);
        if (!linebuf) { close(c); continue; }

        /*
         * We don't start streaming frames until the client sends a
         * FramebufferUpdateRequest (msgtype 3). Many viewers expect to drive updates.
         */
        int client_ready = 0;

        /*
         * Client message loop
         *
         * We poll the socket without blocking (select timeout {0,0}) so that we can:
         * - read any pending client messages
         * - stream frames at a controlled FPS
         *
         * This is intentionally simple: we ignore most client messages.
         */
        for (;;) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(c, &rfds);
            struct timeval tv = {0, 0};

            int r = select(c + 1, &rfds, NULL, NULL, &tv);
            if (r < 0 && errno != EINTR) break;

            /*
             * If the client has sent data, read one RFB message and consume its payload.
             *
             * Client-to-server message types (subset):
             * 0: SetPixelFormat (we ignore; assume server format)
             * 2: SetEncodings   (we ignore; always RAW)
             * 3: FramebufferUpdateRequest (we use as "ready" signal)
             * 4: KeyEvent       (ignored)
             * 5: PointerEvent   (ignored)
             * 6: ClientCutText  (ignored)
             */
            if (r > 0 && FD_ISSET(c, &rfds)) {
                uint8_t msgtype;

                /* read() (not read_all) because we only need 1 byte here */
                if (read(c, &msgtype, 1) != 1) break;

                if (msgtype == 0) {
                    /* SetPixelFormat: 3 padding + 16-byte PixelFormat = 19 bytes remaining */
                    uint8_t rest[19];
                    if (read_all(c, rest, 19)) break;
                } else if (msgtype == 2) {
                    /*
                     * SetEncodings:
                     *   padding(1) + number-of-encodings(2) + encodings(4*count)
                     *
                     * We consume the list but ignore it because we always send RAW.
                     */
                    uint8_t pad;
                    uint16_t count;
                    if (read_all(c, &pad, 1)) break;
                    if (read_all(c, &count, 2)) break;
                    count = ntohs(count);

                    for (uint16_t i = 0; i < count; i++) {
                        uint32_t enc;
                        if (read_all(c, &enc, 4)) { count = 0; break; }
                        (void)enc; /* unused */
                    }
                } else if (msgtype == 3) {
                    /*
                     * FramebufferUpdateRequest:
                     *   incremental(1) + x(2) + y(2) + w(2) + h(2)
                     *
                     * We ignore the requested rectangle and always send full screen.
                     * We also ignore "incremental" (we always send full frame).
                     */
                    uint8_t inc;
                    uint16_t rx, ry, rw2, rh2;
                    if (read_all(c, &inc, 1)) break;
                    if (read_all(c, &rx, 2) ||
                        read_all(c, &ry, 2) ||
                        read_all(c, &rw2, 2) ||
                        read_all(c, &rh2, 2)) break;

                    (void)inc; (void)rx; (void)ry; (void)rw2; (void)rh2;

                    client_ready = 1;
                } else if (msgtype == 4) {
                    /* KeyEvent: down-flag(1) + pad(2) + key(4) = 7 bytes */
                    uint8_t rest[7];
                    if (read_all(c, rest, 7)) break;
                } else if (msgtype == 5) {
                    /* PointerEvent: button-mask(1) + x(2) + y(2) = 5 bytes */
                    uint8_t rest[5];
                    if (read_all(c, rest, 5)) break;
                } else if (msgtype == 6) {
                    /*
                     * ClientCutText:
                     *   pad(3) + length(4) + text(length)
                     *
                     * We consume the text payload safely in chunks.
                     */
                    uint8_t pad3[3];
                    uint32_t len;
                    if (read_all(c, pad3, 3)) break;
                    if (read_all(c, &len, 4)) break;
                    len = ntohl(len);

                    while (len) {
                        uint8_t tmp[256];
                        size_t n = len > sizeof(tmp) ? sizeof(tmp) : (size_t)len;
                        if (read_all(c, tmp, n)) { len = 0; break; }
                        len -= (uint32_t)n;
                    }
                } else {
                    /* Unknown message type -> disconnect to keep implementation simple */
                    break;
                }
            }

            /*
             * If the client hasn't requested updates yet, don't stream.
             * This reduces unnecessary network use and matches many VNC viewers' expectations.
             */
            if (!client_ready) {
                msleep(50);
                continue;
            }

            /*
             * Send a full FramebufferUpdate containing 1 RAW rectangle.
             *
             * Server-to-client FramebufferUpdate message:
             *   message-type(1)=0
             *   padding(1)=0
             *   number-of-rectangles(2)
             *
             * Then for each rectangle:
             *   x(2), y(2), w(2), h(2), encoding-type(4)
             *   followed by pixel data (for RAW: w*h*bytespp)
             */
            uint8_t fbup[4];
            fbup[0] = 0; /* FramebufferUpdate */
            fbup[1] = 0; /* padding */

            uint16_t nrect = htons(1);
            memcpy(&fbup[2], &nrect, 2);

            if (write_all(c, fbup, 4)) break;

            /* Rectangle header: full-screen, RAW encoding (0) */
            uint16_t x0 = 0, y0 = 0;
            uint16_t ww = htons((uint16_t)width);
            uint16_t hh = htons((uint16_t)height);
            uint32_t enc_raw = htonl(0);

            if (write_all(c, &x0, 2) ||
                write_all(c, &y0, 2) ||
                write_all(c, &ww, 2) ||
                write_all(c, &hh, 2) ||
                write_all(c, &enc_raw, 4)) {
                break;
            }

            /*
             * Pixel data transfer:
             * - For each scanline:
             *   copy width*4 bytes from fbmem using (y * stride) as the source.
             * - Send exactly width*4 bytes (no padding bytes, even if stride > width*4).
             *
             * This is the heaviest part of the program:
             * - CPU cost: memcpy per line + TCP write
             * - Network cost: width*height*4 bytes per frame (e.g., 480*544*4 ≈ 1.04 MB/frame)
             * At 3 FPS that is ~3.1 MB/s, which is reasonable on LAN but still non-trivial.
             * Keep FPS low to remain printer-friendly.
             */
            for (int y = 0; y < height; y++) {
                memcpy(linebuf, fbmem + (size_t)y * (size_t)stride, (size_t)width * 4);
                if (write_all(c, linebuf, (size_t)width * 4)) {
                    y = height; /* force loop exit */
                    break;
                }
            }

            /* Frame pacing */
            msleep(1000 / fps);
        }

        /* Cleanup per-client allocations and socket */
        free(linebuf);
        close(c);
        fprintf(stderr, "fb0rfb: client disconnected\n");
    }

    /* Unreachable in current design; left for completeness */
    return 0;
}
