/**
 * @file drm.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "drm.h"
#if USE_DRM

#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <poll.h>
#include <malloc.h>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <lvgl/lvgl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DBG_TAG "drm"
#define NUM_DUMB_BO 3
#define VIDEO_PLANE_ENABLE 1
#define DEBUG
#ifdef DEBUG
#define DRM_DEBUG(fmt, ...) \
    if (getenv("LVGL_DRM_DEBUG")) \
        printf("DRM_DEBUG: %s(%d) " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
#define DRM_DEBUG(fmt, ...)
#endif

#define ALIGN(x, a)     (((x) + (a - 1)) & ~(a - 1))

#if USE_RGA
#define SW_ROTATION     0
#else
#define SW_ROTATION     1
#endif

struct drm_bo {
    int fd;
    void *ptr;
    size_t size;
    size_t offset;
    size_t pitch;
    unsigned int handle;
    int fb_id;
    int buf_fd;
    int w;
    int h;
};

struct device {
    int fd;

    struct {
        int width;
        int height;

        int hdisplay;
        int vdisplay;

        int current;
        int fb_num;
        int bpp;
    } mode;

    drmModeResPtr res;

    int connector_id;
    int encoder_id;
    int crtc_id;
    int plane_id;
    int last_fb_id;

    int waiting_for_flip;
    struct pollfd drm_pollfd;
    drmEventContext drm_evctx;
};

static int lcd_w;
static int lcd_h;
static int lcd_fps = 0;
static int lcd_sw;
static char* drm_buff;
static lv_color_t *buf_1;
static int disp_rot = 0;
static int disp_crop = 0;

static int quit = 0;
static pthread_t drm_thread_pid;
static pthread_mutex_t draw_mutex;
static int draw_update = 0;
#if USE_RGA
static struct drm_bo *gbo;
#endif
static struct drm_bo *vop_buf[2];

struct device *pdev;
static lv_disp_t * lv_disp_drm;

static int bo_map(struct device *dev, struct drm_bo *bo)
{
    struct drm_mode_map_dumb arg = {
        .handle = bo->handle,
    };
    struct drm_prime_handle fd_args = {
        .fd = -1,
        .handle = bo->handle,
        .flags = 0,
    };
    int ret;

    ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
    if (ret)
        return ret;

    ret = drmIoctl(dev->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &fd_args);
    if (ret)
    {
        printf("handle_to_fd failed ret=%d, handle=%x \n", ret ,fd_args.handle);
        return -1;
    }
    bo->buf_fd = fd_args.fd;

    bo->ptr = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   dev->fd, arg.offset);
    if (bo->ptr == MAP_FAILED) {
        bo->ptr = NULL;
        return -1;
    }
    memset(bo->ptr, 0, bo->size);

    return 0;
}

static void bo_unmap(struct device *dev, struct drm_bo *bo)
{
    if (dev == NULL)
        return;
    if (!bo->ptr)
        return;

    drmUnmap(bo->ptr, bo->size);
    if (bo->buf_fd > 0)
        close(bo->buf_fd);
    bo->ptr = NULL;
}

void bo_destroy(struct device *dev, struct drm_bo *bo)
{
    struct drm_mode_destroy_dumb arg = {
        .handle = bo->handle,
    };

    if (bo->fb_id)
        drmModeRmFB(dev->fd, bo->fb_id);

    bo_unmap(dev, bo);

    if (bo->handle)
        drmIoctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);

    free(bo);
}

static struct drm_bo *
bo_create(struct device *dev, int width, int height, int format)
{
    struct drm_mode_create_dumb arg = {
        .bpp = LV_COLOR_DEPTH,
        .width = ALIGN(width, 16),
        .height = ALIGN(height, 16),
    };
    struct drm_bo *bo;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    int ret;

    bo = malloc(sizeof(struct drm_bo));
    if (bo == NULL) {
        fprintf(stderr, "allocate bo failed\n");
        return NULL;
    }
    memset(bo, 0, sizeof(*bo));
    if (format == DRM_FORMAT_NV12) {
        arg.bpp = 8;
        arg.height = height * 3 / 2;
    }

    ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if (ret) {
        fprintf(stderr, "create dumb failed\n");
        goto err;
    }

    bo->fd = dev->fd;
    bo->handle = arg.handle;
    bo->size = arg.size;
    bo->pitch = arg.pitch;
    bo->w = width;
    bo->h = height;

    ret = bo_map(dev, bo);
    if (ret) {
        fprintf(stderr, "map bo failed\n");
        goto err;
    }

    switch (format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV16:
      handles[0] = bo->handle;
      pitches[0] = bo->pitch ;
      offsets[0] = 0;
      handles[1] = bo->handle;
      pitches[1] = pitches[0];
      offsets[1] = pitches[0] * height;
      break;
    case DRM_FORMAT_RGB332:
      handles[0] = bo->handle;
      pitches[0] = bo->pitch;
      offsets[0] = 0;
      break;
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
      handles[0] = bo->handle;
      pitches[0] = bo->pitch ;
      offsets[0] = 0;
      break;
    case DRM_FORMAT_RGB888:
    case DRM_FORMAT_BGR888:
      handles[0] = bo->handle;
      pitches[0] = bo->pitch ;
      offsets[0] = 0;
      break;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRA8888:
      handles[0] = bo->handle;
      pitches[0] = bo->pitch ;
      offsets[0] = 0;
      break;
    }

    ret = drmModeAddFB2(dev->fd, width, height, format, handles,
                        pitches, offsets, (uint32_t *)&bo->fb_id, 0);
    if (ret) {
        fprintf(stderr, "add fb failed\n");
        goto err;
    }
    DRM_DEBUG("Created bo: %d, %dx%d\n", bo->fb_id, width, height);

    return bo;
err:
    bo_destroy(dev, bo);
    return NULL;
}

struct drm_bo *malloc_drm_bo(int width, int height, int format)
{
    struct device *dev = pdev;
    return bo_create(dev, width, height, format);
}

void free_drm_bo(struct drm_bo *bo)
{
    struct device *dev = pdev;
    if (bo)
        bo_destroy(dev, bo);
}

static void free_fb(struct device *dev)
{
    DRM_DEBUG("Free fb, num: %d, bpp: %d\n", dev->mode.fb_num, dev->mode.bpp);

    dev->mode.fb_num = 0;
    dev->mode.bpp = 0;
    dev->mode.current = 0;
}

static int alloc_fb(struct device *dev, int num, int bpp)
{
    DRM_DEBUG("Alloc fb num: %d, bpp: %d\n", num, bpp);

    dev->mode.fb_num = num;
    dev->mode.bpp = bpp;
    dev->mode.current = 0;

    return 0;
}

static int drm_get_preferred_connector(void)
{
    const char *path;
    char buf[256] = "\0";
    int fd;

#define DRM_CONNECTOR_CFG_PATH_ENV	"DRM_CONNECTOR_CFG_PATH"
#define DRM_CONNECTOR_CFG_PATH_DEFAULT	"/tmp/drm_connector.cfg"
    path = getenv(DRM_CONNECTOR_CFG_PATH_ENV);
    if (!path)
        path = DRM_CONNECTOR_CFG_PATH_DEFAULT;

    fd = open(path, O_RDONLY);
    if (read(fd, buf, sizeof(buf)) != sizeof(buf)) {
        DRM_DEBUG("Warning: read failed\n");
    }
    close(fd);

    if (!buf[0])
        return -1;

    return atoi(buf);
}

static int drm_get_preferred_mode(int *width, int *height)
{
    const char *path;
    char buf[256] = "\0";
    int fd, w, h;

#define DRM_MODE_CFG_PATH_ENV	"DRM_CONNECTOR_CFG_PATH"
#define DRM_MODE_CFG_PATH_DEFAULT	"/tmp/drm_mode.cfg"
    path = getenv(DRM_MODE_CFG_PATH_ENV);
    if (!path)
        path = DRM_MODE_CFG_PATH_DEFAULT;

    fd = open(path, O_RDONLY);
    if (read(fd, buf, sizeof(buf)) != sizeof(buf)) {
        DRM_DEBUG("Warning: read failed\n");
    }
    close(fd);

    if (!buf[0])
        return -1;

    if (2 != sscanf(buf, "%dx%d", &w, &h))
        return -1;

    *width = w;
    *height = h;

    return 0;
}

static drmModeConnectorPtr
drm_get_connector(struct device *dev, int connector_id)
{
    drmModeConnectorPtr conn;

    conn = drmModeGetConnector(dev->fd, connector_id);
    if (!conn)
        return NULL;

    DRM_DEBUG("Connector id: %d, %sconnected, modes: %d\n", connector_id,
              (conn->connection == DRM_MODE_CONNECTED) ? "" : "dis",
              conn->count_modes);
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes)
        return conn;

    drmModeFreeConnector(conn);
    return NULL;
}

static drmModeConnectorPtr
drm_find_best_connector(struct device *dev)
{
    drmModeResPtr res = dev->res;
    drmModeConnectorPtr conn;
    int i, preferred_connector_id = drm_get_preferred_connector();

    DRM_DEBUG("Preferred connector id: %d\n", preferred_connector_id);
    conn = drm_get_connector(dev, preferred_connector_id);
    if (conn)
        return conn;

    for (i = 0; i < res->count_connectors; i++) {
        conn = drm_get_connector(dev, res->connectors[i]);
        if (conn)
            return conn;
    }
    return NULL;
}

static drmModeCrtcPtr
drm_find_best_crtc(struct device *dev, drmModeConnectorPtr conn)
{
    drmModeResPtr res = dev->res;
    drmModeEncoderPtr encoder;
    drmModeCrtcPtr crtc;
    int i, preferred_crtc_id = 0;
    int crtcs_for_connector = 0;

    encoder = drmModeGetEncoder(dev->fd, conn->encoder_id);
    if (encoder) {
        preferred_crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
    }
    DRM_DEBUG("Preferred crtc: %d\n", preferred_crtc_id);

    crtc = drmModeGetCrtc(dev->fd, preferred_crtc_id);
    if (crtc)
        return crtc;

    for (i = 0; i < res->count_encoders; i++) {
        encoder = drmModeGetEncoder(dev->fd, res->encoders[i]);
        if (encoder)
            crtcs_for_connector |= encoder->possible_crtcs;
        drmModeFreeEncoder(encoder);
    }
    DRM_DEBUG("Possible crtcs: %x\n", crtcs_for_connector);
    if (!crtcs_for_connector)
        return NULL;

    return drmModeGetCrtc(dev->fd, res->crtcs[ffs(crtcs_for_connector) - 1]);
}

int
drm_plane_is_primary(struct device *dev, int plane_id)
{
    drmModeObjectPropertiesPtr props;
    drmModePropertyPtr prop;
    unsigned int i;
    int type = 0;

    props = drmModeObjectGetProperties(dev->fd, plane_id,
                                       DRM_MODE_OBJECT_PLANE);
    if (!props)
        return 0;

    for (i = 0; i < props->count_props; i++) {
        prop = drmModeGetProperty(dev->fd, props->props[i]);
        if (prop && !strcmp(prop->name, "type"))
            type = props->prop_values[i];
        drmModeFreeProperty(prop);
    }
    DRM_DEBUG("Plane: %d, type: %d\n", plane_id, type);

    drmModeFreeObjectProperties(props);
    return type == DRM_PLANE_TYPE_PRIMARY;
}

int
drm_plane_is_overlay(struct device *dev, int plane_id)
{
    drmModeObjectPropertiesPtr props;
    drmModePropertyPtr prop;
    unsigned int i;
    int type = 0;

    props = drmModeObjectGetProperties(dev->fd, plane_id,
                                       DRM_MODE_OBJECT_PLANE);
    if (!props)
        return 0;

    for (i = 0; i < props->count_props; i++) {
        prop = drmModeGetProperty(dev->fd, props->props[i]);
        if (prop && !strcmp(prop->name, "type"))
            type = props->prop_values[i];
        drmModeFreeProperty(prop);
    }
    DRM_DEBUG("Plane: %d, type: %d\n", plane_id, type);

    drmModeFreeObjectProperties(props);
    return type == DRM_PLANE_TYPE_OVERLAY;
}

int
drm_plane_is_cursor(struct device *dev, int plane_id)
{
    drmModeObjectPropertiesPtr props;
    drmModePropertyPtr prop;
    unsigned int i;
    int type = 0;

    props = drmModeObjectGetProperties(dev->fd, plane_id,
                                       DRM_MODE_OBJECT_PLANE);
    if (!props)
        return 0;

    for (i = 0; i < props->count_props; i++) {
        prop = drmModeGetProperty(dev->fd, props->props[i]);
        if (prop && !strcmp(prop->name, "type"))
            type = props->prop_values[i];
        drmModeFreeProperty(prop);
    }
    DRM_DEBUG("Plane: %d, type: %d\n", plane_id, type);

    drmModeFreeObjectProperties(props);
    return type == DRM_PLANE_TYPE_CURSOR;
}

static drmModePlanePtr
drm_get_plane(struct device *dev, int plane_id, int pipe)
{
    drmModePlanePtr plane;
    char* set_plane;
    plane = drmModeGetPlane(dev->fd, plane_id);
    if (!plane)
        return NULL;

    DRM_DEBUG("Check plane: %d, possible_crtcs: %x\n", plane_id,
              plane->possible_crtcs);
    set_plane = getenv("LV_DRIVERS_SET_PLANE");
    if (set_plane == NULL) {
        printf("LV_DRIVERS_SET_PLANE not be set, use DRM_PLANE_TYPE_PRIMARY\n");
        if (drm_plane_is_primary(dev, plane_id)) {
            if (plane->possible_crtcs & (1 << pipe))
                return plane;
        }
        goto end;
    }
    if (!strcmp("OVERLAY", set_plane)) {
        printf("LV_DRIVERS_SET_PLANE = DRM_PLANE_TYPE_OVERLAY\n");
        if (drm_plane_is_overlay(dev, plane_id)) {
            if (plane->possible_crtcs & (1 << pipe))
                return plane;
        }
    } else if (!strcmp("PRIMARY", set_plane)) {
        printf("LV_DRIVERS_SET_PLANE = DRM_PLANE_TYPE_PRIMARY\n");
        if (drm_plane_is_primary(dev, plane_id)) {
            if (plane->possible_crtcs & (1 << pipe))
                return plane;
        }
    } else if (!strcmp("CURSOR", set_plane)) {
        printf("LV_DRIVERS_SET_PLANE = DRM_PLANE_TYPE_CURSOR\n");
        if (drm_plane_is_cursor(dev, plane_id)) {
            if (plane->possible_crtcs & (1 << pipe))
                return plane;
        }
    } else {
        printf("LV_DRIVERS_SET_PLANE set err, use DRM_PLANE_TYPE_PRIMARY\n");
        if (drm_plane_is_primary(dev, plane_id)) {
            if (plane->possible_crtcs & (1 << pipe))
                return plane;
        }
    }

end:
    drmModeFreePlane(plane);
    return NULL;
}

static drmModePlanePtr
drm_find_best_plane(struct device *dev, drmModeCrtcPtr crtc)
{
    drmModeResPtr res = dev->res;
    drmModePlaneResPtr pres;
    drmModePlanePtr plane;
    unsigned int i;
    int pipe;

    for (pipe = 0; pipe < res->count_crtcs; pipe++) {
        if (crtc->crtc_id == res->crtcs[pipe])
            break;
    }
    if (pipe == res->count_crtcs)
        return NULL;

    pres = drmModeGetPlaneResources(dev->fd);
    if (!pres)
        return NULL;

    for (i = 0; i < pres->count_planes; i++) {
        plane = drm_get_plane(dev, pres->planes[i], pipe);
        if (plane) {
            drmModeFreePlaneResources(pres);
            return plane;
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(pres);
    return NULL;
}

static drmModeModeInfoPtr
drm_find_best_mode(struct device *dev, drmModeConnectorPtr conn)
{
    drmModeModeInfoPtr mode;
    int i, preferred_width = 1920, preferred_height = 1080;

    if (dev == NULL)
        return 0;
    drm_get_preferred_mode(&preferred_width, &preferred_height);
    DRM_DEBUG("Preferred mode: %dx%d\n", preferred_width, preferred_height);

    mode = &conn->modes[0];
    for (i = 0; i < conn->count_modes; i++) {
        DRM_DEBUG("Check mode: %dx%d\n",
                conn->modes[i].hdisplay, conn->modes[i].vdisplay);
        if (conn->modes[i].hdisplay == preferred_width &&
                conn->modes[i].vdisplay == preferred_height) {
            mode = &conn->modes[i];
            break;
        }
    }

    return mode;
}

static int drm_crtc_mode_frame_rate_adjust(struct device *dev, int format, uint32_t target_fps)
{
    int ret,connector_id;
    struct drm_bo *temp_bo;
    drmModeModeInfoPtr mode;
    drmModeConnector *conn;

    if (dev == NULL) {
        ret = -1;
        goto err_1;
    }

    if (!target_fps) {
        printf("skip frame rate adjust\n");
        return 0;
    }

    connector_id= dev->connector_id;

    conn = drmModeGetConnector(dev->fd, dev->connector_id);
    if (!conn) {
        printf("drmModeGetConnector failed\n");
        ret = -1;
        goto err_1;
    }

    mode = drm_find_best_mode(dev, conn);
    if (!mode) {
        printf("drm find mode failed\n");
        ret = -1;
        goto err_2;
    }

    mode->clock = mode->clock * target_fps / mode->vrefresh;
    mode->vrefresh = target_fps;

    temp_bo = malloc_drm_bo(mode->hdisplay, mode->vdisplay, format);
    if (temp_bo == NULL) {
        ret = -1;
        goto err_2;
    }

    ret = drmModeSetCrtc(dev->fd, dev->crtc_id, temp_bo->fb_id, 0, 0, &connector_id, 1, mode);
    if (ret)
    {
        printf("%s drmModeSetCrtc failed %s crtc %d fb:%d connector %d mode %dx%d-%d\n",
               __func__, strerror(errno), dev->crtc_id, temp_bo->fb_id, dev->connector_id,
               mode->hdisplay, mode->vdisplay, mode->vrefresh);
    }

    free_drm_bo(temp_bo);
err_2:
    drmModeFreeConnector(conn);
err_1:
    return ret;
}

static int drm_get_preferred_fb_mode(int *width, int *height)
{
    char *buf;
    int w, h;

    buf = getenv("LVGL_DRM_FB_MODE");
    if (!buf)
        return -1;

    if (2 != sscanf(buf, "%dx%d", &w, &h))
        return -1;

    DRM_DEBUG("Preferred fb mode: %dx%d\n", w, h);
    *width = w;
    *height = h;

    return 0;
}

static void drm_setup_fb_mode(struct device *dev)
{
    drmModeResPtr res = dev->res;
    drmModeConnectorPtr conn;
    drmModeModeInfoPtr mode;
    int i;

    if (dev->mode.width && dev->mode.height)
        return;

    if (!drm_get_preferred_fb_mode(&dev->mode.width, &dev->mode.height))
        return;

    dev->mode.width = dev->mode.hdisplay;
    dev->mode.height = dev->mode.vdisplay;

    for (i = 0; i < res->count_connectors; i++) {
        conn = drm_get_connector(dev, res->connectors[i]);
        if (!conn)
            continue;

        mode = drm_find_best_mode(dev, conn);
        if (mode) {
            DRM_DEBUG("Best mode for connector(%d): %dx%d\n",
                      conn->connector_id, mode->hdisplay, mode->vdisplay);
            if (dev->mode.width > mode->hdisplay ||
                    dev->mode.height > mode->vdisplay) {
                dev->mode.width = mode->hdisplay;
                dev->mode.height = mode->vdisplay;
            }
        }
        drmModeFreeConnector(conn);
    }
}

static void drm_free(struct device *dev)
{
    int i;

    if (dev->res) {
        drmModeFreeResources(dev->res);
        dev->res = NULL;
    }

    dev->connector_id = 0;
    dev->crtc_id = 0;
    dev->plane_id = 0;
    dev->mode.hdisplay = 0;
    dev->mode.vdisplay = 0;
}

static void configure_plane_zpos(struct device *self, int plane_id, uint64_t zpos)
{
    drmModeObjectPropertiesPtr props = NULL;
    drmModePropertyPtr prop = NULL;
    char *buf;
    unsigned int i;

    if (plane_id <= 0)
        return;

    if (drmSetClientCap (self->fd, DRM_CLIENT_CAP_ATOMIC, 1))
        return;

    props = drmModeObjectGetProperties (self->fd, plane_id,
          DRM_MODE_OBJECT_PLANE);
    if (!props)
        goto out;

    for (i = 0; i < props->count_props; i++) {
        prop = drmModeGetProperty (self->fd, props->props[i]);
        if (prop && !strcmp (prop->name, "ZPOS"))
          break;
        drmModeFreeProperty (prop);
        prop = NULL;
    }

    if (!prop)
        goto out;

    drmModeObjectSetProperty (self->fd, plane_id,
          DRM_MODE_OBJECT_PLANE, props->props[i], zpos);
out:
    drmModeFreeProperty (prop);
    drmModeFreeObjectProperties (props);
}

static int drm_setup(struct device *dev)
{
    drmModeConnectorPtr conn = NULL;
    drmModeModeInfoPtr mode;
    drmModePlanePtr plane = NULL;
    drmModeCrtcPtr crtc = NULL;
    //int ret;
    int i, success = 0;

    dev->res = drmModeGetResources(dev->fd);
    if (!dev->res) {
        fprintf(stderr, "drm get resource failed\n");
        goto err;
    }

    conn = drm_find_best_connector(dev);
    if (!conn) {
        fprintf(stderr, "drm find connector failed\n");
        goto err;
    }
    DRM_DEBUG("Best connector id: %d\n", conn->connector_id);

    mode = drm_find_best_mode(dev, conn);
    if (!mode) {
        fprintf(stderr, "drm find mode failed\n");
        goto err;
    }
    DRM_DEBUG("Best mode: %dx%d\n", mode->hdisplay, mode->vdisplay);

    crtc = drm_find_best_crtc(dev, conn);
    if (!crtc) {
        fprintf(stderr, "drm find crtc failed\n");
        goto err;
    }
    DRM_DEBUG("Best crtc: %d\n", crtc->crtc_id);

    plane = drm_find_best_plane(dev, crtc);
    if (!plane) {
        fprintf(stderr, "drm find plane failed\n");
        goto err;
    }
    configure_plane_zpos(dev, plane->plane_id, 1);
    printf("Best plane: %d\n", plane->plane_id);
    dev->connector_id = conn->connector_id;
    dev->crtc_id = crtc->crtc_id;
    dev->plane_id = plane->plane_id;
    dev->last_fb_id = 0;
    dev->mode.hdisplay = mode->hdisplay;
    dev->mode.vdisplay = mode->vdisplay;

    drm_setup_fb_mode(dev);
    DRM_DEBUG("Drm fb mode: %dx%d\n", dev->mode.width, dev->mode.height);

    success = 1;
err:
    drmModeFreeConnector(conn);
    drmModeFreePlane(plane);
    drmModeFreeCrtc(crtc);
    if (!success) {
        drm_free(dev);
        return -1;
    }
    return 0;
}

static void drm_flip_handler(int fd, unsigned frame, unsigned sec,
                             unsigned usec, void *data)
{
    // data is &dev->waiting_for_flip
    DRM_DEBUG("Page flip received(%d)!, %d, %d, %d, %d\n", *(int*)data, fd, frame, sec, usec);
    *(int*)data = 0;
}

int drm_init(int bpp)
{
    int ret;

    pdev = malloc(sizeof(struct device));
    if (pdev == NULL) {
        fprintf(stderr, "allocate device failed\n");
        return -1;
    }
    memset(pdev, 0, sizeof(*pdev));

    //drm_install_sighandler(pdev);

    pdev->fd = drmOpen(NULL, NULL);
    if (pdev->fd < 0)
        pdev->fd = open("/dev/dri/card0", O_RDWR);
    if (pdev->fd < 0) {
        fprintf(stderr, "drm open failed\n");
        goto err_drm_open;
    }
    fcntl(pdev->fd, F_SETFD, FD_CLOEXEC);

    drmSetClientCap(pdev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    ret = alloc_fb(pdev, NUM_DUMB_BO, bpp);
    if (ret) {
        fprintf(stderr, "alloc fb failed\n");
        goto err_alloc_fb;
    }

    ret = drm_setup(pdev);
    if (ret) {
        fprintf(stderr, "drm setup failed\n");
        goto err_drm_setup;
    }

    pdev->drm_pollfd.fd = pdev->fd;
    pdev->drm_pollfd.events = POLLIN;

    pdev->drm_evctx.version = DRM_EVENT_CONTEXT_VERSION;
    pdev->drm_evctx.page_flip_handler = drm_flip_handler;

    return 0;
err_alloc_fb:
    drm_free(pdev);
err_drm_setup:
    drmClose(pdev->fd);
err_drm_open:
    free(pdev);
    pdev = NULL;
    return -1;
}

int drm_exit(void)
{
    struct device* dev = pdev;
    if (!dev)
        return 0;

    free_fb(dev);
    drm_free(dev);

    if (pdev->fd > 0)
        drmClose(dev->fd);

    free(pdev);
    pdev = NULL;

    return 0;
}

int getdrmfd(void)
{
    return pdev->fd;
}

static void drm_wait_flip(struct device* dev, int timeout)
{
    int ret;

    while (dev->waiting_for_flip) {
        dev->drm_pollfd.revents = 0;
        ret = poll(&dev->drm_pollfd, 1, timeout);
        if (ret <= 0)
            return;

        drmHandleEvent(dev->fd, &dev->drm_evctx);
    }
}

void setdrmdisp(struct drm_bo *bo)
{
    struct device* dev = pdev;
    int crtc_x, crtc_y, crtc_w, crtc_h;
    int ret;
    int fb = bo->fb_id, sw = lcd_w, sh= lcd_h;

    if (dev == NULL)
        return;

    if (disp_crop)
    {
        crtc_w = sw;
        crtc_h = sh;
        crtc_x = (dev->mode.width - sw) / 2;
        crtc_y = (dev->mode.height - sh) / 2;
    }
    else
    {
        /* Fullscreen */
        crtc_w = dev->mode.width;
        crtc_h = dev->mode.height;
        crtc_x = 0;
        crtc_y = 0;
    }

    DRM_DEBUG("Display bo %d(%dx%d) at (%d,%d) %dx%d\n", fb, sw, sh,
             crtc_x, crtc_y, crtc_w, crtc_h);
    ret = drmModeSetPlane(dev->fd, dev->plane_id, dev->crtc_id, fb, 0,
                          crtc_x, crtc_y, crtc_w, crtc_h,
                          0, 0, sw << 16, sh << 16);
    if (ret) {
        fprintf(stderr, "drm set plane failed\n");
        return;
    }
    if (0) {
        // Queue page flip
        dev->waiting_for_flip = 1;
        ret = drmModePageFlip(dev->fd, dev->crtc_id, fb,
                              DRM_MODE_PAGE_FLIP_EVENT, &dev->waiting_for_flip);
        if (ret) {
            fprintf(stderr, "drm page flip failed\n");
            return;
        }
        // Wait for last page flip
        drm_wait_flip(dev, -1);
    }
}

