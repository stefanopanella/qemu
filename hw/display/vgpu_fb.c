#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "hw/xen/xen_backend.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef struct shared_surface {
    uint32_t    offset;
    uint32_t    linesize;
    uint32_t    width;
    uint32_t    height;
    uint32_t    depth;
    uint32_t    update;
    uint16_t    port;
} QEMU_PACKED shared_surface_t;

typedef struct VGPUState {
    SysBusDevice sysdev;

    QemuConsole *con;
    shared_surface_t *shared;

    uint32_t surface_offset;
    uint32_t surface_linesize;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t surface_depth;
    uint32_t surface_update;

    uint8_t *surface_buffer;

    struct sockaddr_in server;
    int surface_fd;
} VGPUState;

#define TYPE_VGPU "vgpu"
#define VGPU(obj) \
    OBJECT_CHECK(VGPUState, (obj), TYPE_VGPU)

static void vgpu_fb_update(void *opaque)
{
    VGPUState *s = VGPU(opaque);
    DisplaySurface *surface;
    pixman_format_code_t format;
    char buf = 'S';

    sendto(s->surface_fd, &buf, 1, MSG_DONTWAIT, &s->server, sizeof (s->server));

    if (s->surface_offset != s->shared->offset ||
        s->surface_linesize != s->shared->linesize ||
        s->surface_width != s->shared->width ||
        s->surface_height != s->shared->height ||
        s->surface_depth != s->shared->depth) {

        s->surface_offset = s->shared->offset;
        s->surface_linesize = s->shared->linesize;
        s->surface_width = s->shared->width;
        s->surface_height = s->shared->height;
        s->surface_depth = s->shared->depth;

        fprintf(stderr, "%s: %dx%dx%d @ %x (linesize = %x)\n", __func__,
                s->surface_width, s->surface_height, s->surface_depth,
                s->surface_offset, s->surface_linesize);

        format = qemu_default_pixman_format(s->surface_depth, true);
        surface = qemu_create_displaysurface_from(s->surface_width,
                                                  s->surface_height,
                                                  format,
                                                  s->surface_linesize,
                                                  s->surface_buffer + s->surface_offset);
        dpy_gfx_replace_surface(s->con, surface);
    }

    if (s->surface_update != s->shared->update) {
        s->surface_update = s->shared->update;

        dpy_gfx_update(s->con, 0, 0,
                       s->surface_width, s->surface_height);
    }
}

#define SURFACE_RESERVED_ADDRESS    0xff000000
#define SURFACE_RESERVED_SIZE       0x01000000

static const GraphicHwOps vgpu_ops = {
    .gfx_update  = vgpu_fb_update
};

static void vgpu_fb_realize(DeviceState *dev, Error **errp)
{
    VGPUState *s = VGPU(dev);
    const int n = SURFACE_RESERVED_SIZE >> TARGET_PAGE_BITS;
    xen_pfn_t pfn[n];
    int fd, i;

    s->con = graphic_console_init(dev, 0, &vgpu_ops, s);

    for (i = 0; i < n; i++)
        pfn[i] = (SURFACE_RESERVED_ADDRESS >> TARGET_PAGE_BITS) + i;

    s->surface_buffer = xenforeignmemory_map(xen_fmem, xen_domid,
                                             PROT_READ | PROT_WRITE,
                                             n, pfn, NULL);
    if (s->surface_buffer == NULL) {
        fprintf(stderr, "mmap failed\n");
        exit(1);
    }

    s->shared = (shared_surface_t *)(s->surface_buffer +
                                     SURFACE_RESERVED_SIZE -
                                     TARGET_PAGE_SIZE);

    fprintf(stderr, "vgpu: port = %u\n", s->shared->port);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket failed\n");
        exit(1);
    }

    s->surface_fd = fd;

    s->server.sin_family = AF_INET;
    s->server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s->server.sin_port = htons(s->shared->port);
}

static void vgpu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = vgpu_fb_realize;
    /* we might want to have some properties here later */
    dc->props = NULL;
    dc->user_creatable = true;
    dc->hotpluggable = false;
}

static const TypeInfo vgpu_info = {
    .name          = TYPE_VGPU,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VGPUState),
    .class_init    = vgpu_class_init,
};

static void vgpu_register_types(void)
{
    type_register_static(&vgpu_info);
}

type_init(vgpu_register_types)
