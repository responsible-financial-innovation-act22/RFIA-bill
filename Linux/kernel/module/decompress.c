// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2021 Google LLC.
 */

#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>

#include "internal.h"

static int module_extend_max_pages(struct load_info *info, unsigned int extent)
{
	struct page **new_pages;

	new_pages = kvmalloc_array(info->max_pages + extent,
				   sizeof(info->pages), GFP_KERNEL);
	if (!new_pages)
		return -ENOMEM;

	memcpy(new_pages, info->pages, info->max_pages * sizeof(info->pages));
	kvfree(info->pages);
	info->pages = new_pages;
	info->max_pages += extent;

	return 0;
}

static struct page *module_get_next_page(struct load_info *info)
{
	struct page *page;
	int error;

	if (info->max_pages == info->used_pages) {
		error = module_extend_max_pages(info, info->used_pages);
		if (error)
			return ERR_PTR(error);
	}

	page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
	if (!page)
		return ERR_PTR(-ENOMEM);

	info->pages[info->used_pages++] = page;
	return page;
}

#ifdef CONFIG_MODULE_COMPRESS_GZIP
#include <linux/zlib.h>
#define MODULE_COMPRESSION	gzip
#define MODULE_DECOMPRESS_FN	module_gzip_decompress

/*
 * Calculate length of the header which consists of signature, header
 * flags, time stamp and operating system ID (10 bytes total), plus
 * an optional filename.
 */
static size_t module_gzip_header_len(const u8 *buf, size_t size)
{
	const u8 signature[] = { 0x1f, 0x8b, 0x08 };
	size_t len = 10;

	if (size < len || memcmp(buf, signature, sizeof(signature)))
		return 0;

	if (buf[3] & 0x08) {
		do {
			/*
			 * If we can't find the end of the file name we must
			 * be dealing with a corrupted file.
			 */
			if (len == size)
				return 0;
		} while (buf[len++] != '\0');
	}

	return len;
}

static ssize_t module_gzip_decompress(struct load_info *info,
				      const void *buf, size_t size)
{
	struct z_stream_s s = { 0 };
	size_t new_size = 0;
	size_t gzip_hdr_len;
	ssize_t retval;
	int rc;

	gzip_hdr_len = module_gzip_header_len(buf, size);
	if (!gzip_hdr_len) {
		pr_err("not a gzip compressed module\n");
		return -EINVAL;
	}

	s.next_in = buf + gzip_hdr_len;
	s.avail_in = size - gzip_hdr_len;

	s.workspace = kmalloc(zlib_inflate_workspacesize(), GFP_KERNEL);
	if (!s.workspace)
		return -ENOMEM;

	rc = zlib_inflateInit2(&s, -MAX_WBITS);
	if (rc != Z_OK) {
		pr_err("failed to initialize decompressor: %d\n", rc);
		retval = -EINVAL;
		goto out;
	}

	do {
		struct page *page = module_get_next_page(info);

		if (!page) {
			retval = -ENOMEM;
			goto out_inflate_end;
		}

		s.next_out = kmap(page);
		s.avail_out = PAGE_SIZE;
		rc = zlib_inflate(&s, 0);
		kunmap(page);

		new_size += PAGE_SIZE - s.avail_out;
	} while (rc == Z_OK);

	if (rc != Z_STREAM_END) {
		pr_err("decompression failed with status %d\n", rc);
		retval = -EINVAL;
		goto out_inflate_end;
	}

	retval = new_size;

out_inflate_end:
	zlib_inflateEnd(&s);
out:
	kfree(s.workspace);
	return retval;
}
#elif CONFIG_MODULE_COMPRESS_XZ
#include <linux/xz.h>
#define MODULE_COMPRESSION	xz
#define MODULE_DECOMPRESS_FN	module_xz_decompress

static ssize_t module_xz_decompress(struct load_info *info,
				    const void *buf, size_t size)
{
	static const u8 signature[] = { 0xfd, '7', 'z', 'X', 'Z', 0 };
	struct xz_dec *xz_dec;
	struct xz_buf xz_buf;
	enum xz_ret xz_ret;
	size_t new_size = 0;
	ssize_t retval;

	if (size < sizeof(signature) ||
	    memcmp(buf, signature, sizeof(signature))) {
		pr_err("not an xz compressed module\n");
		return -EINVAL;
	}

	xz_dec = xz_dec_init(XZ_DYNALLOC, (u32)-1);
	if (!xz_dec)
		return -ENOMEM;

	xz_buf.in_size = size;
	xz_buf.in = buf;
	xz_buf.in_pos = 0;

	do {
		struct page *page = module_get_next_page(info);

		if (!page) {
			retval = -ENOMEM;
			goto out;
		}

		xz_buf.out = kmap(page);
		xz_buf.out_pos = 0;
		xz_buf.out_size = PAGE_SIZE;
		xz_ret = xz_dec_run(xz_dec, &xz_buf);
		kunmap(page);

		new_size += xz_buf.out_pos;
	} while (xz_buf.out_pos == PAGE_SIZE && xz_ret == XZ_OK);

	if (xz_ret != XZ_STREAM_END) {
		pr_err("decompression failed with status %d\n", xz_ret);
		retval = -EINVAL;
		goto out;
	}

	retval = new_size;

 out:
	xz_dec_end(xz_dec);
	return retval;
}
#else
#error "Unexpected configuration for CONFIG_MODULE_DECOMPRESS"
#endif

int module_decompress(struct load_info *info, const void *buf, size_t size)
{
	unsigned int n_pages;
	ssize_t data_size;
	int error;

	/*
	 * Start with number of pages twice as big as needed for
	 * compressed data.
	 */
	n_pages = DIV_ROUND_UP(size, PAGE_SIZE) * 2;
	error = module_extend_max_pages(info, n_pages);

	data_size = MODULE_DECOMPRESS_FN(info, buf, size);
	if (data_size < 0) {
		error = data_size;
		goto err;
	}

	info->hdr = vmap(info->pages, info->used_pages, VM_MAP, PAGE_KERNEL);
	if (!info->hdr) {
		error = -ENOMEM;
		goto err;
	}

	info->len = data_size;
	return 0;

err:
	module_decompress_cleanup(info);
	return error;
}

void module_decompress_cleanup(struct load_info *info)
{
	int i;

	if (info->hdr)
		vunmap(info->hdr);

	for (i = 0; i < info->used_pages; i++)
		__free_page(info->pages[i]);

	kvfree(info->pages);

	info->pages = NULL;
	info->max_pages = info->used_pages = 0;
}

#ifdef CONFIG_SYSFS
static ssize_t compression_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", __stringify(MODULE_COMPRESSION));
}

static struct kobj_attribute module_compression_attr = __ATTR_RO(compression);

static int __init module_decompress_sysfs_init(void)
{
	int error;

	error = sysfs_create_file(&module_kset->kobj,
				  &module_compression_attr.attr);
	if (error)
		pr_warn("Failed to create 'compression' attribute");

	return 0;
}
late_initcall(module_decompress_sysfs_init);
#endif
