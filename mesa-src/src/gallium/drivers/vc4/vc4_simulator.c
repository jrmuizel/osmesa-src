/*
 * Copyright © 2014 Broadcom
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * @file vc4_simulator.c
 *
 * Implements VC4 simulation on top of a non-VC4 GEM fd.
 *
 * This file's goal is to emulate the VC4 ioctls' behavior in the kernel on
 * top of the simpenrose software simulator.  Generally, VC4 driver BOs have a
 * GEM-side copy of their contents and a simulator-side memory area that the
 * GEM contents get copied into during simulation.  Once simulation is done,
 * the simulator's data is copied back out to the GEM BOs, so that rendering
 * appears on the screen as if actual hardware rendering had been done.
 *
 * One of the limitations of this code is that we shouldn't really need a
 * GEM-side BO for non-window-system BOs.  However, do we need unique BO
 * handles for each of our GEM bos so that this file can look up its state
 * from the handle passed in at submit ioctl time (also, a couple of places
 * outside of this file still call ioctls directly on the fd).
 *
 * Another limitation is that BO import doesn't work unless the underlying
 * window system's BO size matches what VC4 is going to use, which of course
 * doesn't work out in practice.  This means that for now, only DRI3 (VC4
 * makes the winsys BOs) is supported, not DRI2 (window system makes the winys
 * BOs).
 */

#ifdef USE_VC4_SIMULATOR

#include <sys/mman.h>
#include "xf86drm.h"
#include "util/u_memory.h"
#include "util/u_mm.h"
#include "util/ralloc.h"

#include "vc4_screen.h"
#include "vc4_cl_dump.h"
#include "vc4_context.h"
#include "kernel/vc4_drv.h"
#include "vc4_simulator_validate.h"
#include "simpenrose/simpenrose.h"

/** Global (across GEM fds) state for the simulator */
static struct vc4_simulator_state {
        mtx_t mutex;

        void *mem;
        ssize_t mem_size;
        struct mem_block *heap;
        struct mem_block *overflow;

        /** Mapping from GEM handle to struct vc4_simulator_bo * */
        struct hash_table *fd_map;

        int refcount;
} sim_state = {
        .mutex = _MTX_INITIALIZER_NP,
};

/** Per-GEM-fd state for the simulator. */
struct vc4_simulator_file {
        int fd;

        /* This is weird -- we make a "vc4_device" per file, even though on
         * the kernel side this is a global.  We do this so that kernel code
         * calling us for BO allocation can get to our screen.
         */
        struct drm_device dev;

        /** Mapping from GEM handle to struct vc4_simulator_bo * */
        struct hash_table *bo_map;
};

/** Wrapper for drm_vc4_bo tracking the simulator-specific state. */
struct vc4_simulator_bo {
        struct drm_vc4_bo base;
        struct vc4_simulator_file *file;

        /** Area for this BO within sim_state->mem */
        struct mem_block *block;
        void *winsys_map;
        uint32_t winsys_stride;

        int handle;
};

static void *
int_to_key(int key)
{
        return (void *)(uintptr_t)key;
}

static struct vc4_simulator_file *
vc4_get_simulator_file_for_fd(int fd)
{
        struct hash_entry *entry = _mesa_hash_table_search(sim_state.fd_map,
                                                           int_to_key(fd + 1));
        return entry ? entry->data : NULL;
}

/* A marker placed just after each BO, then checked after rendering to make
 * sure it's still there.
 */
#define BO_SENTINEL		0xfedcba98

#define PAGE_ALIGN2		12

/**
 * Allocates space in simulator memory and returns a tracking struct for it
 * that also contains the drm_gem_cma_object struct.
 */
