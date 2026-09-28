/* ABOUTME: Drives /dev/video0 and /dev/ion with the kernel-exact ioctls from kabi.h.
 * ABOUTME: Sequence from MMKeypad's mmk-selftest-camera.c (Apache-2.0), rewritten with strict cleanup. */
#include "capture.h"

/*
 * The capture sequence (S_FMT -> REQBUFS(OVERLAY) -> ion alloc + GET_PHYS ->
 * QBUF(OVERLAY, m.offset = phys) -> STREAMON -> DQBUF -> ION cache
 * invalidate) follows MMKeypad firmware-linux-t3/tools/selftest/
 * mmk-selftest-camera.c and reference/t3-control4/CAMERA.md, commit
 * c95555d644e747db3d0fb15b12e087974e5662a7, Copyright 2026 NuVoxel LLC,
 * Apache License 2.0 (third_party/mmkeypad/LICENSE and NOTICE). No code is
 * copied verbatim; the structs and numbers come from kabi.h instead.
 *
 * Why OVERLAY: the RK CIF driver's buf_prepare refuses a buffer whose boff
 * is 0 (rk30_camera_oneframe.c, rk_videobuf_prepare), and only the OVERLAY
 * QBUF path sets boff (videobuf-core.c, videobuf_qbuf: "case
 * V4L2_MEMORY_OVERLAY: buf->boff = b->m.offset"). The driver captures into
 * its own vipmem region, then ipp_blit_sync() copies to boff
 * (rk_camera_scale_crop_ipp).
 *
 * Added here: the CPU cache is flushed after the sentinel fill, so a dirty
 * 0xAA line cannot be written back over the IPP's output later; the wait
 * uses poll() with a timeout instead of a blocking DQBUF; each grab refills
 * the sentinel.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kabi.h"

static int xioctl(int fd, uint32_t request, void *arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r == -1 && errno == EINTR);
    return r;
}

#define LOG(c, ...) (fprintf((c)->log, "tt7cam: " __VA_ARGS__), fputc('\n', (c)->log))
#define FAIL(c, what) (LOG(c, "%s failed: %s", (what), strerror(errno)), -1)

void cam_init(struct cam *c) {
    memset(c, 0, sizeof *c);
    c->vfd = c->ionfd = c->buf_fd = -1;
    c->log = stderr;
}

static int cache_op(struct cam *c, uint32_t type) {
    struct k_ion_cacheop_data op = {.type = type, .handle = c->handle, .virt = (uint32_t)(uintptr_t)c->va};
    return xioctl(c->ionfd, K_ION_CUSTOM_CACHE_OP, &op);
}

static int ion_alloc(struct cam *c) {
    /* NOR first: the RK3188 board file registers only that heap (kabi.h).
     * MMKeypad's selftest tries CAM(17) and CMA(1) too, so keep them as fallbacks. */
    static const struct { uint32_t id; const char *name; } heaps[] = {
        {K_ION_NOR_HEAP_ID, "NOR(0)"},
        {K_ION_CAM_ID, "CAM(17)"},
        {K_ION_CMA_HEAP_ID, "CMA(1)"},
    };
    for (size_t i = 0; i < sizeof heaps / sizeof heaps[0]; i++) {
        struct k_ion_allocation_data a = {.len = (uint32_t)c->len, .align = 4096, .flags = 1u << heaps[i].id};
        if (xioctl(c->ionfd, K_ION_IOC_ALLOC, &a) == 0) {
            c->handle = a.handle;
            c->have_handle = 1;
            c->heap = heaps[i].name;
            LOG(c, "ION_IOC_ALLOC ok: heap %s, %zu bytes", heaps[i].name, c->len);
            return 0;
        }
        LOG(c, "ION_IOC_ALLOC heap %s: %s", heaps[i].name, strerror(errno));
    }
    LOG(c, "no ion heap accepted %zu bytes", c->len);
    return -1;
}

