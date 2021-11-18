#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "context.h"
#include <EGL/eglext.h>
#include "linux-dmabuf-unstable-v1.h"

// --- dmabuf_frame event handlers ---

static void dmabuf_frame_event_frame(
    void * data, struct zwlr_export_dmabuf_frame_v1 * frame,
    uint32_t width, uint32_t height, uint32_t x, uint32_t y,
    uint32_t buffer_flags, uint32_t frame_flags, uint32_t format,
    uint32_t mod_high, uint32_t mod_low, uint32_t num_objects
) {
    ctx_t * ctx = (ctx_t *)data;
    log_debug(ctx, "dmabuf_frame: received %dx%d frame with %d objects\n", width, height, num_objects);
    if (ctx->mirror.state != STATE_WAIT_FRAME) {
        log_error("dmabuf_frame: got frame while in state %d\n", ctx->mirror.state);
        exit_fail(ctx);
    } else if (num_objects > 4) {
        log_error("dmabuf_frame: got frame with more than 4 objects\n");
        exit_fail(ctx);
    }

    uint32_t unhandled_buffer_flags = buffer_flags & ~(
        ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT
    );
    if (unhandled_buffer_flags != 0) {
        log_error("dmabuf_frame: frame uses unhandled buffer flags, buffer_flags = {");
        if (buffer_flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT) fprintf(stderr, "Y_INVERT, ");
        if (buffer_flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED) fprintf(stderr, "INTERLACED, ");
        if (buffer_flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST) fprintf(stderr, "BOTTOM_FIRST, ");
        fprintf(stderr, "}\n");
    }

    uint32_t unhandled_frame_flags = frame_flags & ~(
        ZWLR_EXPORT_DMABUF_FRAME_V1_FLAGS_TRANSIENT
    );
    if (unhandled_frame_flags != 0) {
        log_error("dmabuf_frame: frame uses unhandled frame flags, frame_flags = {");
        if (frame_flags & ZWLR_EXPORT_DMABUF_FRAME_V1_FLAGS_TRANSIENT) fprintf(stderr, "TRANSIENT, ");
        fprintf(stderr, "}\n");
    }

    ctx->mirror.width = width;
    ctx->mirror.height = height;
    ctx->mirror.x = x;
    ctx->mirror.y = y;
    ctx->mirror.buffer_flags = buffer_flags;
    ctx->mirror.frame_flags = frame_flags;
    ctx->mirror.format = format;
    ctx->mirror.modifier_high = mod_high;
    ctx->mirror.modifier_low = mod_low;
    ctx->mirror.num_objects = num_objects;

    ctx->mirror.state = STATE_WAIT_OBJECTS;
    ctx->mirror.processed_objects = 0;

    (void)frame;
}

static void dmabuf_frame_event_object(
    void * data, struct zwlr_export_dmabuf_frame_v1 * frame,
    uint32_t index, int32_t fd, uint32_t size,
    uint32_t offset, uint32_t stride, uint32_t plane_index
) {
    ctx_t * ctx = (ctx_t *)data;
    log_debug(ctx, "dmabuf_frame: received %d byte object with plane_index %d\n", size, plane_index);
    if (ctx->mirror.state != STATE_WAIT_OBJECTS) {
        log_error("dmabuf_frame: got object while in state %d\n", ctx->mirror.state);
        exit_fail(ctx);
    } else if (index >= ctx->mirror.num_objects) {
        log_error("dmabuf_frame: got object with out-of-bounds index %d\n", index);
        exit_fail(ctx);
    }

    ctx->mirror.objects[index].fd = fd;
    ctx->mirror.objects[index].size = size;
    ctx->mirror.objects[index].offset = offset;
    ctx->mirror.objects[index].stride = stride;
    ctx->mirror.objects[index].plane_index = plane_index;

    ctx->mirror.processed_objects++;
    if (ctx->mirror.processed_objects == ctx->mirror.num_objects) {
        ctx->mirror.state = STATE_WAIT_READY;
    }

    (void)frame;
}