void getdrmresolve(int *w, int *h)
{
    *w = pdev->mode.width;
    *h = pdev->mode.height;
}

static void *drm_thread(void *arg)
{
#define MIN_TICK    16 // 60 FPS
    struct drm_bo *bo;
    uint32_t tick = 0, ts;
#if USE_RGA
    rga_info_t src;
    rga_info_t dst;
    int ret;
#endif
    pthread_setname_np(pthread_self(), "lv_drm");

    while (!quit) {
        ts = lv_tick_get();
        if ((ts - tick) < MIN_TICK) {
            usleep(5000);
            continue;
        }
        tick = ts;
        pthread_mutex_lock(&draw_mutex);
        if (draw_update) {
            bo = (bo == vop_buf[0]) ? vop_buf[1] : vop_buf[0];

#if USE_RGA
            memset(&src, 0, sizeof(rga_info_t));
            memset(&dst, 0, sizeof(rga_info_t));
            src.fd = gbo->buf_fd;
            src.mmuFlag = 1;
            dst.fd = bo->buf_fd;
            dst.mmuFlag = 1;
            rga_set_rotation(&src, disp_rot);
            rga_set_rect(&src.rect, 0, 0, gbo->w, gbo->h,
                         gbo->pitch / (LV_COLOR_DEPTH >> 3),
                         gbo->h, RK_FORMAT_BGRA_8888);
            rga_set_rect(&dst.rect, 0, 0, bo->w, bo->h,
                         bo->pitch / (LV_COLOR_DEPTH >> 3),
                         bo->h, RK_FORMAT_BGRA_8888);
            ret = c_RkRgaBlit(&src, &dst, NULL);
            if (ret)
                printf("c_RkRgaBlit error : %s\n", strerror(errno));
#else
            for (int i = 0; i < lcd_h; i++)
            {
                memcpy(bo->ptr + i * bo->pitch,
                       drm_buff + i * lcd_sw * (LV_COLOR_DEPTH >> 3),
                       lcd_w * (LV_COLOR_DEPTH >> 3));
            }
#endif
            setdrmdisp(bo);
            draw_update = 0;
        }
        pthread_mutex_unlock(&draw_mutex);
    }
    return NULL;
}