int cam_open(struct cam *c, const struct cam_config *cfg) {
    c->log = cfg->log ? cfg->log : stderr;

    c->vfd = open(cfg->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c->vfd < 0) {
        LOG(c, "open %s failed: %s", cfg->device, strerror(errno));
        return -1;
    }

    struct k_v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = (uint32_t)cfg->width;
    f.fmt.pix.height = (uint32_t)cfg->height;
    f.fmt.pix.pixelformat = K_V4L2_PIX_FMT_NV12;
    f.fmt.pix.field = K_V4L2_FIELD_ANY;
    if (xioctl(c->vfd, K_VIDIOC_S_FMT, &f) < 0) return FAIL(c, "VIDIOC_S_FMT");
    if (f.fmt.pix.pixelformat != K_V4L2_PIX_FMT_NV12) {
        LOG(c, "S_FMT: driver chose fourcc 0x%08x, not NV12", f.fmt.pix.pixelformat);
        return -1;
    }
    if (f.fmt.pix.width != (uint32_t)cfg->width || f.fmt.pix.height != (uint32_t)cfg->height)
        LOG(c, "S_FMT: asked %dx%d, driver set %ux%u; using the driver's size", cfg->width, cfg->height,
            f.fmt.pix.width, f.fmt.pix.height);
    c->width = (int)f.fmt.pix.width;
    c->height = (int)f.fmt.pix.height;
    if (c->width <= 0 || c->height <= 0 || (c->width & 1) || (c->height & 1)) {
        LOG(c, "S_FMT: unusable size %dx%d (NV12 needs even, nonzero sides)", c->width, c->height);
        return -1;
    }
    c->frame_len = (size_t)c->width * c->height * 3 / 2;
    size_t want = f.fmt.pix.sizeimage > c->frame_len ? f.fmt.pix.sizeimage : c->frame_len;
    c->len = (want + 4095u) & ~(size_t)4095u;
    LOG(c, "S_FMT ok: NV12 %dx%d bytesperline=%u sizeimage=%u; ion buffer %zu bytes", c->width, c->height,
        f.fmt.pix.bytesperline, f.fmt.pix.sizeimage, c->len);

    struct k_v4l2_requestbuffers rb = {.count = 1, .type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                       .memory = K_V4L2_MEMORY_OVERLAY};
    if (xioctl(c->vfd, K_VIDIOC_REQBUFS, &rb) < 0) return FAIL(c, "VIDIOC_REQBUFS(OVERLAY)");
    LOG(c, "REQBUFS(OVERLAY) ok: count=%u", rb.count);
    if (rb.count < 1) {
        LOG(c, "REQBUFS granted no buffers");
        return -1;
    }

    c->ionfd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (c->ionfd < 0) return FAIL(c, "open /dev/ion");
    if (ion_alloc(c) < 0) return -1;

    struct k_ion_fd_data fd = {.handle = c->handle, .fd = -1};
    if (xioctl(c->ionfd, K_ION_IOC_MAP, &fd) < 0) return FAIL(c, "ION_IOC_MAP");
    c->buf_fd = fd.fd;
    void *va = mmap(NULL, c->len, PROT_READ | PROT_WRITE, MAP_SHARED, c->buf_fd, 0);
    if (va == MAP_FAILED) return FAIL(c, "mmap ion buffer");
    c->va = va;

    struct k_ion_phys_data pd = {.handle = c->handle};
    if (xioctl(c->ionfd, K_ION_CUSTOM_GET_PHYS, &pd) < 0) return FAIL(c, "ION_CUSTOM_GET_PHYS");
    if (pd.phys == 0 || pd.size < c->len) {
        LOG(c, "GET_PHYS: phys=0x%08x size=%u is not a usable %zu-byte buffer", pd.phys, pd.size, c->len);
        return -1;
    }
    c->phys = pd.phys;
    LOG(c, "ion buffer: phys=0x%08x size=%u", pd.phys, pd.size);
    return 0;
}

int cam_grab(struct cam *c, int timeout_ms, struct sentinel_report *rep) {
    sentinel_fill(c->va, c->width, c->height);
    if (cache_op(c, K_ION_CACHE_FLUSH) < 0) return FAIL(c, "ION_CUSTOM_CACHE_OP(flush)");

    struct k_v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.index = 0;
    b.type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = K_V4L2_MEMORY_OVERLAY;
    b.m.offset = c->phys;
    b.length = (uint32_t)c->len;
    if (xioctl(c->vfd, K_VIDIOC_QBUF, &b) < 0) return FAIL(c, "VIDIOC_QBUF(OVERLAY)");

    if (!c->streaming) {
        int32_t type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(c->vfd, K_VIDIOC_STREAMON, &type) < 0) return FAIL(c, "VIDIOC_STREAMON");
        c->streaming = 1;
        LOG(c, "STREAMON ok");
    }

    struct pollfd p = {.fd = c->vfd, .events = POLLIN};
    int n = poll(&p, 1, timeout_ms);
    if (n < 0) return errno == EINTR ? -1 : FAIL(c, "poll");
    if (n == 0) {
        LOG(c, "no frame within %d ms (the CIF produced nothing)", timeout_ms);
        errno = ETIMEDOUT;
        return -1;
    }
    if (p.revents & POLLERR) {
        LOG(c, "poll reported POLLERR (driver says no buffer is queued)");
        errno = EIO;
        return -1;
    }

    memset(&b, 0, sizeof b);
    b.type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = K_V4L2_MEMORY_OVERLAY;
    if (xioctl(c->vfd, K_VIDIOC_DQBUF, &b) < 0) return FAIL(c, "VIDIOC_DQBUF");
    c->sequence = b.sequence;
    if (cache_op(c, K_ION_CACHE_INV) < 0) return FAIL(c, "ION_CUSTOM_CACHE_OP(invalidate)");

    sentinel_check(c->va, c->width, c->height, rep);
    return rep->verdict == FRAME_CAPTURED ? 0 : 1;
}

void cam_close(struct cam *c) {
    if (c->streaming) {
        int32_t type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(c->vfd, K_VIDIOC_STREAMOFF, &type) < 0) LOG(c, "VIDIOC_STREAMOFF failed: %s", strerror(errno));
        else LOG(c, "STREAMOFF ok");
        c->streaming = 0;
    }
    if (c->vfd >= 0 && close(c->vfd) < 0) LOG(c, "close video fd: %s", strerror(errno));
    c->vfd = -1;
    if (c->va && munmap(c->va, c->len) < 0) LOG(c, "munmap ion buffer: %s", strerror(errno));
    c->va = NULL;
    if (c->buf_fd >= 0 && close(c->buf_fd) < 0) LOG(c, "close ion buffer fd: %s", strerror(errno));
    c->buf_fd = -1;
    if (c->have_handle) {
        struct k_ion_handle_data h = {.handle = c->handle};
        if (xioctl(c->ionfd, K_ION_IOC_FREE, &h) < 0) LOG(c, "ION_IOC_FREE failed: %s", strerror(errno));
        c->have_handle = 0;
    }
    if (c->ionfd >= 0 && close(c->ionfd) < 0) LOG(c, "close /dev/ion: %s", strerror(errno));
    c->ionfd = -1;
}
