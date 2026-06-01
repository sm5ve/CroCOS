/*
 * croco_profile.c — CroCOS QEMU TCG profiling plugin.
 *
 * A single clean-room plugin that combines basic-block execution counting
 * (hotblocks-style) and guest-page access counting (hotpages-style), with an
 * optional region-of-interest (ROI) gate so counting can be scoped to the part
 * of the run you care about — excluding firmware (SeaBIOS) and kernel boot/init.
 *
 * ROI gating: counting is active only while "armed". With no roi_start the
 * plugin is armed from the first instruction (whole-run profile). With
 *   roi_start=0x<vaddr>  it arms the first time the guest executes that PC, and
 *   roi_end=0x<vaddr>    disarms when that PC executes.
 * Point roi_start at, e.g., the kernel's stress-loop entry to skip everything
 * before the workload.
 *
 * Output is byte-compatible with the upstream hotblocks/hotpages reports
 * (same headers/columns), so tools/qemu_profile_report.py parses it unchanged:
 *     collected <N> entries in the hash table
 *     pc, tcount, icount, ecount
 *     0x<pc>, <tcount>, <icount>, <ecount>
 *     Addr, RCPUs, Reads, WCPUs, Writes
 *     0x<page>, 0x<rcpus>, <reads>, 0x<wcpus>, <writes>
 *
 * Assumes single-threaded TCG (-accel tcg,thread=single); state is plain
 * globals with no cross-thread synchronization, matching how the script runs it.
 *
 * Args: roi_start=0x.., roi_end=0x.., io=on|off (track I/O pages instead of
 * RAM, default off), pagesize=N, blocklimit=N (default 100), pagelimit=N
 * (default 100).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* ── ROI state ─────────────────────────────────────────────────────────── */
static bool     armed = true;     /* flipped to false in install if roi_start set */
static bool     have_roi_start;
static bool     have_roi_end;
static uint64_t roi_start;
static uint64_t roi_end;

/* ── Config ────────────────────────────────────────────────────────────── */
static uint64_t page_size = 4096;
static uint64_t page_mask = 4095;
static bool     track_io;
static int      block_limit = 100;
static int      page_limit = 100;

/* ── Block counting ────────────────────────────────────────────────────── */
typedef struct {
    uint64_t start_addr;
    int      trans_count;
    unsigned long insns;
    uint64_t exec_count;
} BlockCount;

/* ── Page counting ─────────────────────────────────────────────────────── */
typedef struct {
    uint64_t page_address;
    int      cpu_read;
    int      cpu_write;
    uint64_t reads;
    uint64_t writes;
} PageCount;

static GHashTable *blocks;   /* (pc ^ insns) -> BlockCount  */
static GHashTable *pages;    /* page_address -> PageCount    */

/* ── ROI markers ───────────────────────────────────────────────────────── */
static void arm_cb(unsigned int cpu_index, void *udata)
{
    (void)cpu_index; (void)udata;
    armed = true;
}

static void disarm_cb(unsigned int cpu_index, void *udata)
{
    (void)cpu_index; (void)udata;
    armed = false;
}

/* ── Counting callbacks ────────────────────────────────────────────────── */
static void vcpu_tb_exec(unsigned int cpu_index, void *udata)
{
    (void)cpu_index;
    if (!armed) {
        return;
    }
    ((BlockCount *)udata)->exec_count++;
}

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t meminfo,
                     uint64_t vaddr, void *udata)
{
    (void)udata;
    if (!armed) {
        return;
    }

    struct qemu_plugin_hwaddr *hwaddr = qemu_plugin_get_hwaddr(meminfo, vaddr);
    uint64_t page;

    if (track_io) {
        if (hwaddr && qemu_plugin_hwaddr_is_io(hwaddr)) {
            page = vaddr;
        } else {
            return;
        }
    } else {
        if (hwaddr && !qemu_plugin_hwaddr_is_io(hwaddr)) {
            page = (uint64_t)qemu_plugin_hwaddr_phys_addr(hwaddr);
        } else {
            page = vaddr;
        }
    }
    page &= ~page_mask;

    PageCount *c = g_hash_table_lookup(pages, GUINT_TO_POINTER(page));
    if (!c) {
        c = g_new0(PageCount, 1);
        c->page_address = page;
        g_hash_table_insert(pages, GUINT_TO_POINTER(page), c);
    }
    if (qemu_plugin_mem_is_store(meminfo)) {
        c->writes++;
        c->cpu_write |= (1 << cpu_index);
    } else {
        c->reads++;
        c->cpu_read |= (1 << cpu_index);
    }
}

