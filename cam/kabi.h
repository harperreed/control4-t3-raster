/* ABOUTME: Kernel-exact V4L2 and Rockchip ION structs and ioctl numbers for the TT7's 3.0.36 kernel.
 * ABOUTME: Fixed-width fields, so the layout is identical on the ARM target and the x86-64 test host. */
#ifndef CAM_KABI_H
#define CAM_KABI_H

/*
 * Why not <linux/videodev2.h>: zig's musl has a 64-bit time_t, so its
 * struct v4l2_buffer is 80 bytes and its VIDIOC_QBUF encodes that size; the
 * 3.0.36 kernel's is 68 bytes and it rejects the ioctl (MMKeypad,
 * reference/t3-control4/CAMERA.md, "The ABI bug"). The other structs here
 * have no timeval, but defining them all from one source keeps the ABI in
 * one place and pins it to the kernel we run, not to zig's newer headers.
 *
 * Every layout below is transcribed from the Rockchip RK3188 3.0.36 kernel
 * tree github.com/Nu3001/kernel_rk3188 (master, read 2026-09-27):
 *   include/linux/videodev2.h     v4l2_* structs, VIDIOC_* numbers, enums
 *   include/linux/ion.h           ion_* structs, ION_IOC_*, heap ids
 *   include/asm-generic/ioctl.h   _IOC encoding (arch/arm/include/asm/ioctl.h includes it)
 *   arch/arm/include/asm/posix_types.h  __kernel_time_t/__kernel_suseconds_t = long
 * On 32-bit ARM, long, size_t and pointers are 4 bytes, so each is uint32_t
 * here. The panel's kernel is Control4's "glassedge" build of 3.0.36; its
 * source is not public, so these match the Rockchip tree, not a verified
 * Control4 tree. test_kabi.c checks the sizes and numbers against a hand
 * derivation.
 */

#include <stddef.h>
#include <stdint.h>

/* _IOC(dir,type,nr,size) from include/asm-generic/ioctl.h:
 * NRBITS 8, TYPEBITS 8, SIZEBITS 14, DIRBITS 2; NONE 0, WRITE 1, READ 2. */
