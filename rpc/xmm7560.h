#include <asm/ioctl.h>

#define XMM7560_IOCTL_GET_PAGE_SIZE _IOC(_IOC_READ, 'x', 0xc0, sizeof(uint32_t))
