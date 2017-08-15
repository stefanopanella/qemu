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

#define PCI_VENDOR_ID_ATI               0x1002

extern int xen_pt_register_amd_vf_region(XenPCIPassthroughState *s);
extern int xen_pt_unregister_amd_vf_region(XenPCIPassthroughState *s);

/*
 * register VGA resources for the domain with assigned gfx
 */
int xen_pt_register_vga_regions(XenPCIPassthroughState *s)
{
    XenHostPCIDevice *host_dev = &s->real_device;

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

    return 0;
}

/*
 * unregister VGA resources for the domain with assigned gfx
 */
int xen_pt_unregister_vga_regions(XenPCIPassthroughState *s)
{
    XenHostPCIDevice *host_dev = &s->real_device;
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