static struct vc4_simulator_bo *
vc4_create_simulator_bo(int fd, int handle, unsigned size)
{
        struct vc4_simulator_file *file = vc4_get_simulator_file_for_fd(fd);
        struct vc4_simulator_bo *sim_bo = rzalloc(file,
                                                  struct vc4_simulator_bo);
        struct drm_vc4_bo *bo = &sim_bo->base;
        struct drm_gem_cma_object *obj = &bo->base;
        size = align(size, 4096);

        sim_bo->file = file;
        sim_bo->handle = handle;

        mtx_lock(&sim_state.mutex);
        sim_bo->block = u_mmAllocMem(sim_state.heap, size + 4, PAGE_ALIGN2, 0);
        mtx_unlock(&sim_state.mutex);
        assert(sim_bo->block);

        obj->base.size = size;
        obj->base.dev = &file->dev;
        obj->vaddr = sim_state.mem + sim_bo->block->ofs;
        obj->paddr = simpenrose_hw_addr(obj->vaddr);

        *(uint32_t *)(obj->vaddr + size) = BO_SENTINEL;

        /* A handle of 0 is used for vc4_gem.c internal allocations that
         * don't need to go in the lookup table.
         */
        if (handle != 0) {
                mtx_lock(&sim_state.mutex);
                _mesa_hash_table_insert(file->bo_map, int_to_key(handle), bo);
                mtx_unlock(&sim_state.mutex);
        }

        return sim_bo;
}

static void
vc4_free_simulator_bo(struct vc4_simulator_bo *sim_bo)
{
        struct vc4_simulator_file *sim_file = sim_bo->file;
        struct drm_vc4_bo *bo = &sim_bo->base;
        struct drm_gem_cma_object *obj = &bo->base;

        if (sim_bo->winsys_map)
                munmap(sim_bo->winsys_map, obj->base.size);

        mtx_lock(&sim_state.mutex);
        u_mmFreeMem(sim_bo->block);
        if (sim_bo->handle) {
                struct hash_entry *entry =
                        _mesa_hash_table_search(sim_file->bo_map,
                                                int_to_key(sim_bo->handle));
                _mesa_hash_table_remove(sim_file->bo_map, entry);
        }
        mtx_unlock(&sim_state.mutex);
        ralloc_free(sim_bo);
}

static struct vc4_simulator_bo *
vc4_get_simulator_bo(struct vc4_simulator_file *file, int gem_handle)
{
        mtx_lock(&sim_state.mutex);
        struct hash_entry *entry =
                _mesa_hash_table_search(file->bo_map, int_to_key(gem_handle));
        mtx_unlock(&sim_state.mutex);

        return entry ? entry->data : NULL;
}

struct drm_gem_cma_object *
drm_gem_cma_create(struct drm_device *dev, size_t size)
{
        struct vc4_screen *screen = dev->screen;
        struct vc4_simulator_bo *sim_bo = vc4_create_simulator_bo(screen->fd,
                                                                  0, size);
        return &sim_bo->base.base;
}

static int
vc4_simulator_pin_bos(struct drm_device *dev, struct vc4_job *job,
                      struct vc4_exec_info *exec)
{
        int fd = dev->screen->fd;
        struct vc4_simulator_file *file = vc4_get_simulator_file_for_fd(fd);
        struct drm_vc4_submit_cl *args = exec->args;
        struct vc4_bo **bos = job->bo_pointers.base;

        exec->bo_count = args->bo_handle_count;
        exec->bo = calloc(exec->bo_count, sizeof(void *));
        for (int i = 0; i < exec->bo_count; i++) {
                struct vc4_bo *bo = bos[i];
                struct vc4_simulator_bo *sim_bo =
                        vc4_get_simulator_bo(file, bo->handle);
                struct drm_vc4_bo *drm_bo = &sim_bo->base;
                struct drm_gem_cma_object *obj = &drm_bo->base;

                drm_bo->bo = bo;
#if 0
                fprintf(stderr, "bo hindex %d: %s\n", i, bo->name);
#endif

                vc4_bo_map(bo);
                memcpy(obj->vaddr, bo->map, bo->size);

                exec->bo[i] = obj;

                /* The kernel does this validation at shader create ioctl
                 * time.
                 */
                if (strcmp(bo->name, "code") == 0) {
                        drm_bo->validated_shader = vc4_validate_shader(obj);
                        if (!drm_bo->validated_shader)
                                abort();
                }
        }
        return 0;
}

