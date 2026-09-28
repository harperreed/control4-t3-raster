/* ABOUTME: Host test for kabi.h: struct sizes and ioctl numbers must match the 3.0.36 kernel ABI.
 * ABOUTME: Expected numbers are worked out by hand from the kernel's _IOC formula (see below). */
#include "kabi.h"
#include "test_common.h"

/*
 * Hand derivation. include/asm-generic/ioctl.h (Rockchip 3.0.36 tree,
 * Nu3001/kernel_rk3188; ARM's asm/ioctl.h just includes it):
 *   _IOC(dir,type,nr,size) = dir<<30 | size<<16 | type<<8 | nr
 *   _IOC_NONE 0, _IOC_WRITE 1, _IOC_READ 2 (so _IOWR = 3)
 * 'V' = 0x56, 'I' = 0x49.
 *
 * Struct sizes on 32-bit ARM (long and pointers are 4 bytes; the kernel's
 * struct timeval is {long, long} = 8 bytes, arch/arm/include/asm/posix_types.h):
 *   v4l2_capability   16+32+32 + 4+4 + 4*4                      = 104 = 0x68
 *   v4l2_fmtdesc      4+4+4 + 32 + 4 + 4*4                      =  64 = 0x40
 *   v4l2_format       4 (type) + 200 (union raw_data)           = 204 = 0xcc
 *   v4l2_requestbuffers 4+4+4 + 2*4                             =  20 = 0x14
 *   v4l2_buffer       5*4 + 8 (timeval) + 16 (timecode) + 4 (sequence)
 *                     + 4 (memory) + 4 (m) + 3*4                =  68 = 0x44
 *   ion_allocation_data size_t,size_t,uint,ptr                  =  16 = 0x10
 *   ion_handle_data   ptr                                       =   4
 *   ion_fd_data       ptr,int                                   =   8
 *   ion_phys_data     ptr,ulong,ulong                           =  12 = 0x0c
 *   ion_cacheop_data  uint,ptr,ptr                              =  12 = 0x0c
 *
 *   VIDIOC_QUERYCAP  _IOR ('V', 0,104) = 2<<30 | 0x68<<16 | 0x5600 = 0x80685600
 *   VIDIOC_ENUM_FMT  _IOWR('V', 2, 64) = 3<<30 | 0x40<<16 | 0x5602 = 0xc0405602
 *   VIDIOC_G_FMT     _IOWR('V', 4,204) = 3<<30 | 0xcc<<16 | 0x5604 = 0xc0cc5604
 *   VIDIOC_S_FMT     _IOWR('V', 5,204)                             = 0xc0cc5605
 *   VIDIOC_REQBUFS   _IOWR('V', 8, 20) = 3<<30 | 0x14<<16 | 0x5608 = 0xc0145608
 *   VIDIOC_QBUF      _IOWR('V',15, 68) = 3<<30 | 0x44<<16 | 0x560f = 0xc044560f
 *   VIDIOC_DQBUF     _IOWR('V',17, 68)                             = 0xc0445611
 *   VIDIOC_STREAMON  _IOW ('V',18,int) = 1<<30 | 0x04<<16 | 0x5612 = 0x40045612
 *   VIDIOC_STREAMOFF _IOW ('V',19,int)                             = 0x40045613
 *   ION_IOC_ALLOC    _IOWR('I', 0, 16) = 3<<30 | 0x10<<16 | 0x4900 = 0xc0104900
 *   ION_IOC_FREE     _IOWR('I', 1,  4)                             = 0xc0044901
 *   ION_IOC_MAP      _IOWR('I', 2,  8)                             = 0xc0084902
 *   ION_CUSTOM_GET_PHYS _IOWR('I',7,12)                            = 0xc00c4907
 *   ION_CUSTOM_CACHE_OP _IOWR('I',8,12)                            = 0xc00c4908
 *
 * MMKeypad's reference/t3-control4/CAMERA.md independently reports the
 * kernel's VIDIOC_QBUF as 0xc044560f and musl's 80-byte one as 0xc050560f.
 */