#define K_IOC_WRITE 1u
#define K_IOC_READ 2u
#define K_IOC(dir, type, nr, size) \
    ((uint32_t)(((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr)))
#define K_IOR(type, nr, size) K_IOC(K_IOC_READ, (type), (nr), (size))
#define K_IOW(type, nr, size) K_IOC(K_IOC_WRITE, (type), (nr), (size))
#define K_IOWR(type, nr, size) K_IOC(K_IOC_READ | K_IOC_WRITE, (type), (nr), (size))

/* ---- V4L2 (include/linux/videodev2.h) ---- */

#define K_V4L2_BUF_TYPE_VIDEO_CAPTURE 1u /* enum v4l2_buf_type */
#define K_V4L2_MEMORY_OVERLAY 3u         /* enum v4l2_memory */
#define K_V4L2_FIELD_ANY 0u              /* enum v4l2_field */
#define K_V4L2_CAP_VIDEO_CAPTURE 0x00000001u
#define K_V4L2_CAP_STREAMING 0x04000000u
#define K_V4L2_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define K_V4L2_PIX_FMT_NV12 K_V4L2_FOURCC('N', 'V', '1', '2')

struct k_v4l2_capability {
    uint8_t driver[16];
    uint8_t card[32];
    uint8_t bus_info[32];
    uint32_t version;
    uint32_t capabilities;
    uint32_t reserved[4];
};

struct k_v4l2_fmtdesc {
    uint32_t index;
    uint32_t type; /* enum v4l2_buf_type */
    uint32_t flags;
    uint8_t description[32];
    uint32_t pixelformat;
    uint32_t reserved[4];
};

struct k_v4l2_pix_format {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t field; /* enum v4l2_field */
    uint32_t bytesperline;
    uint32_t sizeimage;
    uint32_t colorspace; /* enum v4l2_colorspace */
    uint32_t priv;
};

/* The kernel's union also holds v4l2_window etc.; none has a member wider
 * than 4 bytes on ARM, so raw_data[200] sets the size and alignment. */
struct k_v4l2_format {
    uint32_t type;
    union {
        struct k_v4l2_pix_format pix;
        uint8_t raw_data[200];
    } fmt;
};

struct k_v4l2_requestbuffers {
    uint32_t count;
    uint32_t type;
    uint32_t memory;
    uint32_t reserved[2];
};

struct k_timeval { /* the kernel's: {long tv_sec; long tv_usec;} on 32-bit ARM */
    int32_t tv_sec;
    int32_t tv_usec;
};

struct k_v4l2_timecode {
    uint32_t type;
    uint32_t flags;
    uint8_t frames, seconds, minutes, hours;
    uint8_t userbits[4];
};

struct k_v4l2_buffer {
    uint32_t index;
    uint32_t type;
    uint32_t bytesused;
    uint32_t flags;
    uint32_t field;
    struct k_timeval timestamp;
    struct k_v4l2_timecode timecode;
    uint32_t sequence;
    uint32_t memory;
    union { /* offset / unsigned long userptr / struct v4l2_plane *planes */
        uint32_t offset;
        uint32_t userptr;
    } m;
    uint32_t length;
    uint32_t input;
    uint32_t reserved;
};

#define K_VIDIOC_QUERYCAP K_IOR('V', 0, sizeof(struct k_v4l2_capability))
#define K_VIDIOC_ENUM_FMT K_IOWR('V', 2, sizeof(struct k_v4l2_fmtdesc))
#define K_VIDIOC_G_FMT K_IOWR('V', 4, sizeof(struct k_v4l2_format))
#define K_VIDIOC_S_FMT K_IOWR('V', 5, sizeof(struct k_v4l2_format))
#define K_VIDIOC_REQBUFS K_IOWR('V', 8, sizeof(struct k_v4l2_requestbuffers))
#define K_VIDIOC_QBUF K_IOWR('V', 15, sizeof(struct k_v4l2_buffer))
#define K_VIDIOC_DQBUF K_IOWR('V', 17, sizeof(struct k_v4l2_buffer))
#define K_VIDIOC_STREAMON K_IOW('V', 18, sizeof(int32_t))
#define K_VIDIOC_STREAMOFF K_IOW('V', 19, sizeof(int32_t))

/* ---- Rockchip ION (include/linux/ion.h) ---- */

/* enum ion_heap_ids. ion_alloc() treats the ALLOC flags as a mask of
 * (1 << heap id) (drivers/gpu/ion/ion.c). The RK3188 SDK board file
 * (arch/arm/mach-rk3188/board-rk3188-sdk.c) registers one heap: a
 * CARVEOUT with id ION_NOR_HEAP_ID, sized 120M when DDR >= 1G. Our dmesg
 * shows exactly that ("reserved for <ion>", 120M), so NOR is tried first. */
#define K_ION_NOR_HEAP_ID 0u
#define K_ION_CMA_HEAP_ID 1u
#define K_ION_CAM_ID 17u

#define K_ION_CACHE_FLUSH 0u
#define K_ION_CACHE_INV 2u

/* struct ion_handle * is an opaque kernel pointer: 4 bytes on ARM. */
struct k_ion_allocation_data {
    uint32_t len;   /* size_t */
    uint32_t align; /* size_t */
    uint32_t flags; /* heap mask */
    uint32_t handle;
};

struct k_ion_fd_data {
    uint32_t handle;
    int32_t fd;
};

struct k_ion_handle_data {
    uint32_t handle;
};

struct k_ion_phys_data {
    uint32_t handle;
    uint32_t phys; /* unsigned long */
    uint32_t size; /* unsigned long */
};

/* virt must be the exact address mmap() returned for this buffer: the
 * carveout heap looks it up in its list of user mappings
 * (drivers/gpu/ion/ion_carveout_heap.c, ion_carveout_cache_op). The
 * ION_CUSTOM_CACHE_OP ioctl returns 0 even when the op fails (ion.c computes
 * err but breaks out of the switch); the only trace is a dmesg line
 * "virt(...) has not been maped or has been unmaped". */
struct k_ion_cacheop_data {
    uint32_t type;
    uint32_t handle;
    uint32_t virt; /* void * */
};

#define K_ION_IOC_ALLOC K_IOWR('I', 0, sizeof(struct k_ion_allocation_data))
#define K_ION_IOC_FREE K_IOWR('I', 1, sizeof(struct k_ion_handle_data))
#define K_ION_IOC_MAP K_IOWR('I', 2, sizeof(struct k_ion_fd_data))
#define K_ION_CUSTOM_GET_PHYS K_IOWR('I', 7, sizeof(struct k_ion_phys_data))
#define K_ION_CUSTOM_CACHE_OP K_IOWR('I', 8, sizeof(struct k_ion_cacheop_data))

/* Compile-time guard for both the ARM build and the host tests: the sizes
 * that get encoded into the ioctl numbers (derivation in test_kabi.c). */
_Static_assert(sizeof(struct k_v4l2_capability) == 104, "v4l2_capability: 104 bytes");
_Static_assert(sizeof(struct k_v4l2_fmtdesc) == 64, "v4l2_fmtdesc: 64 bytes");
_Static_assert(sizeof(struct k_v4l2_format) == 204, "v4l2_format: 204 bytes");
_Static_assert(sizeof(struct k_v4l2_requestbuffers) == 20, "v4l2_requestbuffers: 20 bytes");
_Static_assert(sizeof(struct k_v4l2_buffer) == 68, "v4l2_buffer: the 3.0.36 ARM kernel's 68 bytes, not musl's 80");
_Static_assert(sizeof(struct k_ion_allocation_data) == 16, "ion_allocation_data: 16 bytes");
_Static_assert(sizeof(struct k_ion_handle_data) == 4, "ion_handle_data: 4 bytes");
_Static_assert(sizeof(struct k_ion_fd_data) == 8, "ion_fd_data: 8 bytes");
_Static_assert(sizeof(struct k_ion_phys_data) == 12, "ion_phys_data: 12 bytes");
_Static_assert(sizeof(struct k_ion_cacheop_data) == 12, "ion_cacheop_data: 12 bytes");

#endif