static int
vc4_simulator_unpin_bos(struct vc4_exec_info *exec)
{
        for (int i = 0; i < exec->bo_count; i++) {
                struct drm_gem_cma_object *obj = exec->bo[i];
                struct drm_vc4_bo *drm_bo = to_vc4_bo(&obj->base);
                struct vc4_bo *bo = drm_bo->bo;

                assert(*(uint32_t *)(obj->vaddr +
                                     obj->base.size) == BO_SENTINEL);
                memcpy(bo->map, obj->vaddr, bo->size);

                if (drm_bo->validated_shader) {
                        free(drm_bo->validated_shader->texture_samples);
                        free(drm_bo->validated_shader);
                }
        }

        free(exec->bo);

        return 0;
}

static void
vc4_dump_to_file(struct vc4_exec_info *exec)
{
        static int dumpno = 0;
        struct drm_vc4_get_hang_state *state;
        struct drm_vc4_get_hang_state_bo *bo_state;
        unsigned int dump_version = 0;

        if (!(vc4_debug & VC4_DEBUG_DUMP))
                return;

        state = calloc(1, sizeof(*state));

        int unref_count = 0;
        list_for_each_entry_safe(struct drm_vc4_bo, bo, &exec->unref_list,
                                 unref_head) {
                unref_count++;
        }

        /* Add one more for the overflow area that isn't wrapped in a BO. */
        state->bo_count = exec->bo_count + unref_count + 1;
        bo_state = calloc(state->bo_count, sizeof(*bo_state));

        char *filename = NULL;
        asprintf(&filename, "vc4-dri-%d.dump", dumpno++);
        FILE *f = fopen(filename, "w+");
        if (!f) {
                fprintf(stderr, "Couldn't open %s: %s", filename,
                        strerror(errno));
                return;
        }

        fwrite(&dump_version, sizeof(dump_version), 1, f);

        state->ct0ca = exec->ct0ca;
        state->ct0ea = exec->ct0ea;
        state->ct1ca = exec->ct1ca;
        state->ct1ea = exec->ct1ea;
        state->start_bin = exec->ct0ca;
        state->start_render = exec->ct1ca;
        fwrite(state, sizeof(*state), 1, f);

        int i;
        for (i = 0; i < exec->bo_count; i++) {
                struct drm_gem_cma_object *cma_bo = exec->bo[i];
                bo_state[i].handle = i; /* Not used by the parser. */
                bo_state[i].paddr = cma_bo->paddr;
                bo_state[i].size = cma_bo->base.size;
        }

        list_for_each_entry_safe(struct drm_vc4_bo, bo, &exec->unref_list,
                                 unref_head) {
                struct drm_gem_cma_object *cma_bo = &bo->base;
                bo_state[i].handle = 0;
                bo_state[i].paddr = cma_bo->paddr;
                bo_state[i].size = cma_bo->base.size;
                i++;
        }

        /* Add the static overflow memory area. */
        bo_state[i].handle = exec->bo_count;
        bo_state[i].paddr = sim_state.overflow->ofs;
        bo_state[i].size = sim_state.overflow->size;
        i++;

        fwrite(bo_state, sizeof(*bo_state), state->bo_count, f);

        for (int i = 0; i < exec->bo_count; i++) {
                struct drm_gem_cma_object *cma_bo = exec->bo[i];
                fwrite(cma_bo->vaddr, cma_bo->base.size, 1, f);
        }

        list_for_each_entry_safe(struct drm_vc4_bo, bo, &exec->unref_list,
                                 unref_head) {
                struct drm_gem_cma_object *cma_bo = &bo->base;
                fwrite(cma_bo->vaddr, cma_bo->base.size, 1, f);
        }

        void *overflow = calloc(1, sim_state.overflow->size);
        fwrite(overflow, 1, sim_state.overflow->size, f);
        free(overflow);

        free(state);
        free(bo_state);
        fclose(f);
}

int
vc4_simulator_flush(struct vc4_context *vc4,
                    struct drm_vc4_submit_cl *args, struct vc4_job *job)
{
        struct vc4_screen *screen = vc4->screen;
        int fd = screen->fd;
        struct vc4_simulator_file *file = vc4_get_simulator_file_for_fd(fd);
        struct vc4_surface *csurf = vc4_surface(vc4->framebuffer.cbufs[0]);
        struct vc4_resource *ctex = csurf ? vc4_resource(csurf->base.texture) : NULL;
        struct vc4_simulator_bo *csim_bo = ctex ? vc4_get_simulator_bo(file, ctex->bo->handle) : NULL;
        uint32_t winsys_stride = ctex ? csim_bo->winsys_stride : 0;
        uint32_t sim_stride = ctex ? ctex->slices[0].stride : 0;
        uint32_t row_len = MIN2(sim_stride, winsys_stride);
        struct vc4_exec_info exec;
        struct drm_device *dev = &file->dev;
        int ret;