int main(void) {
    CHECK(sizeof(struct k_v4l2_capability) == 104, "%zu", sizeof(struct k_v4l2_capability));
    CHECK(sizeof(struct k_v4l2_fmtdesc) == 64, "%zu", sizeof(struct k_v4l2_fmtdesc));
    CHECK(sizeof(struct k_v4l2_pix_format) == 32, "%zu", sizeof(struct k_v4l2_pix_format));
    CHECK(sizeof(struct k_v4l2_format) == 204, "%zu", sizeof(struct k_v4l2_format));
    CHECK(sizeof(struct k_v4l2_requestbuffers) == 20, "%zu", sizeof(struct k_v4l2_requestbuffers));
    CHECK(sizeof(struct k_v4l2_buffer) == 68, "%zu", sizeof(struct k_v4l2_buffer));
    CHECK(sizeof(struct k_ion_allocation_data) == 16, "%zu", sizeof(struct k_ion_allocation_data));
    CHECK(sizeof(struct k_ion_handle_data) == 4, "%zu", sizeof(struct k_ion_handle_data));
    CHECK(sizeof(struct k_ion_fd_data) == 8, "%zu", sizeof(struct k_ion_fd_data));
    CHECK(sizeof(struct k_ion_phys_data) == 12, "%zu", sizeof(struct k_ion_phys_data));
    CHECK(sizeof(struct k_ion_cacheop_data) == 12, "%zu", sizeof(struct k_ion_cacheop_data));

    /* Field offsets in v4l2_buffer that the capture path reads or writes. */
    CHECK(offsetof(struct k_v4l2_buffer, timestamp) == 20, "%zu", offsetof(struct k_v4l2_buffer, timestamp));
    CHECK(offsetof(struct k_v4l2_buffer, sequence) == 44, "%zu", offsetof(struct k_v4l2_buffer, sequence));
    CHECK(offsetof(struct k_v4l2_buffer, memory) == 48, "%zu", offsetof(struct k_v4l2_buffer, memory));
    CHECK(offsetof(struct k_v4l2_buffer, m) == 52, "%zu", offsetof(struct k_v4l2_buffer, m));
    CHECK(offsetof(struct k_v4l2_buffer, length) == 56, "%zu", offsetof(struct k_v4l2_buffer, length));
    CHECK(offsetof(struct k_v4l2_format, fmt) == 4, "%zu", offsetof(struct k_v4l2_format, fmt));

    struct { const char *name; uint32_t got, want; } nums[] = {
        {"VIDIOC_QUERYCAP", K_VIDIOC_QUERYCAP, 0x80685600u},
        {"VIDIOC_ENUM_FMT", K_VIDIOC_ENUM_FMT, 0xc0405602u},
        {"VIDIOC_G_FMT", K_VIDIOC_G_FMT, 0xc0cc5604u},
        {"VIDIOC_S_FMT", K_VIDIOC_S_FMT, 0xc0cc5605u},
        {"VIDIOC_REQBUFS", K_VIDIOC_REQBUFS, 0xc0145608u},
        {"VIDIOC_QBUF", K_VIDIOC_QBUF, 0xc044560fu},
        {"VIDIOC_DQBUF", K_VIDIOC_DQBUF, 0xc0445611u},
        {"VIDIOC_STREAMON", K_VIDIOC_STREAMON, 0x40045612u},
        {"VIDIOC_STREAMOFF", K_VIDIOC_STREAMOFF, 0x40045613u},
        {"ION_IOC_ALLOC", K_ION_IOC_ALLOC, 0xc0104900u},
        {"ION_IOC_FREE", K_ION_IOC_FREE, 0xc0044901u},
        {"ION_IOC_MAP", K_ION_IOC_MAP, 0xc0084902u},
        {"ION_CUSTOM_GET_PHYS", K_ION_CUSTOM_GET_PHYS, 0xc00c4907u},
        {"ION_CUSTOM_CACHE_OP", K_ION_CUSTOM_CACHE_OP, 0xc00c4908u},
    };
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++)
        CHECK(nums[i].got == nums[i].want, "%s = 0x%08x, want 0x%08x", nums[i].name, nums[i].got, nums[i].want);

    CHECK(K_V4L2_PIX_FMT_NV12 == 0x3231564eu, "NV12 fourcc 0x%08x", K_V4L2_PIX_FMT_NV12);
    return test_finish("cam kabi: struct sizes and ioctl numbers");
}
