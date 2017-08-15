/*
 * graphics passthrough
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "xen_pt.h"
#include "xen-host-pci-device.h"
#include "hw/xen/xen_backend.h"

static unsigned long igd_guest_opregion;
static unsigned long igd_host_opregion;

#define XEN_PCI_INTEL_OPREGION_MASK 0xfff

typedef struct VGARegion {
    int type;           /* Memory or port I/O */
    uint64_t guest_base_addr;
    uint64_t machine_base_addr;
    uint64_t size;    /* size of the region */
    int rc;
} VGARegion;

#define IORESOURCE_IO           0x00000100
#define IORESOURCE_MEM          0x00000200

static struct VGARegion vga_args[] = {
    {
        .type = IORESOURCE_IO,
        .guest_base_addr = 0x3B0,
        .machine_base_addr = 0x3B0,
        .size = 0xC,
        .rc = -1,
    },
    {
        .type = IORESOURCE_IO,
        .guest_base_addr = 0x3C0,
        .machine_base_addr = 0x3C0,
        .size = 0x20,
        .rc = -1,
    },
    {
        .type = IORESOURCE_MEM,
        .guest_base_addr = 0xa0000 >> XC_PAGE_SHIFT,
        .machine_base_addr = 0xa0000 >> XC_PAGE_SHIFT,
        .size = 0x20,
        .rc = -1,
    },
};

#define PCI_VENDOR_ID_ATI               0x1002

extern int xen_pt_register_amd_vf_region(XenPCIPassthroughState *s);
extern int xen_pt_unregister_amd_vf_region(XenPCIPassthroughState *s);

/*
 * register VGA resources for the domain with assigned gfx
 */
int xen_pt_register_vga_regions(XenPCIPassthroughState *s)
{
    XenHostPCIDevice *host_dev = &s->real_device;
    int i = 0;

    XEN_PT_LOG(&s->dev, "vendor: %04x device: %04x: class: %08x\n",
               host_dev->vendor_id, host_dev->device_id,
               host_dev->class_code);

    if ((host_dev->vendor_id == PCI_VENDOR_ID_AMD ||
         host_dev->vendor_id == PCI_VENDOR_ID_ATI) &&
        ((host_dev->class_code >> 8) == PCI_CLASS_DISPLAY_OTHER ||
         (host_dev->class_code >> 8) == PCI_CLASS_DISPLAY_VGA) &&
        host_dev->is_virtfn) {
        return xen_pt_register_amd_vf_region(s);
    }

    if (!is_igd_vga_passthrough(host_dev)) {
        return 0;
    }

    for (i = 0 ; i < ARRAY_SIZE(vga_args); i++) {
        if (vga_args[i].type == IORESOURCE_IO) {
            vga_args[i].rc = xc_domain_ioport_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_ADD_MAPPING);
        } else {
            vga_args[i].rc = xc_domain_memory_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_ADD_MAPPING);
        }

        if (vga_args[i].rc) {
            XEN_PT_ERR(NULL, "VGA %s mapping failed! (rc: %i)\n",
                    vga_args[i].type == IORESOURCE_IO ? "ioport" : "memory",
                    vga_args[i].rc);
            return vga_args[i].rc;
        }
    }

    return 0;
}

/*
 * unregister VGA resources for the domain with assigned gfx
 */