        memset(&exec, 0, sizeof(exec));
        list_inithead(&exec.unref_list);

        if (ctex && csim_bo->winsys_map) {
#if 0
                fprintf(stderr, "%dx%d %d %d %d\n",
                        ctex->base.b.width0, ctex->base.b.height0,
                        winsys_stride,
                        sim_stride,
                        ctex->bo->size);
#endif

                for (int y = 0; y < ctex->base.height0; y++) {
                        memcpy(ctex->bo->map + y * sim_stride,
                               csim_bo->winsys_map + y * winsys_stride,
                               row_len);
                }
        }

        exec.args = args;

        ret = vc4_simulator_pin_bos(dev, job, &exec);
        if (ret)
                return ret;

        ret = vc4_cl_validate(dev, &exec);
        if (ret)
                return ret;

        if (vc4_debug & VC4_DEBUG_CL) {
                fprintf(stderr, "RCL:\n");
                vc4_dump_cl(sim_state.mem + exec.ct1ca,
                            exec.ct1ea - exec.ct1ca, true);
        }

        vc4_dump_to_file(&exec);

        if (exec.ct0ca != exec.ct0ea) {
                int bfc = simpenrose_do_binning(exec.ct0ca, exec.ct0ea);
                if (bfc != 1) {
                        fprintf(stderr, "Binning returned %d flushes, should be 1.\n",
                                bfc);
                        fprintf(stderr, "Relocated binning command list:\n");
                        vc4_dump_cl(sim_state.mem + exec.ct0ca,
                                    exec.ct0ea - exec.ct0ca, false);
                        abort();
                }
        }
        int rfc = simpenrose_do_rendering(exec.ct1ca, exec.ct1ea);
        if (rfc != 1) {
                fprintf(stderr, "Rendering returned %d frames, should be 1.\n",
                        rfc);
                fprintf(stderr, "Relocated render command list:\n");
                vc4_dump_cl(sim_state.mem + exec.ct1ca,
                            exec.ct1ea - exec.ct1ca, true);
                abort();
        }

        ret = vc4_simulator_unpin_bos(&exec);
        if (ret)
                return ret;

        list_for_each_entry_safe(struct drm_vc4_bo, bo, &exec.unref_list,
                                 unref_head) {
                struct vc4_simulator_bo *sim_bo = (struct vc4_simulator_bo *)bo;
                struct drm_gem_cma_object *obj = &sim_bo->base.base;
		list_del(&bo->unref_head);
                assert(*(uint32_t *)(obj->vaddr + obj->base.size) ==
                       BO_SENTINEL);
                vc4_free_simulator_bo(sim_bo);
        }

        if (ctex && csim_bo->winsys_map) {
                for (int y = 0; y < ctex->base.height0; y++) {
                        memcpy(csim_bo->winsys_map + y * winsys_stride,
                               ctex->bo->map + y * sim_stride,
                               row_len);
                }
        }

        return 0;
}

/**
 * Map the underlying GEM object from the real hardware GEM handle.
 */
static void *
vc4_simulator_map_winsys_bo(int fd, struct vc4_simulator_bo *sim_bo)
{
        struct drm_vc4_bo *bo = &sim_bo->base;
        struct drm_gem_cma_object *obj = &bo->base;
        int ret;
        void *map;

        struct drm_mode_map_dumb map_dumb = {
                .handle = sim_bo->handle,
        };
        ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
        if (ret != 0) {
                fprintf(stderr, "map ioctl failure\n");
                abort();
        }

        map = mmap(NULL, obj->base.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, map_dumb.offset);
        if (map == MAP_FAILED) {
                fprintf(stderr,
                        "mmap of bo %d (offset 0x%016llx, size %d) failed\n",
                        sim_bo->handle, (long long)map_dumb.offset,
                        (int)obj->base.size);
                abort();
        }

        return map;
}