static void dmabuf_frame_event_ready(
    void * data, struct zwlr_export_dmabuf_frame_v1 * frame,
    uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec
) {
    ctx_t * ctx = (ctx_t *)data;
    log_debug(ctx, "dmabuf_frame: frame is ready\n");
    if (ctx->mirror.state != STATE_WAIT_READY) {
        log_error("dmabuf_frame: got ready while in state %d\n", ctx->mirror.state);
        exit_fail(ctx);
    }

    if (ctx->mirror.frame_image != EGL_NO_IMAGE) {
        log_debug(ctx, "dmabuf_frame: destroying old EGL image\n");
        eglDestroyImage(ctx->egl.display, ctx->mirror.frame_image);
    }

    log_debug(ctx, "dmabuf_frame: creating EGL image from dmabuf\n");
    int i = 0;
    EGLAttrib image_attribs[6 + 10 * 4 + 1];

    image_attribs[i++] = EGL_WIDTH;
    image_attribs[i++] = ctx->mirror.width;
    image_attribs[i++] = EGL_HEIGHT;
    image_attribs[i++] = ctx->mirror.height;
    image_attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    image_attribs[i++] = ctx->mirror.format;

    if (ctx->mirror.num_objects >= 1) {
        image_attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        image_attribs[i++] = ctx->mirror.objects[0].fd;
        image_attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        image_attribs[i++] = ctx->mirror.objects[0].offset;
        image_attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        image_attribs[i++] = ctx->mirror.objects[0].stride;
        image_attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        image_attribs[i++] = ctx->mirror.modifier_low;
        image_attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        image_attribs[i++] = ctx->mirror.modifier_high;
    }

    if (ctx->mirror.num_objects >= 2) {
        image_attribs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
        image_attribs[i++] = ctx->mirror.objects[1].fd;
        image_attribs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
        image_attribs[i++] = ctx->mirror.objects[1].offset;
        image_attribs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
        image_attribs[i++] = ctx->mirror.objects[1].stride;
        image_attribs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
        image_attribs[i++] = ctx->mirror.modifier_low;
        image_attribs[i++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
        image_attribs[i++] = ctx->mirror.modifier_high;
    }

    if (ctx->mirror.num_objects >= 3) {
        image_attribs[i++] = EGL_DMA_BUF_PLANE2_FD_EXT;
        image_attribs[i++] = ctx->mirror.objects[2].fd;
        image_attribs[i++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
        image_attribs[i++] = ctx->mirror.objects[2].offset;
        image_attribs[i++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
        image_attribs[i++] = ctx->mirror.objects[2].stride;
        image_attribs[i++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
        image_attribs[i++] = ctx->mirror.modifier_low;
        image_attribs[i++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
        image_attribs[i++] = ctx->mirror.modifier_high;
    }

    if (ctx->mirror.num_objects >= 4) {
        image_attribs[i++] = EGL_DMA_BUF_PLANE3_FD_EXT;
        image_attribs[i++] = ctx->mirror.objects[3].fd;
        image_attribs[i++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
        image_attribs[i++] = ctx->mirror.objects[3].offset;
        image_attribs[i++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
        image_attribs[i++] = ctx->mirror.objects[3].stride;
        image_attribs[i++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
        image_attribs[i++] = ctx->mirror.modifier_low;
        image_attribs[i++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
        image_attribs[i++] = ctx->mirror.modifier_high;
    }

    image_attribs[i++] = EGL_NONE;

    ctx->mirror.frame_image = eglCreateImage(ctx->egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, image_attribs);
    if (ctx->mirror.frame_image == EGL_NO_IMAGE) {
        log_error("dmabuf_frame: failed to create EGL image from dmabuf\n");
        exit_fail(ctx);
    }

    log_debug(ctx, "dmabuf_frame: binding image to EGL texture\n");
    glBindTexture(GL_TEXTURE_2D, ctx->egl.texture);
    ctx->egl.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ctx->mirror.frame_image);
    ctx->egl.texture_initialized = true;

    log_debug(ctx, "dmabuf_frame: setting buffer flags\n");
    ctx->mirror.invert_y = ctx->mirror.buffer_flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT;

    log_debug(ctx, "dmabuf_frame: setting frame aspect ratio\n");
    ctx->egl.width = ctx->mirror.width;
    ctx->egl.height = ctx->mirror.height;
    resize_viewport_egl(ctx);

    log_debug(ctx, "dmabuf_frame: closing dmabuf fds\n");
    for (unsigned int i = 0; i < ctx->mirror.num_objects; i++) {
        close(ctx->mirror.objects[i].fd);
        ctx->mirror.objects[i].fd = -1;
    }

    zwlr_export_dmabuf_frame_v1_destroy(ctx->mirror.frame);
    ctx->mirror.frame = NULL;
    ctx->mirror.state = STATE_READY;

    (void)frame;
    (void)sec_hi;
    (void)sec_lo;
    (void)nsec;
}

static void dmabuf_frame_event_cancel(
    void * data, struct zwlr_export_dmabuf_frame_v1 * frame,
    enum zwlr_export_dmabuf_frame_v1_cancel_reason reason
) {
    ctx_t * ctx = (ctx_t *)data;
    log_debug(ctx, "dmabuf_frame: frame was canceled\n");

    zwlr_export_dmabuf_frame_v1_destroy(ctx->mirror.frame);
    ctx->mirror.frame = NULL;
    ctx->mirror.state = STATE_CANCELED;

    switch (reason) {
        case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT:
            log_error("dmabuf_frame: permanent cancellation\n");
            exit_fail(ctx);
            break;

        case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_TEMPORARY:
            log_debug(ctx, "dmabuf_frame: temporary cancellation\n");
            break;

        case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_RESIZING:
            log_debug(ctx, "dmabuf_frame: cancellation due to output resize\n");
            break;

        default:
            log_error("dmabuf_frame: unknown cancellation reason %d\n", reason);
            exit_fail(ctx);
            break;
    }

    log_debug(ctx, "dmabuf_frame: closing files\n");
    for (unsigned int i = 0; i < ctx->mirror.num_objects; i++) {
        if (ctx->mirror.objects[i].fd != -1) close(ctx->mirror.objects[i].fd);
        ctx->mirror.objects[i].fd = -1;
    }

    (void)frame;
}

static const struct zwlr_export_dmabuf_frame_v1_listener dmabuf_frame_listener = {
    .frame = dmabuf_frame_event_frame,
    .object = dmabuf_frame_event_object,
    .ready = dmabuf_frame_event_ready,
    .cancel = dmabuf_frame_event_cancel
};

// --- frame_callback event handlers ---

static const struct wl_callback_listener frame_callback_listener;

static void frame_callback_event_done(
    void * data, struct wl_callback * frame_callback, uint32_t msec
) {
    ctx_t * ctx = (ctx_t *)data;

    wl_callback_destroy(ctx->mirror.frame_callback);
    ctx->mirror.frame_callback = NULL;

    log_debug(ctx, "frame_callback: requesting next callback\n");
    ctx->mirror.frame_callback = wl_surface_frame(ctx->wl.surface);
    wl_callback_add_listener(ctx->mirror.frame_callback, &frame_callback_listener, (void *)ctx);

    log_debug(ctx, "frame_callback: rendering frame\n");
    draw_texture_egl(ctx);

    log_debug(ctx, "frame_callback: swapping buffers\n");
    eglSwapInterval(ctx->egl.display, 0);
    if (eglSwapBuffers(ctx->egl.display, ctx->egl.surface) != EGL_TRUE) {
        log_error("frame_callback: failed to swap buffers\n");
        exit_fail(ctx);
    }

    if (ctx->mirror.state != STATE_WAIT_FRAME) {
        log_debug(ctx, "frame_callback: clearing dmabuf_frame state\n");
        ctx->mirror.width = 0;
        ctx->mirror.height = 0;
        ctx->mirror.x = 0;
        ctx->mirror.y = 0;
        ctx->mirror.buffer_flags = 0;
        ctx->mirror.frame_flags = 0;
        ctx->mirror.modifier_high = 0;
        ctx->mirror.modifier_low = 0;
        ctx->mirror.format = 0;
        ctx->mirror.num_objects = 0;

        dmabuf_object_t empty_obj = {
            .fd = -1,
            .size = 0,
            .offset = 0,
            .stride = 0,
            .plane_index = 0
        };
        ctx->mirror.objects[0] = empty_obj;
        ctx->mirror.objects[1] = empty_obj;
        ctx->mirror.objects[2] = empty_obj;
        ctx->mirror.objects[3] = empty_obj;

        ctx->mirror.state = STATE_WAIT_FRAME;
        ctx->mirror.processed_objects = 0;

        log_debug(ctx, "frame_callback: creating wlr_dmabuf_export_frame\n");
        ctx->mirror.frame = zwlr_export_dmabuf_manager_v1_capture_output(
            ctx->wl.dmabuf_manager, ctx->opt.show_cursor, ctx->mirror.current->output
        );
        if (ctx->mirror.frame == NULL) {
            log_error("frame_callback: failed to create wlr_dmabuf_export_frame\n");
            exit_fail(ctx);
        }

        log_debug(ctx, "frame_callback: adding dmabuf_frame event listener\n");
        zwlr_export_dmabuf_frame_v1_add_listener(ctx->mirror.frame, &dmabuf_frame_listener, (void *)ctx);
    }

    (void)frame_callback;
    (void)msec;
}

static const struct wl_callback_listener frame_callback_listener = {
    .done = frame_callback_event_done
};

// --- init_mirror ---

void init_mirror(ctx_t * ctx) {
    log_debug(ctx, "init_mirror: initializing context structure\n");

    ctx->mirror.current = NULL;
    ctx->mirror.frame_callback = NULL;
    ctx->mirror.frame = NULL;
    ctx->mirror.invert_y = false;

    ctx->mirror.width = 0;
    ctx->mirror.height = 0;
    ctx->mirror.x = 0;
    ctx->mirror.y = 0;
    ctx->mirror.buffer_flags = 0;
    ctx->mirror.frame_flags = 0;
    ctx->mirror.modifier_high = 0;
    ctx->mirror.modifier_low = 0;
    ctx->mirror.format = 0;
    ctx->mirror.num_objects = 0;

    dmabuf_object_t empty_obj = {
        .fd = -1,
        .size = 0,
        .offset = 0,
        .stride = 0,
        .plane_index = 0
    };
    ctx->mirror.objects[0] = empty_obj;
    ctx->mirror.objects[1] = empty_obj;
    ctx->mirror.objects[2] = empty_obj;
    ctx->mirror.objects[3] = empty_obj;

    ctx->mirror.frame_image = EGL_NO_IMAGE;

    ctx->mirror.state = STATE_CANCELED;
    ctx->mirror.processed_objects = 0;
    ctx->mirror.initialized = true;

    char * output_name = NULL;
    if (ctx->opt.output != NULL) {
        log_debug(ctx, "init_mirror: searching for output by name\n");
        output_list_node_t * cur = ctx->wl.outputs;
        while (cur != NULL) {
            if (cur->name != NULL && strcmp(cur->name, ctx->opt.output) == 0) {
                ctx->mirror.current = cur;
                output_name = cur->name;
                break;
            }

            cur = cur->next;
        }
    } else if (ctx->opt.has_region) {
        log_debug(ctx, "init_mirror: searching for output by region\n");
        output_list_node_t * cur = ctx->wl.outputs;
        while (cur != NULL) {
            region_t output_region = {
                .x = cur->x, .y = cur->y,
                .width = cur->width, .height = cur->height
            };
            if (region_contains(&ctx->opt.region, &output_region)) {
                ctx->mirror.current = cur;
                output_name = cur->name;
                break;
            }

            cur = cur->next;
        }
    }

    if (ctx->mirror.current == NULL && ctx->opt.output != NULL) {
        log_error("init_mirror: output %s not found\n", ctx->opt.output);
        exit_fail(ctx);
    } else if (ctx->mirror.current == NULL && ctx->opt.has_region) {
        log_error("init_mirror: output for region not found\n");
        exit_fail(ctx);
    } else if (ctx->mirror.current == NULL) {
        log_error("init_mirror: no output or region specified\n");
        exit_fail(ctx);
    } else {
        log_debug(ctx, "init_mirror: found output with name %s\n", output_name);
    }

    if (ctx->opt.has_region) {
        log_debug(ctx, "init_mirror: checking if region in output\n");
        region_t output_region = {
            .x = ctx->mirror.current->x, .y = ctx->mirror.current->y,
            .width = ctx->mirror.current->width, .height = ctx->mirror.current->height
        };
        if (!region_contains(&ctx->opt.region, &output_region)) {
            log_error("init_mirror: output does not contain region\n");
            exit_fail(ctx);
        }

        log_debug(ctx, "init_mirror: clamping region to output bounds\n");
        region_clamp(&ctx->opt.region, &output_region);
    }

    log_debug(ctx, "init_mirror: formatting window title\n");
    char * title = NULL;
    int status = asprintf(&title, "Wayland Output Mirror for %s", output_name);
    if (status == -1) {
        log_error("init_mirror: failed to format window title\n");
        exit_fail(ctx);
    }

    log_debug(ctx, "init_mirror: setting window title\n");
    xdg_toplevel_set_title(ctx->wl.xdg_toplevel, title);
    free(title);

    log_debug(ctx, "init_mirror: requesting render callback\n");
    ctx->mirror.frame_callback = wl_surface_frame(ctx->wl.surface);
    wl_callback_add_listener(ctx->mirror.frame_callback, &frame_callback_listener, (void *)ctx);
}

// --- output_removed_mirror ---

void output_removed_mirror(ctx_t * ctx, output_list_node_t * node) {
    if (!ctx->mirror.initialized) return;
    if (ctx->mirror.current == NULL) return;
    if (ctx->mirror.current != node) return;

    log_error("output_removed_mirror: output disappeared, closing\n");
    exit_fail(ctx);
}

// --- cleanup_mirror ---

void cleanup_mirror(ctx_t *ctx) {
    if (!ctx->mirror.initialized) return;

    log_debug(ctx, "cleanup_mirror: destroying mirror objects\n");
    if (ctx->mirror.frame_callback != NULL) wl_callback_destroy(ctx->mirror.frame_callback);
    if (ctx->mirror.frame != NULL) zwlr_export_dmabuf_frame_v1_destroy(ctx->mirror.frame);
    if (ctx->mirror.frame_image != EGL_NO_IMAGE) eglDestroyImage(ctx->egl.display, ctx->mirror.frame_image);

    for (int i = 0; i < 4; i++) {
        if (ctx->mirror.objects[i].fd != -1) close(ctx->mirror.objects[i].fd);
        ctx->mirror.objects[i].fd = -1;
    }

    ctx->mirror.initialized = false;
}