static int32_t lv_env_get_u32(const char *name, uint32_t *value, uint32_t default_value)
{
    char *ptr = getenv(name);

    if (NULL == ptr) {
        *value = default_value;
    } else {
        char *endptr;
        int base = (ptr[0] == '0' && ptr[1] == 'x') ? (16) : (10);
        errno = 0;
        *value = strtoul(ptr, &endptr, base);
        if (errno || (ptr == endptr)) {
            errno = 0;
            *value = default_value;
        }
    }
    return 0;
}

#if SW_ROTATION
#if (LV_COLOR_DEPTH == 16)
typedef uint16_t data_type;
#elif (LV_COLOR_DEPTH == 32)
typedef uint32_t data_type;
#endif

static void rotate_90(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p)
{
    data_type *src = (data_type *)color_p;
    data_type *dst;
    int32_t x, tx;
    int32_t y, ty;

    for(y = area->y1; y <= area->y2; y++) {
        tx = (lcd_w - 1) - y;
        ty = area->x1;
        dst = (data_type *)(drm_buff + (ty * lcd_sw + tx) * (LV_COLOR_DEPTH >> 3));
        for(x = area->x1; x <= area->x2; x++) {
            *dst = *src;
            src++;
            dst += lcd_sw;
        }
    }
}

static void rotate_180(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p)
{
    data_type *src = (data_type *)color_p;
    data_type *dst;
    int32_t x, tx;
    int32_t y, ty;

    for(y = area->y1; y <= area->y2; y++) {
        tx = (lcd_w - 1) - area->x1;
        ty = (lcd_h - 1) - y;
        dst = (data_type *)(drm_buff + (ty * lcd_sw + tx) * (LV_COLOR_DEPTH >> 3));
        for(x = area->x1; x <= area->x2; x++) {
            *dst = *src;
            src++;
            dst--;
        }
    }
}