int xen_pt_unregister_vga_regions(XenPCIPassthroughState *s)
{
    XenHostPCIDevice *host_dev = &s->real_device;
    int i = 0;
    int ret = 0;

    XEN_PT_LOG(&s->dev, "vendor: %04x device: %04x: class: %08x\n",
               host_dev->vendor_id, host_dev->device_id,
               host_dev->class_code);

    if ((host_dev->vendor_id == PCI_VENDOR_ID_AMD ||
         host_dev->vendor_id == PCI_VENDOR_ID_ATI) &&
        ((host_dev->class_code >> 8) == PCI_CLASS_DISPLAY_OTHER ||
         (host_dev->class_code >> 8) == PCI_CLASS_DISPLAY_VGA) &&
        host_dev->is_virtfn) {
        return xen_pt_unregister_amd_vf_region(s);
    }

    if (!is_igd_vga_passthrough(host_dev)) {
        return 0;
    }

    for (i = 0 ; i < ARRAY_SIZE(vga_args); i++) {
        if (vga_args[i].type == IORESOURCE_IO) {
            vga_args[i].rc = xc_domain_ioport_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_REMOVE_MAPPING);
        } else {
            vga_args[i].rc = xc_domain_memory_mapping(xen_xc, xen_domid,
                            vga_args[i].guest_base_addr,
                            vga_args[i].machine_base_addr,
                            vga_args[i].size, DPCI_REMOVE_MAPPING);
        }

        if (vga_args[i].rc) {
            XEN_PT_ERR(NULL, "VGA %s unmapping failed! (rc: %i)\n",
                    vga_args[i].type == IORESOURCE_IO ? "ioport" : "memory",
                    vga_args[i].rc);
            return vga_args[i].rc;
        }
    }

    if (igd_guest_opregion) {
        ret = xc_domain_memory_mapping(xen_xc, xen_domid,
                (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT),
                (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
                3,
                DPCI_REMOVE_MAPPING);
        if (ret) {
            return ret;
        }
    }

    return 0;
}

/* Refer to Seabios. */
struct rom_header {
    uint16_t signature;
    uint8_t size;
    uint8_t initVector[4];
    uint8_t reserved[17];
    uint16_t pcioffset;
    uint16_t pnpoffset;
} __attribute__((packed));

struct pci_data {
    uint32_t signature;
    uint16_t vendor;
    uint16_t device;
    uint16_t vitaldata;
    uint16_t dlen;
    uint8_t drevision;
    uint8_t class_lo;
    uint16_t class_hi;
    uint16_t ilen;
    uint16_t irevision;
    uint8_t type;
    uint8_t indicator;
    uint16_t reserved;
} __attribute__((packed));

uint32_t igd_read_opregion(XenPCIPassthroughState *s)
{
    uint32_t val = 0;

    if (!igd_guest_opregion) {
        return val;
    }

    val = igd_guest_opregion;

    XEN_PT_LOG(&s->dev, "Read opregion val=%x\n", val);
    return val;
}

#define XEN_PCI_INTEL_OPREGION_PAGES 0x3
#define XEN_PCI_INTEL_OPREGION_ENABLE_ACCESSED 0x1
void igd_write_opregion(XenPCIPassthroughState *s, uint32_t val)
{
    int ret;

    if (igd_guest_opregion) {
        XEN_PT_LOG(&s->dev, "opregion register already been set, ignoring %x\n",
                   val);
        return;
    }

    /* We just work with LE. */
    xen_host_pci_get_block(&s->real_device, XEN_PCI_INTEL_OPREGION,
            (uint8_t *)&igd_host_opregion, 4);
    igd_guest_opregion = (unsigned long)(val & ~XEN_PCI_INTEL_OPREGION_MASK)
                            | (igd_host_opregion & XEN_PCI_INTEL_OPREGION_MASK);

    ret = xc_domain_iomem_permission(xen_xc, xen_domid,
            (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
            XEN_PCI_INTEL_OPREGION_PAGES,
            XEN_PCI_INTEL_OPREGION_ENABLE_ACCESSED);

    if (ret) {
        XEN_PT_ERR(&s->dev, "[%d]:Can't enable to access IGD host opregion:"
                    " 0x%lx.\n", ret,
                    (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT)),
        igd_guest_opregion = 0;
        return;
    }

    ret = xc_domain_memory_mapping(xen_xc, xen_domid,
            (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT),
            (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
            XEN_PCI_INTEL_OPREGION_PAGES,
            DPCI_ADD_MAPPING);

    if (ret) {
        XEN_PT_ERR(&s->dev, "[%d]:Can't map IGD host opregion:0x%lx to"
                    " guest opregion:0x%lx.\n", ret,
                    (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
                    (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT));
        igd_guest_opregion = 0;
        return;
    }

    XEN_PT_LOG(&s->dev, "Map OpRegion: 0x%lx -> 0x%lx\n",
                    (unsigned long)(igd_host_opregion >> XC_PAGE_SHIFT),
                    (unsigned long)(igd_guest_opregion >> XC_PAGE_SHIFT));
}
