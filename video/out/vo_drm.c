/*
 * video output driver for libdrm
 *
 * by rr- <rr-@sakuya.pl>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/vt.h>
#include <unistd.h>

#include <libswscale/swscale.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "osdep/io.h"
#include "osdep/timer.h"
#include "common/msg.h"
#include "sub/osd.h"
#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "vo.h"

#define USE_MASTER 0
#define EVT_RELEASE 1
#define EVT_ACQUIRE 2
#define EVT_INTERRUPT 255
#define BUF_COUNT 2

static int setup_vo_crtc(struct vo *vo);
static void release_vo_crtc(struct vo *vo);

struct modeset_buf {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint8_t *map;
    uint32_t fb;
};

struct modeset_dev {
    struct modeset_buf bufs[BUF_COUNT];
    drmModeModeInfo mode;
    drmModeEncoder *enc;
    uint32_t conn;
    uint32_t crtc;
    int front_buf;
};

struct vt_switcher {
    int tty_fd;
    struct vo *vo;
};

struct priv {
    int fd;
    struct vt_switcher vt_switcher;
    struct modeset_dev *dev;
    drmModeCrtc *old_crtc;

    bool active;
    char *device_path;
    int connector_id;

    int32_t device_w;
    int32_t device_h;
    int32_t x, y;
    struct mp_image *last_input;
    struct mp_image *cur_frame;
    struct mp_rect src;
    struct mp_rect dst;
    struct mp_osd_res osd;
    struct mp_sws_context *sws;
};

static int modeset_open(struct vo *vo, int *out, const char *node)
{
    *out = -1;

    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        MP_ERR(vo, "Cannot open \"%s\": %s.\n", node, mp_strerror(errno));
        return -errno;
    }

    uint64_t has_dumb;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0) {
        MP_ERR(vo, "Device \"%s\" does not support dumb buffers.\n", node);
        return -EOPNOTSUPP;
    }

    *out = fd;
    return 0;
}

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
    if (buf->map) {
        munmap(buf->map, buf->size);
    }
    if (buf->fb) {
        drmModeRmFB(fd, buf->fb);
    }
    if (buf->handle) {
        struct drm_mode_destroy_dumb dreq = {
            .handle = buf->handle,
        };
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
}

static int modeset_create_fb(struct vo *vo, int fd, struct modeset_buf *buf)
{
    int ret = 0;

    buf->handle = 0;

    // create dumb buffer
    struct drm_mode_create_dumb creq = {
        .width = buf->width,
        .height = buf->height,
        .bpp = 32,
    };
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
        MP_ERR(vo, "Cannot create dumb buffer: %s\n", mp_strerror(errno));
        ret = -errno;
        goto end;
    }
    buf->stride = creq.pitch;
    buf->size = creq.size;
    buf->handle = creq.handle;

    // create framebuffer object for the dumb-buffer
    ret = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride,
                       buf->handle, &buf->fb);
    if (ret) {
        MP_ERR(vo, "Cannot create framebuffer: %s\n", mp_strerror(errno));
        ret = -errno;
        goto end;
    }

    // prepare buffer for memory mapping
    struct drm_mode_map_dumb mreq = {
        .handle = buf->handle,
    };
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        MP_ERR(vo, "Cannot map dumb buffer: %s\n", mp_strerror(errno));
        ret = -errno;
        goto end;
    }

    // perform actual memory mapping
    buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        MP_ERR(vo, "Cannot map dumb buffer: %s\n", mp_strerror(errno));
        ret = -errno;
        goto end;
    }

    memset(buf->map, 0, buf->size);

end:
    if (ret == 0) {
        return 0;
    }

    modeset_destroy_fb(fd, buf);
    return ret;
}

static int modeset_find_crtc(struct vo *vo, int fd, drmModeRes *res,
                             drmModeConnector *conn, struct modeset_dev *dev)
{
    for (unsigned int i = 0; i < conn->count_encoders; ++i) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!enc) {
            MP_WARN(vo, "Cannot retrieve encoder %u:%u: %s\n",
                    i, conn->encoders[i], mp_strerror(errno));
            continue;
        }

        // iterate all global CRTCs
        for (unsigned int j = 0; j < res->count_crtcs; ++j) {
            // check whether this CRTC works with the encoder
            if (!(enc->possible_crtcs & (1 << j)))
                continue;

            dev->enc = enc;
            dev->crtc = enc->crtc_id;
            return 0;
        }

        drmModeFreeEncoder(enc);
    }

    MP_ERR(vo, "Connector %u has no suitable CRTC\n", conn->connector_id);
    return -ENOENT;
}

static bool is_connector_valid(struct vo *vo, int conn_id,
                               drmModeConnector *conn, bool silent)
{
    if (!conn) {
        if (!silent) {
            MP_ERR(vo, "Cannot get connector %d: %s\n", conn_id,
                   mp_strerror(errno));
        }
        return false;
    }

    if (conn->connection != DRM_MODE_CONNECTED) {
        if (!silent) {
            MP_ERR(vo, "Connector %d is disconnected\n", conn_id);
        }
        return false;
    }

    if (conn->count_modes == 0) {
        if (!silent) {
            MP_ERR(vo, "Connector %d has no valid modes\n", conn_id);
        }
        return false;
    }

    return true;
}

static int modeset_prepare_dev(struct vo *vo, int fd, int conn_id,
                               struct modeset_dev **out)
{
    struct modeset_dev *dev = NULL;
    drmModeConnector *conn = NULL;

    int ret = 0;
    *out = NULL;

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        MP_ERR(vo, "Cannot retrieve DRM resources: %s\n", mp_strerror(errno));
        ret = -errno;
        goto end;
    }

    if (conn_id == -1) {
        // get the first connected connector
        for (int i = 0; i < res->count_connectors; i++) {
            conn = drmModeGetConnector(fd, res->connectors[i]);
            if (is_connector_valid(vo, i, conn, true)) {
                conn_id = i;
                break;
            }
            if (conn) {
                drmModeFreeConnector(conn);
                conn = NULL;
            }
        }
        if (conn_id == -1) {
            MP_ERR(vo, "No connected connectors found\n");
            ret = -ENODEV;
            goto end;
        }
    }

    if (conn_id < 0 || conn_id >= res->count_connectors) {
        MP_ERR(vo, "Bad connector ID. Max valid connector ID = %u\n",
               res->count_connectors);
        ret = -ENODEV;
        goto end;
    }

    conn = drmModeGetConnector(fd, res->connectors[conn_id]);
    if (!is_connector_valid(vo, conn_id, conn, false)) {
        ret = -ENODEV;
        goto end;
    }

    dev = talloc_zero(vo->priv, struct modeset_dev);
    dev->conn = conn->connector_id;
    dev->front_buf = 0;
    dev->mode = conn->modes[0];
    dev->bufs[0].width = conn->modes[0].hdisplay;
    dev->bufs[0].height = conn->modes[0].vdisplay;
    dev->bufs[1].width = conn->modes[0].hdisplay;
    dev->bufs[1].height = conn->modes[0].vdisplay;

    MP_INFO(vo, "Connector using mode %ux%u\n",
            dev->bufs[0].width, dev->bufs[0].height);

    ret = modeset_find_crtc(vo, fd, res, conn, dev);
    if (ret) {
        MP_ERR(vo, "Connector %d has no valid CRTC\n", conn_id);
        goto end;
    }

    for (unsigned int i = 0; i < BUF_COUNT; i++) {
        ret = modeset_create_fb(vo, fd, &dev->bufs[i]);
        if (ret) {
            MP_ERR(vo, "Cannot create framebuffer for connector %d\n",
                   conn_id);
            for (unsigned int j = 0; j < i; j++) {
                modeset_destroy_fb(fd, &dev->bufs[j]);
            }
            goto end;
        }
    }

end:
    if (conn) {
        drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (res) {
        drmModeFreeResources(res);
        res = NULL;
    }
    if (ret == 0) {
        *out = dev;
    } else {
        talloc_free(dev);
    }
    return ret;
}



static int vt_switcher_pipe[2];

static void vt_switcher_sighandler(int sig)
{
    unsigned char event = sig == SIGUSR1 ? EVT_RELEASE : EVT_ACQUIRE;
    write(vt_switcher_pipe[1], &event, sizeof(event));
}

static void vt_switcher_interrupt(struct vt_switcher *s)
{
    unsigned char event = EVT_INTERRUPT;
    write(vt_switcher_pipe[1], &event, sizeof(event));
}

static int vt_switcher_init(struct vt_switcher *s, struct vo *vo)
{
    s->tty_fd = -1;
    vt_switcher_pipe[0] = -1;
    vt_switcher_pipe[1] = -1;

    s->vo = vo;
    if (mp_make_cloexec_pipe(vt_switcher_pipe)) {
        MP_ERR(vo, "Creating pipe failed: %s", mp_strerror(errno));
        return -1;
    }

    s->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (s->tty_fd < 0) {
        MP_ERR(vo, "Can't open TTY for VT control: %s", mp_strerror(errno));
        return -1;
    }

    struct sigaction act;
    act.sa_handler = vt_switcher_sighandler;
    act.sa_flags = SA_RESTART;
    sigemptyset(&act.sa_mask);
    sigaction(SIGUSR1, &act, 0);
    sigaction(SIGUSR2, &act, 0);

    struct vt_mode vt_mode;
    if (ioctl(s->tty_fd, VT_GETMODE, &vt_mode) < 0) {
        MP_ERR(vo, "VT_GETMODE failed: %s", mp_strerror(errno));
        return -1;
    }

    vt_mode.mode = VT_PROCESS;
    vt_mode.relsig = SIGUSR1;
    vt_mode.acqsig = SIGUSR2;
    if (ioctl(s->tty_fd, VT_SETMODE, &vt_mode) < 0) {
        MP_ERR(vo, "VT_SETMODE failed: %s", mp_strerror(errno));
        return -1;
    }

    return 0;
}

static void vt_switcher_destroy(struct vt_switcher *s)
{
    close(s->tty_fd);
    close(vt_switcher_pipe[0]);
    close(vt_switcher_pipe[1]);
}

static void vt_switcher_poll(struct vt_switcher *s, int timeout_ms)
{
    struct pollfd fds[1] = {
        { .events = POLLIN, .fd = vt_switcher_pipe[0] },
    };
    poll(fds, 1, timeout_ms);
    if (!fds[0].revents) return;

    unsigned char event;
    if (read(fds[0].fd, &event, sizeof(event)) != sizeof(event)) return;

    switch (event) {
    case EVT_RELEASE:
        release_vo_crtc(s->vo);
        if (USE_MASTER) {
            //this function enables support for switching to x, weston etc.
            //however, for whatever reason, it can be called only by root users.
            //until things change, this is commented.
            struct priv *p = s->vo->priv;
            if (drmDropMaster(p->fd)) {
                MP_WARN(s->vo, "Failed to drop DRM master: %s\n", mp_strerror(errno));
            }
        }

        if (ioctl(s->tty_fd, VT_RELDISP, 1) < 0) {
            MP_ERR(s->vo, "Failed to release virtual terminal\n");
        }
        break;

    case EVT_ACQUIRE:
        if (USE_MASTER) {
            struct priv *p = s->vo->priv;
            if (drmSetMaster(p->fd)) {
                MP_WARN(s->vo, "Failed to acquire DRM master: %s\n", mp_strerror(errno));
            }
        }

        setup_vo_crtc(s->vo);
        if (ioctl(s->tty_fd, VT_RELDISP, VT_ACKACQ) < 0) {
            MP_ERR(s->vo, "Failed to acquire virtual terminal\n");
        }
        break;

    case EVT_INTERRUPT:
        break;
    }
}



static int setup_vo_crtc(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->old_crtc = drmModeGetCrtc(p->fd, p->dev->crtc);
    int ret = drmModeSetCrtc(p->fd, p->dev->crtc,
                          p->dev->bufs[p->dev->front_buf + BUF_COUNT - 1].fb,
                          0, 0, &p->dev->conn, 1, &p->dev->mode);
    p->active = true;
    return ret;
}

static void release_vo_crtc(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->active = false;
    if (p->old_crtc) {
        drmModeSetCrtc(p->fd,
                       p->old_crtc->crtc_id,
                       p->old_crtc->buffer_id,
                       p->old_crtc->x,
                       p->old_crtc->y,
                       &p->dev->conn,
                       1,
                       &p->dev->mode);
        drmModeFreeCrtc(p->old_crtc);
    }
}

static int wait_events(struct vo *vo, int64_t until_time_us)
{
    struct priv *p = vo->priv;
    int64_t wait_us = until_time_us - mp_time_us();
    int timeout_ms = MPCLAMP((wait_us + 500) / 1000, 0, 10000);
    vt_switcher_poll(&p->vt_switcher, timeout_ms);
    return 0;
}

static void wakeup(struct vo *vo)
{
    struct priv *p = vo->priv;
    vt_switcher_interrupt(&p->vt_switcher);
}

static int reconfig(struct vo *vo, struct mp_image_params *params, int flags)
{
    struct priv *p = vo->priv;

    vo->dwidth = p->device_w;
    vo->dheight = p->device_h;
    vo_get_src_dst_rects(vo, &p->src, &p->dst, &p->osd);

    int32_t w = p->dst.x1 - p->dst.x0;
    int32_t h = p->dst.y1 - p->dst.y0;

    // p->osd contains the parameters assuming OSD rendering in window
    // coordinates, but OSD can only be rendered in the intersection
    // between window and video rectangle (i.e. not into panscan borders).
    p->osd.w = w;
    p->osd.h = h;
    p->osd.mt = MPMIN(0, p->osd.mt);
    p->osd.mb = MPMIN(0, p->osd.mb);
    p->osd.mr = MPMIN(0, p->osd.mr);
    p->osd.ml = MPMIN(0, p->osd.ml);

    p->x = (p->device_w - w) >> 1;
    p->y = (p->device_h - h) >> 1;

    mp_sws_set_from_cmdline(p->sws, vo->opts->sws_opts);
    p->sws->src = *params;
    p->sws->dst = (struct mp_image_params) {
        .imgfmt = IMGFMT_BGR0,
        .w = w,
        .h = h,
        .d_w = w,
        .d_h = h,
    };

    talloc_free(p->cur_frame);
    p->cur_frame = mp_image_alloc(IMGFMT_BGR0, p->device_w, p->device_h);
    mp_image_params_guess_csp(&p->sws->dst);
    mp_image_set_params(p->cur_frame, &p->sws->dst);

    if (mp_sws_reinit(p->sws) < 0)
        return -1;

    vo->want_redraw = true;
    return 0;
}

static void draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct priv *p = vo->priv;

    if (p->active) {
        struct mp_rect src_rc = p->src;
        src_rc.x0 = MP_ALIGN_DOWN(src_rc.x0, mpi->fmt.align_x);
        src_rc.y0 = MP_ALIGN_DOWN(src_rc.y0, mpi->fmt.align_y);
        mp_image_crop_rc(mpi, src_rc);
        mp_sws_scale(p->sws, p->cur_frame, mpi);
        osd_draw_on_image(vo->osd, p->osd, mpi ? mpi->pts : 0, 0, p->cur_frame);

        struct modeset_buf *front_buf = &p->dev->bufs[p->dev->front_buf];
        int32_t shift = (p->device_w * p->y + p->x) * 4;
        memcpy_pic(front_buf->map + shift,
                   p->cur_frame->planes[0],
                   (p->dst.x1 - p->dst.x0) * 4,
                   p->dst.y1 - p->dst.y0,
                   p->device_w * 4,
                   p->cur_frame->stride[0]);
    }

    if (mpi != p->last_input) {
        talloc_free(p->last_input);
        p->last_input = mpi;
    }
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->active) return;

    int ret = drmModeSetCrtc(p->fd, p->dev->crtc,
                             p->dev->bufs[p->dev->front_buf].fb,
                             0, 0, &p->dev->conn, 1, &p->dev->mode);
    if (ret) {
        MP_WARN(vo, "Cannot flip page for connector\n");
    } else {
        p->dev->front_buf++;
        p->dev->front_buf %= BUF_COUNT;
    }
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (p->dev) {
        release_vo_crtc(vo);

        modeset_destroy_fb(p->fd, &p->dev->bufs[1]);
        modeset_destroy_fb(p->fd, &p->dev->bufs[0]);
        drmModeFreeEncoder(p->dev->enc);
    }

    vt_switcher_destroy(&p->vt_switcher);
    talloc_free(p->last_input);
    talloc_free(p->cur_frame);
    talloc_free(p->dev);
    close(p->fd);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->sws = mp_sws_alloc(vo);
    p->fd = -1;

    if (vt_switcher_init(&p->vt_switcher, vo))
        goto err;

    if (modeset_open(vo, &p->fd, p->device_path))
        goto err;

    if (modeset_prepare_dev(vo, p->fd, p->connector_id, &p->dev))
        goto err;

    assert(p->dev);
    p->device_w = p->dev->bufs[0].width;
    p->device_h = p->dev->bufs[0].height;

    if (setup_vo_crtc(vo)) {
        MP_ERR(vo, "Cannot set CRTC for connector %u: %s\n", p->connector_id,
               mp_strerror(errno));
        goto err;
    }

    return 0;

err:
    uninit(vo);
    return -1;
}

static int query_format(struct vo *vo, int format)
{
    return sws_isSupportedInput(imgfmt2pixfmt(format));
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    switch (request) {
    case VOCTRL_REDRAW_FRAME:
        draw_image(vo, p->last_input);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_drm = {
    .name = "drm",
    .description = "Direct Rendering Manager",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_image = draw_image,
    .flip_page = flip_page,
    .uninit = uninit,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_STRING("devpath", device_path, 0),
        OPT_INT("connector", connector_id, 0),
        {0},
    },
    .priv_defaults = &(const struct priv) {
        .device_path = "/dev/dri/card0",
        .connector_id = -1,
    },
};