static void rotate_270(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p)
{
    data_type *src = (data_type *)color_p;
    data_type *dst;
    int32_t x, tx;
    int32_t y, ty;

    for(y = area->y1; y <= area->y2; y++) {
        tx = y;
        ty = (lcd_h - 1) - area->x1;
        dst = (data_type *)(drm_buff + (ty * lcd_sw + tx) * (LV_COLOR_DEPTH >> 3));
        for(x = area->x1; x <= area->x2; x++) {
            *dst = *src;
            src++;
            dst -= lcd_sw;
        }
    }
}
#endif

void drm_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
   /*The most simple case (but also the slowest) to put all pixels to the screen one-by-one*/
    int32_t x;
    int32_t y;
    lv_coord_t w = (area->x2 - area->x1 + 1);
    lv_coord_t h = (area->y2 - area->y1 + 1);
#if 0//USE_RGA
    int wstride = w;
    int hstride = h;
    int lcd_ws = lcd_w;
    int lcd_hs = lcd_h;
    int format = 0;
    if(lcd_ws % 32 != 0) {
        lcd_ws = (lcd_ws + 32) & (~31);
    }
    if(lcd_hs % 32 != 0) {
        lcd_hs = (lcd_hs + 32) & (~31);
    }
    if (LV_COLOR_DEPTH == 16) {
        format = RK_FORMAT_RGB_565;
    }else if (LV_COLOR_DEPTH == 32) {
        format = RK_FORMAT_BGRA_8888;
    }else {
        format = -1;
        printf("drm_flush rga not supported format\n");
        return;
    }