/**
 * Do fixups after a BO has been opened from a handle.
 *
 * This could be done at DRM_IOCTL_GEM_OPEN/DRM_IOCTL_GEM_PRIME_FD_TO_HANDLE
 * time, but we're still using drmPrimeFDToHandle() so we have this helper to
 * be called afterward instead.
 */
void vc4_simulator_open_from_handle(int fd, uint32_t winsys_stride,
                                    int handle, uint32_t size)
{
        struct vc4_simulator_bo *sim_bo =
                vc4_create_simulator_bo(fd, handle, size);

        sim_bo->winsys_stride = winsys_stride;
        sim_bo->winsys_map = vc4_simulator_map_winsys_bo(fd, sim_bo);
}

/**
 * Simulated ioctl(fd, DRM_VC4_CREATE_BO) implementation.
 *
 * Making a VC4 BO is just a matter of making a corresponding BO on the host.
 */
static int
vc4_simulator_create_bo_ioctl(int fd, struct drm_vc4_create_bo *args)
{
        int ret;
        struct drm_mode_create_dumb create = {
                .width = 128,
                .bpp = 8,
                .height = (args->size + 127) / 128,
        };

        ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
        assert(create.size >= args->size);

        args->handle = create.handle;

        vc4_create_simulator_bo(fd, create.handle, args->size);

        return ret;
}

/**
 * Simulated ioctl(fd, DRM_VC4_CREATE_SHADER_BO) implementation.
 *
 * In simulation we defer shader validation until exec time.  Just make a host
 * BO and memcpy the contents in.
 */
static int
vc4_simulator_create_shader_bo_ioctl(int fd,
                                     struct drm_vc4_create_shader_bo *args)
{
        int ret;
        struct drm_mode_create_dumb create = {
                .width = 128,
                .bpp = 8,
                .height = (args->size + 127) / 128,
        };

        ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
        if (ret)
                return ret;
        assert(create.size >= args->size);

        args->handle = create.handle;

        vc4_create_simulator_bo(fd, create.handle, args->size);

        struct drm_mode_map_dumb map = {
                .handle = create.handle
        };
        ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
        if (ret)
                return ret;

        void *shader = mmap(NULL, args->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                            fd, map.offset);
        memcpy(shader, (void *)(uintptr_t)args->data, args->size);
        munmap(shader, args->size);

        return 0;
}

/**
 * Simulated ioctl(fd, DRM_VC4_MMAP_BO) implementation.
 *
 * We just pass this straight through to dumb mmap.
 */
static int
vc4_simulator_mmap_bo_ioctl(int fd, struct drm_vc4_mmap_bo *args)
{
        int ret;
        struct drm_mode_map_dumb map = {
                .handle = args->handle,
        };

        ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
        args->offset = map.offset;

        return ret;
}

static int
vc4_simulator_gem_close_ioctl(int fd, struct drm_gem_close *args)
{
        /* Free the simulator's internal tracking. */
        struct vc4_simulator_file *file = vc4_get_simulator_file_for_fd(fd);
        struct vc4_simulator_bo *sim_bo = vc4_get_simulator_bo(file,
                                                               args->handle);

        vc4_free_simulator_bo(sim_bo);

        /* Pass the call on down. */
        return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, args);
}

static int
vc4_simulator_get_param_ioctl(int fd, struct drm_vc4_get_param *args)
{
        switch (args->param) {
        case DRM_VC4_PARAM_SUPPORTS_BRANCHES:
        case DRM_VC4_PARAM_SUPPORTS_ETC1:
        case DRM_VC4_PARAM_SUPPORTS_THREADED_FS:
        case DRM_VC4_PARAM_SUPPORTS_FIXED_RCL_ORDER:
                args->value = true;
                return 0;

        case DRM_VC4_PARAM_V3D_IDENT0:
                args->value = 0x02000000;
                return 0;

        case DRM_VC4_PARAM_V3D_IDENT1:
                args->value = 0x00000001;
                return 0;

        default:
                fprintf(stderr, "Unknown DRM_IOCTL_VC4_GET_PARAM(%lld)\n",
                        (long long)args->param);
                abort();
        };
}

