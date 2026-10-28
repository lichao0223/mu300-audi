// SPDX-License-Identifier: GPL-2.0
/*
 * audio_reserve: take physical ranges out of the page allocator at runtime, for memory the device tree does not
 * reserve and a device needs at fixed addresses (here the audio DSP's, which audio_mem then maps by address).
 *
 *   insmod audio_reserve.ko ranges=0xaf700000,0x900000[;BASE,SIZE...]
 *
 * Pages that are free are taken; on kernels with alloc_contig_range() movable ones are migrated away first. Free
 * pages are found by allocating blocks until they turn up, holding the others for the moment: never below the high
 * watermark plus HEADROOM, so the rest of the system never runs short meanwhile. What is still in use stays missing (/sys/module/audio_reserve/parameters/missing); writing 1 to .../retry tries
 * again. Pages taken are never given back: the module cannot be unloaded. A range the kernel itself reserved
 * already (every page PageReserved) counts as taken.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/bitmap.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/version.h>

#ifdef MAX_PAGE_ORDER
#define TOP_ORDER MAX_PAGE_ORDER
#else
#define TOP_ORDER (MAX_ORDER - 1)
#endif

#define MAX_RANGES 4
#define HEADROOM ((64UL << 20) >> PAGE_SHIFT)

struct ar_range {
	unsigned long start, end;	/* pfns, [start, end) */
	unsigned long *got;
	bool kernel_reserved;
};

static struct ar_range rg[MAX_RANGES];
static int nr;
static unsigned long missing_pages;
static DEFINE_MUTEX(lock);
static LIST_HEAD(hold);
static struct zone *zone;

static char *ranges = "";
module_param(ranges, charp, 0444);
MODULE_PARM_DESC(ranges, "BASE,SIZE[;BASE,SIZE...] physical ranges to take");

static int missing_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%lu\n", missing_pages);
}
static const struct kernel_param_ops missing_ops = { .get = missing_get };
module_param_cb(missing, &missing_ops, NULL, 0444);
MODULE_PARM_DESC(missing, "pages of the ranges not taken (0: all of them are this module's)");

static struct ar_range *find(unsigned long pfn)
{
	int i;

	for (i = 0; i < nr; i++)
		if (pfn >= rg[i].start && pfn < rg[i].end)
			return &rg[i];
	return NULL;
}

static bool overlaps(unsigned long s, unsigned long e)
{
	int i;

	for (i = 0; i < nr; i++)
		if (s < rg[i].end && e > rg[i].start)
			return true;
	return false;
}

static void got(struct ar_range *r, unsigned long pfn)
{
	if (!test_and_set_bit(pfn - r->start, r->got))
		missing_pages--;
}

static void hold_block(struct page *p, unsigned int order)
{
	set_page_private(p, order);
	list_add(&p->lru, &hold);
}

/* a block the allocator handed out: the pages of the ranges are kept, the rest held until the sweep ends */
static void take(struct page *page, unsigned int order)
{
	unsigned long pfn = page_to_pfn(page), n = 1UL << order, i;
	struct ar_range *r;

	if (!overlaps(pfn, pfn + n)) {
		hold_block(page, order);
		return;
	}
	if (order)
		split_page(page, order);
	for (i = 0; i < n; i++) {
		r = find(pfn + i);
		if (r && !test_bit(pfn + i - r->start, r->got))
			got(r, pfn + i);
		else
			hold_block(page + i, 0);
	}
}

/* is there room to hold another block of ORDER? */
static bool room(unsigned int order)
{
	return zone_page_state(zone, NR_FREE_PAGES) > high_wmark_pages(zone) + HEADROOM + (1UL << order);
}

/* every free block the allocator gives without reclaim or reserves, largest first, until the ranges are taken or
 * there is no room left */
static long sweep(void *arg)
{
	gfp_t gfp = (gfp_t)(unsigned long)arg;
	int order;
	struct page *p;

	for (order = TOP_ORDER; order >= 0 && missing_pages; order--) {
		while (missing_pages && room(order)) {
			p = alloc_pages(gfp, order);
			if (!p)
				break;
			take(p, order);
		}
	}
	return 0;
}