#endif
    pthread_mutex_lock(&draw_mutex);
#if 0//USE_RGA
    rga_info_t src;
    rga_info_t dst;
    int area_w = area->x2 - area->x1 + 1;
    int area_h = area->y2 - area->y1 + 1;
    memset(&src, 0, sizeof(rga_info_t));
    memset(&dst, 0, sizeof(rga_info_t));
    src.virAddr = color_p;
    src.mmuFlag = 1;
    dst.fd = gbo->buf_fd;
    dst.mmuFlag = 1;
    rga_set_rect(&src.rect, 0, 0, area_w, area_h, wstride, hstride, format);
    rga_set_rect(&dst.rect, area->x1, area->y1, area_w, area_h, lcd_ws, lcd_hs, format);
    int ret = c_RkRgaBlit(&src, &dst, NULL);
    if (ret)
        printf("c_RkRgaBlit2 error : %s\n", strerror(errno));
#else
#if SW_ROTATION
    if (!disp_rot)
    {
#endif
        for(y = area->y1; y <= area->y2; y++) {
            int area_w = area->x2 - area->x1 + 1;
            lv_color_t *disp = (lv_color_t*)(drm_buff + (y * lcd_sw + area->x1) * (LV_COLOR_DEPTH >> 3));
            memcpy(disp, color_p, area_w * (LV_COLOR_DEPTH >> 3));
            color_p += area_w;
        }
#if SW_ROTATION
    }
    else
    {
        if (disp_rot == 90)
            rotate_90(disp_drv, area, color_p);
        else if (disp_rot == 180)
            rotate_180(disp_drv, area, color_p);
        else if (disp_rot == 270)
            rotate_270(disp_drv, area, color_p);
    }
