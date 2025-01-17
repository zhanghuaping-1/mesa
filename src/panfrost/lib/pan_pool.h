/*
 * © Copyright 2017-2018 Alyssa Rosenzweig
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __PAN_POOL_H__
#define __PAN_POOL_H__

#include <stddef.h>
#include <midgard_pack.h>
#include "pan_bo.h"

#include "util/u_dynarray.h"

/* Represents grow-only memory. It may be owned by the batch (OpenGL) or
 * command pool (Vulkan), or may be unowned for persistent uploads. */

struct pan_pool {
        /* Parent device for allocation */
        struct panfrost_device *dev;

        /* BOs allocated by this pool */
        struct util_dynarray bos;

        /* Current transient BO */
        struct panfrost_bo *transient_bo;

        /* Within the topmost transient BO, how much has been used? */
        unsigned transient_offset;

        /* Label for created BOs */
        const char *label;

        /* BO flags to use in the pool */
        unsigned create_flags;

        /* Minimum size for allocated BOs. */
        size_t slab_size;

        /* Mode of the pool. BO management is in the pool for owned mode, but
         * the consumed for unowned mode. */
        bool owned;
};

/* Reference to pool allocated memory for an unowned pool */

struct pan_pool_ref {
        /* Owning BO */
        struct panfrost_bo *bo;

        /* Mapped GPU VA */
        mali_ptr gpu;
};

/* Take a reference to an allocation pool. Call directly after allocating from
 * an unowned pool for correct operation. */

static inline struct pan_pool_ref
pan_take_ref(struct pan_pool *pool, mali_ptr ptr)
{
        if (!pool->owned)
                panfrost_bo_reference(pool->transient_bo);

        return (struct pan_pool_ref) {
                .gpu = ptr,
                .bo = pool->transient_bo
        };
}

void
panfrost_pool_init(struct pan_pool *pool, void *memctx,
                   struct panfrost_device *dev, unsigned create_flags,
                   size_t slab_size, const char *label, bool prealloc, bool
                   owned);

void
panfrost_pool_cleanup(struct pan_pool *pool);

static inline unsigned
panfrost_pool_num_bos(struct pan_pool *pool)
{
        assert(pool->owned && "pool does not track BOs in unowned mode");
        return util_dynarray_num_elements(&pool->bos, struct panfrost_bo *);
}

void
panfrost_pool_get_bo_handles(struct pan_pool *pool, uint32_t *handles);

/* Represents a fat pointer for GPU-mapped memory, returned from the transient
 * allocator and not used for much else */

struct panfrost_ptr
panfrost_pool_alloc_aligned(struct pan_pool *pool, size_t sz, unsigned alignment);

mali_ptr
panfrost_pool_upload(struct pan_pool *pool, const void *data, size_t sz);

mali_ptr
panfrost_pool_upload_aligned(struct pan_pool *pool, const void *data, size_t sz, unsigned alignment);

struct pan_desc_alloc_info {
        unsigned size;
        unsigned align;
        unsigned nelems;
};

#define PAN_DESC_ARRAY(count, name) \
        { \
                .size = MALI_ ## name ## _LENGTH, \
                .align = MALI_ ## name ## _ALIGN, \
                .nelems = count, \
        }

#define PAN_DESC(name) PAN_DESC_ARRAY(1, name)

#define PAN_DESC_AGGREGATE(...) \
        (struct pan_desc_alloc_info[]) { \
                __VA_ARGS__, \
                { 0 }, \
        }

static inline struct panfrost_ptr
panfrost_pool_alloc_descs(struct pan_pool *pool,
                          const struct pan_desc_alloc_info *descs)
{
        unsigned size = 0;
        unsigned align = descs[0].align;

        for (unsigned i = 0; descs[i].size; i++) {
                assert(!(size & (descs[i].align - 1)));
                size += descs[i].size * descs[i].nelems;
        }

        return panfrost_pool_alloc_aligned(pool, size, align);
}

#define panfrost_pool_alloc_desc(pool, name) \
        panfrost_pool_alloc_descs(pool, PAN_DESC_AGGREGATE(PAN_DESC(name)))

#define panfrost_pool_alloc_desc_array(pool, count, name) \
        panfrost_pool_alloc_descs(pool, PAN_DESC_AGGREGATE(PAN_DESC_ARRAY(count, name)))

#define panfrost_pool_alloc_desc_aggregate(pool, ...) \
        panfrost_pool_alloc_descs(pool, PAN_DESC_AGGREGATE(__VA_ARGS__))

#endif
