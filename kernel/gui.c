/* gui.c -- graphics subsystem bring-up
 *
 * Sets the video mode via Bochs VBE port I/O, finds the resulting
 * linear framebuffer's physical address via real PCI enumeration
 * (not a hardcoded guess), maps that physical range into kernel
 * virtual memory (identity-mapped for simplicity -- same physical and
 * virtual address, just like our low-memory region, so no extra
 * bookkeeping is needed anywhere else), and hands it to fb.c.
 */

#include "gui.h"
#include "vbe.h"
#include "pci.h"
#include "fb.h"
#include "paging.h"
#include "serial.h"

#define GUI_WIDTH  1024
#define GUI_HEIGHT 768
#define GUI_BPP    32
#define PAGE_SIZE  4096u
#define PAGE_RW    0x2

static unsigned int g_fb_phys = 0, g_fb_pitch = 0;
static int g_gui_ready = 0;

int gui_init(void) {
    if (g_gui_ready) return 1; /* idempotent -- safe to call more than once */

    if (!vbe_set_mode(GUI_WIDTH, GUI_HEIGHT, GUI_BPP)) {
        serial_write("[serial] gui_init: VBE mode set failed\n");
        return 0;
    }

    unsigned int fb_phys;
    if (!pci_find_vga_framebuffer(&fb_phys)) {
        serial_write("[serial] gui_init: could not find framebuffer via PCI\n");
        return 0;
    }

    unsigned int pitch = GUI_WIDTH * (GUI_BPP / 8);
    unsigned int fb_bytes = pitch * GUI_HEIGHT;
    unsigned int num_pages = (fb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (unsigned int i = 0; i < num_pages; i++) {
        unsigned int addr = fb_phys + i * PAGE_SIZE;
        if (!paging_map_kernel_page(addr, addr, PAGE_RW)) {
            serial_write("[serial] gui_init: framebuffer mapping failed mid-way\n");
            return 0;
        }
    }

    fb_init((unsigned int *)fb_phys, GUI_WIDTH, GUI_HEIGHT, pitch);
    g_fb_phys = fb_phys;
    g_fb_pitch = pitch;
    g_gui_ready = 1;
    serial_write("[serial] gui_init: framebuffer ready\n");
    return 1;
}

/* Raw physical framebuffer info -- used by the SYS_MAP_FB syscall to
 * map these exact physical frames into whichever task's own directory
 * asks for them, at syscall-time. Mapping directly into the calling
 * task's directory (rather than pre-installing into the shared kernel
 * directory ahead of time) sidesteps any "was this task created
 * before or after gui_init() ran" ordering question entirely -- it
 * always works, regardless of when graphics happens to get requested. */
void gui_get_fb_phys_info(unsigned int *phys, unsigned int *width,
                           unsigned int *height, unsigned int *pitch) {
    *phys = g_fb_phys;
    *width = GUI_WIDTH;
    *height = GUI_HEIGHT;
    *pitch = g_fb_pitch;
}

/* Once graphics mode has been engaged, we deliberately never leave
 * it -- see the long comment in vga.c on why "restore standard VGA
 * text mode" turned out to be an unreliable thing to depend on (it
 * requires the hardware to cleanly revert register state that toggling
 * one Bochs DISPI bit doesn't reliably restore), and how the plain
 * console renders through the framebuffer instead once this is true. */
int gui_is_active(void) {
    return g_gui_ready;
}