#endif
#endif
    if(lv_disp_flush_is_last(disp_drv))
        draw_update = 1;
    pthread_mutex_unlock(&draw_mutex);
    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_disp_flush_ready(disp_drv);
}

void disp_init(int hor_res, int ver_res)
{
    int format = 0;
    int buf_w, buf_h;
    /*You code here*/
    drm_init(32);

    lv_env_get_u32("lv_disp_width", &lcd_w, hor_res);
    lv_env_get_u32("lv_disp_height", &lcd_h, ver_res);
    lv_env_get_u32("lv_disp_fps", &lcd_fps, 60);
    lv_env_get_u32("lv_disp_crop", &disp_crop, 0);

    if(lcd_w == 0 || lcd_h == 0)
        getdrmresolve(&lcd_w, &lcd_h);

    printf("%s res1:[%d x %d@%dfps] bit depth %d\n", __func__, lcd_w, lcd_h, lcd_fps, LV_COLOR_DEPTH);
    if (LV_COLOR_DEPTH == 16) {
        format = DRM_FORMAT_RGB565;
    } else {
        format = DRM_FORMAT_ARGB8888;
    }
    if ((disp_rot == 0) || (disp_rot == 180))
    {
        buf_w = lcd_w;
        buf_h = lcd_h;
    }
    else
    {
        buf_w = lcd_h;
        buf_h = lcd_w;
    }
#if USE_RGA
    gbo = malloc_drm_bo(buf_w, buf_h, format);
    drm_buff = gbo->ptr;
    lcd_sw = gbo->pitch / (LV_COLOR_DEPTH >> 3);
    c_RkRgaInit();
#else
    drm_buff = malloc(buf_w * buf_h * (LV_COLOR_DEPTH >> 3));
    memset(drm_buff, 0, buf_w * buf_h * (LV_COLOR_DEPTH >> 3));
    lcd_sw = lcd_w;
#endif
    vop_buf[0] = malloc_drm_bo(lcd_w, lcd_h, format);
    vop_buf[1] = malloc_drm_bo(lcd_w, lcd_h, format);

    drm_crtc_mode_frame_rate_adjust(pdev, format, lcd_fps);

    printf("DRM subsystem and buffer mapped successfully lcd_fps:%d\n", lcd_fps);
}