static void release_hold(void)
{
	struct page *p, *t;

	list_for_each_entry_safe(p, t, &hold, lru) {
		unsigned int order = page_private(p);

		list_del(&p->lru);
		set_page_private(p, 0);
		__free_pages(p, order);
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0) && IS_ENABLED(CONFIG_CONTIG_ALLOC)
#include <linux/delay.h>
/*
 * Migrate what can be moved. alloc_contig_range() isolates whole MAX_ORDER blocks and fails on any page in them it
 * cannot move - this module's own included - so it runs once per block, on the part of the ranges in it, after
 * the pages taken there so far went back (nothing uses them while pages are missing: audio_mem waits for 0).
 */
static void contig(void)
{
	unsigned long b, lo, hi, pfn;
	struct ar_range *r;
	int i, try;

	for (i = 0; i < nr; i++) {
		for (b = ALIGN_DOWN(rg[i].start, MAX_ORDER_NR_PAGES); b < rg[i].end; b += MAX_ORDER_NR_PAGES) {
			lo = max(b, rg[i].start);
			hi = min(b + MAX_ORDER_NR_PAGES, rg[i].end);
			/* the ranges next to this one in the same block, as one span */
			while (lo > b && (r = find(lo - 1)))
				lo = max(b, r->start);
			while (hi < b + MAX_ORDER_NR_PAGES && (r = find(hi)))
				hi = min(b + MAX_ORDER_NR_PAGES, r->end);
			if (lo < rg[i].start)
				continue;	/* done with the range that starts the span */
			for (pfn = lo; pfn < hi; pfn++) {
				r = find(pfn);
				if (r->kernel_reserved || !test_bit(pfn - r->start, r->got))
					break;
			}
			if (pfn == hi)
				continue;	/* all of it taken already */
			for (pfn = lo; pfn < hi; pfn++) {
				r = find(pfn);
				if (r->kernel_reserved || !test_bit(pfn - r->start, r->got))
					continue;
				clear_bit(pfn - r->start, r->got);
				missing_pages++;
				__free_page(pfn_to_page(pfn));
			}
			/* (a page under writeback or on its way to the LRU fails it for a moment) */
			for (try = 0; try < 3; try++) {
				if (!alloc_contig_range(lo, hi, MIGRATE_MOVABLE, GFP_KERNEL | __GFP_NOWARN))
					break;
				msleep(20);
			}
			if (try == 3)
				continue;
			for (pfn = lo; pfn < hi; pfn++)
				got(find(pfn), pfn);
		}
	}
}
#else
static void contig(void) { }
#endif

/* no line of a page taken may stay dirty in the cacheable linear map: the DSP and audio_mem's uncached mapping
 * would see it written back over them later. Only the runs of pages taken: the unmap invalidates, which on a page
 * still in use would throw away its owner's data, and faults where that page is mapped read-only (a retry with
 * pages missing oopsed so on 7.3) */
static void clean(void)
{
	struct platform_device *pd;
	struct device *dev;
	unsigned long s, e, n;
	dma_addr_t a;
	int i;

	pd = platform_device_register_simple("audio_reserve", -1, NULL, 0);
	if (IS_ERR(pd))
		return;
	dev = &pd->dev;
	if (!dev->dma_mask)
		dev->dma_mask = &dev->coherent_dma_mask;
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)))
		goto out;
	for (i = 0; i < nr; i++) {
		if (rg[i].kernel_reserved)
			continue;
		n = rg[i].end - rg[i].start;
		for (s = find_first_bit(rg[i].got, n); s < n; s = find_next_bit(rg[i].got, n, e)) {
			e = find_next_zero_bit(rg[i].got, n, s);
			a = dma_map_page(dev, pfn_to_page(rg[i].start + s), 0, (e - s) << PAGE_SHIFT, DMA_BIDIRECTIONAL);
			if (dma_mapping_error(dev, a)) {
				pr_warn("could not clean the cache of range %d\n", i);
				continue;
			}
			dma_unmap_page(dev, a, (e - s) << PAGE_SHIFT, DMA_BIDIRECTIONAL);
		}
	}
out:
	platform_device_unregister(pd);
}

static void report(void)
{
	unsigned long pfn, n[6] = { 0 };
	int i, shown = 0;

	for (i = 0; i < nr; i++) {
		for (pfn = rg[i].start; pfn < rg[i].end; pfn++) {
			struct page *p;

			if (test_bit(pfn - rg[i].start, rg[i].got))
				continue;
			p = pfn_to_page(pfn);
			if (PageReserved(p))
				n[0]++;
			else if (PageSlab(p))
				n[1]++;
			else if (PageLRU(p) && PageAnon(p))
				n[2]++;
			else if (PageLRU(p))
				n[3]++;
			else if (PageBuddy(p))
				n[4]++;
			else
				n[5]++;
			if (shown++ < 8)
				pr_info("pfn %#lx in use: count %d\n", pfn, page_count(p));
		}
	}
	pr_warn("%lu pages not taken: reserved %lu slab %lu anon %lu file %lu free %lu other %lu\n",
		missing_pages, n[0], n[1], n[2], n[3], n[4], n[5]);
}