int
vc4_simulator_ioctl(int fd, unsigned long request, void *args)
{
        switch (request) {
        case DRM_IOCTL_VC4_CREATE_BO:
                return vc4_simulator_create_bo_ioctl(fd, args);
        case DRM_IOCTL_VC4_CREATE_SHADER_BO:
                return vc4_simulator_create_shader_bo_ioctl(fd, args);
        case DRM_IOCTL_VC4_MMAP_BO:
                return vc4_simulator_mmap_bo_ioctl(fd, args);

        case DRM_IOCTL_VC4_WAIT_BO:
        case DRM_IOCTL_VC4_WAIT_SEQNO:
                /* We do all of the vc4 rendering synchronously, so we just
                 * return immediately on the wait ioctls.  This ignores any
                 * native rendering to the host BO, so it does mean we race on
                 * front buffer rendering.
                 */
                return 0;

        case DRM_IOCTL_VC4_GET_TILING:
        case DRM_IOCTL_VC4_SET_TILING:
                /* Disable these for now, since the sharing with i965 requires
                 * linear buffers.
                 */
                return -1;

        case DRM_IOCTL_VC4_GET_PARAM:
                return vc4_simulator_get_param_ioctl(fd, args);

        case DRM_IOCTL_GEM_CLOSE:
                return vc4_simulator_gem_close_ioctl(fd, args);

        case DRM_IOCTL_GEM_OPEN:
        case DRM_IOCTL_GEM_FLINK:
                return drmIoctl(fd, request, args);
        default:
                fprintf(stderr, "Unknown ioctl 0x%08x\n", (int)request);
                abort();
        }
}

static void
vc4_simulator_init_global(void)
{
        mtx_lock(&sim_state.mutex);
        if (sim_state.refcount++) {
                mtx_unlock(&sim_state.mutex);
                return;
        }

        sim_state.mem_size = 256 * 1024 * 1024;
        sim_state.mem = calloc(sim_state.mem_size, 1);
        if (!sim_state.mem)
                abort();
        sim_state.heap = u_mmInit(0, sim_state.mem_size);

        /* We supply our own memory so that we can have more aperture
         * available (256MB instead of simpenrose's default 64MB).
         */
        simpenrose_init_hardware_supply_mem(sim_state.mem, sim_state.mem_size);

        /* Carve out low memory for tile allocation overflow.  The kernel
         * should be automatically handling overflow memory setup on real
         * hardware, but for simulation we just get one shot to set up enough
         * overflow memory before execution.  This overflow mem will be used
         * up over the whole lifetime of simpenrose (not reused on each
         * flush), so it had better be big.
         */
        sim_state.overflow = u_mmAllocMem(sim_state.heap, 32 * 1024 * 1024,
                                          PAGE_ALIGN2, 0);
        simpenrose_supply_overflow_mem(sim_state.overflow->ofs,
                                       sim_state.overflow->size);

        mtx_unlock(&sim_state.mutex);

        sim_state.fd_map =
                _mesa_hash_table_create(NULL,
                                        _mesa_hash_pointer,
                                        _mesa_key_pointer_equal);
}

void
vc4_simulator_init(struct vc4_screen *screen)
{
        vc4_simulator_init_global();

        screen->sim_file = rzalloc(screen, struct vc4_simulator_file);

        screen->sim_file->bo_map =
                _mesa_hash_table_create(screen->sim_file,
                                        _mesa_hash_pointer,
                                        _mesa_key_pointer_equal);

        mtx_lock(&sim_state.mutex);
        _mesa_hash_table_insert(sim_state.fd_map, int_to_key(screen->fd + 1),
                                screen->sim_file);
        mtx_unlock(&sim_state.mutex);

        screen->sim_file->dev.screen = screen;
}

void
vc4_simulator_destroy(struct vc4_screen *screen)
{
        mtx_lock(&sim_state.mutex);
        if (!--sim_state.refcount) {
                _mesa_hash_table_destroy(sim_state.fd_map, NULL);
                u_mmDestroy(sim_state.heap);
                free(sim_state.mem);
                /* No memsetting it, because it contains the mutex. */
        }
        mtx_unlock(&sim_state.mutex);
}

#endif /* USE_VC4_SIMULATOR */