void disp_deinit(void)
{
    free_drm_bo(vop_buf[1]);
    free_drm_bo(vop_buf[0]);

#if USE_RGA
    free_drm_bo(gbo);
#else
    if (drm_buff)
        free(drm_buff);
#endif

    drm_exit();
}

void drm_disp_drv_deinit(void) {
    quit = 1;
    pthread_join(drm_thread_pid, NULL);
    pthread_mutex_destroy(&draw_mutex);
    if (lv_disp_drm != NULL)
        lv_disp_remove(lv_disp_drm); //align to lv_disp_drv_register

    if(buf_1 != NULL)
        free(buf_1);
    disp_deinit();
}

void drm_disp_drv_init(int hor_res, int ver_res, int rot)
{
    /*-------------------------
     * Initialize your display
     * -----------------------*/
#if USE_RGA || SW_ROTATION
    disp_rot = rot;
#endif
    disp_init(hor_res, ver_res);

    /*-----------------------------
     * Create a buffer for drawing
     *----------------------------*/

    /**
     * LVGL requires a buffer where it internally draws the widgets.
     * Later this buffer will passed to your display driver's `flush_cb` to copy its content to your display.
     * The buffer has to be greater than 1 display row
     *
     * There are 3 buffering configurations:
     * 1. Create ONE buffer:
     *      LVGL will draw the display's content here and writes it to your display
     *
     * 2. Create TWO buffer:
     *      LVGL will draw the display's content to a buffer and writes it your display.
     *      You should use DMA to write the buffer's content to the display.
     *      It will enable LVGL to draw the next part of the screen to the other buffer while
     *      the data is being sent form the first buffer. It makes rendering and flushing parallel.
     *
     * 3. Double buffering
     *      Set 2 screens sized buffers and set disp_drv.full_refresh = 1.
     *      This way LVGL will always provide the whole rendered screen in `flush_cb`
     *      and you only need to change the frame buffer's address.
     */

    /* Example for 1) */
    static lv_disp_draw_buf_t draw_buf_dsc_1;
    buf_1 = memalign(64, lcd_w * lcd_h * 4);

    lv_disp_draw_buf_init(&draw_buf_dsc_1, buf_1, NULL, lcd_w * lcd_h);   /*Initialize the display buffer*/

    /*-----------------------------------
     * Register the display in LVGL
     *----------------------------------*/

    static lv_disp_drv_t disp_drv;                         /*Descriptor of a display driver*/
    lv_disp_drv_init(&disp_drv);                    /*Basic initialization*/

    /*Set up the functions to access to your display*/

    /*Set the resolution of the display*/
    if ((disp_rot == 0) || (disp_rot == 180))
    {
        disp_drv.hor_res = lcd_w;
        disp_drv.ver_res = lcd_h;
    }
    else
    {
        disp_drv.hor_res = lcd_h;
        disp_drv.ver_res = lcd_w;
    }

    disp_drv.screen_transp = LV_COLOR_SCREEN_TRANSP;
    disp_drv.sw_rotate = 0;
    disp_drv.rotated = LV_DISP_ROT_NONE;
    /*Used to copy the buffer's content to the display*/
    disp_drv.flush_cb = drm_flush;

    /*Set a display buffer*/
    disp_drv.draw_buf = &draw_buf_dsc_1;

    /*Finally register the driver*/
    lv_disp_drm = lv_disp_drv_register(&disp_drv);
    pthread_mutex_init(&draw_mutex, NULL);
    pthread_create(&drm_thread_pid, NULL, drm_thread, NULL);
}

#endif