static void claim(void)
{
	static const gfp_t types[] = { __GFP_MOVABLE, __GFP_RECLAIMABLE, 0 };
	const gfp_t base = __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	unsigned long before = missing_pages;
	int i, cpu;

	contig();
	for (i = 0; i < ARRAY_SIZE(types) && missing_pages; i++)
		sweep((void *)(unsigned long)(base | types[i]));
	/* what sits in the other CPUs' per-cpu lists */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		if (!missing_pages)
			break;
		for (i = 0; i < ARRAY_SIZE(types) && missing_pages; i++)
			work_on_cpu(cpu, sweep, (void *)(unsigned long)(base | types[i]));
	}
	cpus_read_unlock();
	release_hold();
	if (before != missing_pages)
		clean();
	if (missing_pages)
		report();
	else
		pr_info("all %d range(s) taken\n", nr);
}

static int retry_set(const char *val, const struct kernel_param *kp)
{
	mutex_lock(&lock);
	if (missing_pages)
		claim();
	mutex_unlock(&lock);
	return missing_pages ? -EBUSY : 0;
}
static const struct kernel_param_ops retry_ops = { .set = retry_set };
module_param_cb(retry, &retry_ops, NULL, 0200);
MODULE_PARM_DESC(retry, "write anything: try again to take the pages still missing");

static int parse(void)
{
	char *p = ranges, *next;
	unsigned long long base, size;
	unsigned long pfn;
	int i;

	while (p && *p) {
		if (nr == MAX_RANGES)
			return -E2BIG;
		base = memparse(p, &next);
		if (next == p || *next != ',')
			return -EINVAL;
		p = next + 1;
		size = memparse(p, &next);
		if (next == p || !size || !PAGE_ALIGNED(base) || !PAGE_ALIGNED(size))
			return -EINVAL;
		p = *next == ';' ? next + 1 : next;
		rg[nr].start = PHYS_PFN(base);
		rg[nr].end = PHYS_PFN(base + size);
		for (i = 0; i < nr; i++)
			if (rg[nr].start < rg[i].end && rg[nr].end > rg[i].start)
				return -EINVAL;
		rg[nr].got = bitmap_zalloc(rg[nr].end - rg[nr].start, GFP_KERNEL);
		if (!rg[nr].got)
			return -ENOMEM;
		nr++;
	}
	if (!nr)
		return -EINVAL;
	for (i = 0; i < nr; i++) {
		unsigned long res = 0;

		for (pfn = rg[i].start; pfn < rg[i].end; pfn++) {
			if (!pfn_valid(pfn) || page_zone(pfn_to_page(pfn)) != page_zone(pfn_to_page(rg[i].start))) {
				pr_err("range %d: pfn %#lx is not memory of one zone\n", i, pfn);
				return -EINVAL;
			}
			res += PageReserved(pfn_to_page(pfn));
		}
		if (zone && page_zone(pfn_to_page(rg[i].start)) != zone) {
			pr_err("range %d is in another zone than range 0\n", i);
			return -EINVAL;
		}
		zone = page_zone(pfn_to_page(rg[i].start));
		missing_pages += rg[i].end - rg[i].start;
		if (res == rg[i].end - rg[i].start) {
			/* the kernel reserved it at boot */
			rg[i].kernel_reserved = true;
			bitmap_fill(rg[i].got, rg[i].end - rg[i].start);
			missing_pages -= res;
		}
	}
	return 0;
}

static int __init audio_reserve_init(void)
{
	int ret = parse(), i;

	if (ret) {
		pr_err("ranges=%s: %d\n", ranges, ret);
		for (i = 0; i < MAX_RANGES; i++)
			bitmap_free(rg[i].got);
		return ret;
	}
	mutex_lock(&lock);
	if (missing_pages)
		claim();
	else
		pr_info("the kernel reserved the range(s) already\n");
	mutex_unlock(&lock);
	return 0;
}
module_init(audio_reserve_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Take fixed physical ranges out of the page allocator at runtime");