/* ── Translation ───────────────────────────────────────────────────────── */
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    size_t insns = qemu_plugin_tb_n_insns(tb);
    uint64_t hash = pc ^ insns;

    BlockCount *cnt = g_hash_table_lookup(blocks, (gconstpointer)hash);
    if (cnt) {
        cnt->trans_count++;
    } else {
        cnt = g_new0(BlockCount, 1);
        cnt->start_addr = pc;
        cnt->trans_count = 1;
        cnt->insns = insns;
        g_hash_table_insert(blocks, (gpointer)hash, cnt);
    }
    qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec,
                                         QEMU_PLUGIN_CB_NO_REGS, cnt);

    /* Per-instruction work: memory callbacks + ROI arm/disarm markers. */
    for (size_t i = 0; i < insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t ivaddr = qemu_plugin_insn_vaddr(insn);

        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem, QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);

        if (have_roi_start && ivaddr == roi_start) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, arm_cb,
                                                   QEMU_PLUGIN_CB_NO_REGS, NULL);
        }
        if (have_roi_end && ivaddr == roi_end) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, disarm_cb,
                                                   QEMU_PLUGIN_CB_NO_REGS, NULL);
        }
    }
}

/* ── Reporting ─────────────────────────────────────────────────────────── */
static gint cmp_blocks(gconstpointer a, gconstpointer b)
{
    uint64_t ca = ((const BlockCount *)a)->exec_count;
    uint64_t cb = ((const BlockCount *)b)->exec_count;
    return ca > cb ? -1 : (ca < cb ? 1 : 0);
}

static gint cmp_pages(gconstpointer a, gconstpointer b)
{
    uint64_t ca = ((const PageCount *)a)->reads + ((const PageCount *)a)->writes;
    uint64_t cb = ((const PageCount *)b)->reads + ((const PageCount *)b)->writes;
    return ca > cb ? -1 : (ca < cb ? 1 : 0);
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    (void)id; (void)p;
    g_autoptr(GString) report = g_string_new("");

    if (have_roi_start) {
        g_string_append_printf(report,
            "# croco_profile: region-of-interest run (roi_start=0x%016"PRIx64
            "%s)\n", roi_start,
            have_roi_end ? "" : ", roi_end=<run-end>");
    } else {
        g_string_append(report, "# croco_profile: whole-run profile (no ROI)\n");
    }

    /* hotblocks-compatible section */
    g_string_append_printf(report, "collected %u entries in the hash table\n",
                           g_hash_table_size(blocks));
    GList *bl = g_list_sort(g_hash_table_get_values(blocks), cmp_blocks);
    g_string_append(report, "pc, tcount, icount, ecount\n");
    int i = 0;
    for (GList *it = bl; it && i < block_limit; it = it->next, i++) {
        BlockCount *r = it->data;
        g_string_append_printf(report, "0x%016"PRIx64", %d, %lu, %"PRId64"\n",
                               r->start_addr, r->trans_count, r->insns,
                               r->exec_count);
    }
    g_list_free(bl);

    /* hotpages-compatible section */
    g_string_append(report, "Addr, RCPUs, Reads, WCPUs, Writes\n");
    GList *pl = g_list_sort(g_hash_table_get_values(pages), cmp_pages);
    i = 0;
    for (GList *it = pl; it && i < page_limit; it = it->next, i++) {
        PageCount *r = it->data;
        g_string_append_printf(report,
                               "0x%016"PRIx64", 0x%04x, %"PRId64", 0x%04x, %"PRId64"\n",
                               r->page_address, r->cpu_read, r->reads,
                               r->cpu_write, r->writes);
    }
    g_list_free(pl);

    qemu_plugin_outs(report->str);
}

/* ── Install ───────────────────────────────────────────────────────────── */
static bool parse_u64(const char *k, const char *v, uint64_t *out)
{
    if (!v) {
        fprintf(stderr, "croco_profile: %s needs a value\n", k);
        return false;
    }
    *out = g_ascii_strtoull(v, NULL, 0);
    return true;
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    (void)info;
    for (int n = 0; n < argc; n++) {
        g_auto(GStrv) t = g_strsplit(argv[n], "=", 2);
        if (g_strcmp0(t[0], "roi_start") == 0) {
            if (!parse_u64(t[0], t[1], &roi_start)) return -1;
            have_roi_start = true;
            armed = false;            /* wait for the marker */
        } else if (g_strcmp0(t[0], "roi_end") == 0) {
            if (!parse_u64(t[0], t[1], &roi_end)) return -1;
            have_roi_end = true;
        } else if (g_strcmp0(t[0], "io") == 0) {
            if (!qemu_plugin_bool_parse(t[0], t[1], &track_io)) return -1;
        } else if (g_strcmp0(t[0], "pagesize") == 0) {
            if (!parse_u64(t[0], t[1], &page_size)) return -1;
        } else if (g_strcmp0(t[0], "blocklimit") == 0) {
            uint64_t v; if (!parse_u64(t[0], t[1], &v)) return -1; block_limit = (int)v;
        } else if (g_strcmp0(t[0], "pagelimit") == 0) {
            uint64_t v; if (!parse_u64(t[0], t[1], &v)) return -1; page_limit = (int)v;
        } else {
            fprintf(stderr, "croco_profile: unknown option '%s'\n", argv[n]);
            return -1;
        }
    }
    page_mask = page_size - 1;
    blocks = g_hash_table_new(NULL, g_direct_equal);
    pages = g_hash_table_new(NULL, g_direct_equal);

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
