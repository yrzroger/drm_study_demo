#define LOG_TAG "modeset-plane-test"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <log/log.h>
#include <libdrm_macros.h>
#include <drm_fourcc.h>

typedef struct buffer_object {
    int32_t  fd;    // point to drm device
	uint32_t width;
	uint32_t height;
	uint32_t pitch; // similar to stride
    uint32_t pixel_format;
	uint32_t handle;
	uint32_t size;
	void     *vaddr; 
	uint32_t fb_id;  // frame buffer id
} buffer_object;

static int modeset_create_dumb(int fd, unsigned width, unsigned height, unsigned depth, unsigned bpp, buffer_object* bo)
{
    struct drm_mode_create_dumb arg;
	memset(&arg, 0, sizeof(arg));
	arg.bpp = bpp;
	arg.width = width;
	arg.height = height;
    // 分配 dumb buffer 连续物理内存
    /* handle, pitch, size will be returned */
	int ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if(ret != 0) {
        ALOGD("Failed to create dumb buffer: %s", strerror(errno));
        return -1;
    }

	bo->fd = fd;
	bo->handle = arg.handle;
	bo->size = arg.size;
	bo->pitch = arg.pitch;
    bo->width = arg.width;
    bo->height = arg.height;
    bo->pixel_format = DRM_FORMAT_XRGB8888;
    
    ALOGD("buffer object info [%d %u %u %u %u %u]", bo->fd, bo->handle, bo->size, bo->pitch, bo->width, bo->height);

    // bind the dumb-buffer to an FB object

	//ret = drmModeAddFB(fd, bo->width, bo->height, depth, bpp, bo->pitch,
	//		   bo->handle, &bo->fb_id); 
    // pixel format 根据 bpp 和 depth 两个参数来决定，本例子 DRM_FORMAT_XRGB8888
    // 具体可以看kernel中的处理
     
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    handles[0] = bo->handle, pitches[0] = bo->pitch;
	ret = drmModeAddFB2(fd, bo->width, bo->height, bo->pixel_format, handles, pitches, offsets, &bo->fb_id, 0);

    if(ret != 0) {
        ALOGD("Failed to drmModeAddFB: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int modeset_map(buffer_object* bo)
{
	struct drm_mode_map_dumb arg;
	void *map = NULL;
	int ret = 0;

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;

	ret = drmIoctl(bo->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
	if (ret) {
        ALOGE("Failed to map dumb buffer: %s", strerror(errno));
		return ret;
    }

	map = drm_mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       bo->fd, arg.offset);
	if (map == MAP_FAILED) {
        ALOGE("Failed to drm_mmap: %s", strerror(errno));
		return -EINVAL;
    }

	bo->vaddr = map;

	return 0;
}

static void modeset_unmap(buffer_object* bo)
{
	if (!bo || !bo->vaddr)
		return;

	drm_munmap(bo->vaddr, bo->size);
	bo->vaddr = NULL;
}

static void modeset_fillColor(buffer_object* bo, uint32_t color)
{
    uint32_t* ptr = (uint32_t*)bo->vaddr;
    for(int i = 0; i < bo->size/4; ++i) {
        ptr[i] = color; // DRM_FORMAT_XRGB8888
    }
}

static void modeset_destroy(buffer_object* bo)
{
    if(bo->fb_id)
        drmModeRmFB(bo->fd, bo->fb_id); // Destroies the given framebuffer.

	struct drm_mode_destroy_dumb arg;
	int ret;

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;

	ret = drmIoctl(bo->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg); // destroy dumb-buffer
	if (ret)
		ALOGE("failed to destroy dumb buffer: %s\n", strerror(errno));
}

int main()
{
	int fd = -1;
	drmModeConnector *connector = NULL;
	drmModeRes *resources = NULL;
    drmModePlaneRes *plane_res = NULL;
	uint32_t conn_id = 0;
	uint32_t crtc_id = 0;
    uint32_t plane_id = 0;
    struct buffer_object buf = {0};
    int ret = 0;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC); // 打开DRM设备
    if(fd < 0) {
        ALOGE("Failed to open drm device, %s", strerror(errno));
        return -1;
    }

	resources = drmModeGetResources(fd); // 获取DRM资源，主要获取 crtc id 和 connector id
    if(!resources) {
        close(fd);
        ALOGE("Failed to get resources");
        return -1;
    }

	crtc_id = resources->crtcs[0];
	conn_id = resources->connectors[0];

    // 获取 Plane 信息
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	plane_res = drmModeGetPlaneResources(fd);
	plane_id = plane_res->planes[0];
    ALOGD("Select plane id : %u", plane_id);


	connector = drmModeGetConnector(fd, conn_id); // 根据connector id获取Connector
    if (!connector) {
        ALOGE("Failed to get connector %d", conn_id);
        return -1;
    }

	uint32_t width = connector->modes[0].hdisplay;  // 设置buffer宽
	uint32_t height = connector->modes[0].vdisplay; // 设置buffer高
    ALOGI("Display info: width=%u, height=%u", width, height);

	// 创建 dumb-buffer
    ret = modeset_create_dumb(fd, width, height, 24/*depth*/, 32/*bpp*/, &buf);
    if(ret) {
        return -1;
    }
    // map 映射得到虚拟地址，以便对 buffer 进行读写操作
    ret = modeset_map(&buf);
    if(ret) {
        return -1;
    }
    // 填充颜色
    modeset_fillColor(&buf, 0xff0000); // DRM_FORMAT_XRGB8888
    // unmap
    modeset_unmap(&buf);

    /** 显示 framebuffer 内容 **/
	//ret = drmModeSetCrtc(fd, crtc_id, buf.fb_id, 0, 0, &conn_id, 1, &connector->modes[0]);
    //if(ret)
    //    ALOGE("failed to set mode: %s\n", strerror(errno));
    //
	//getchar(); // 等待

	/** crop the rect from framebuffer(100, 150) to crtc(50, 50) */
    /** 我的平台无法做到裁剪和缩放 */
	ret = drmModeSetPlane(fd, plane_id, crtc_id, buf.fb_id, 0,
			480, 270, 960, 540,
			0<<16, 0<<16, buf.width<<16, buf.height<<16); // source的width&height是和分配的buffer大小一致吗？
    if(ret)
        ALOGE("failed to drmModeSetPlane: %s\n", strerror(errno));

    getchar();
    
    /** 释放资源 **/
	modeset_destroy(&buf);
	drmModeFreeConnector(connector);
    drmModeFreePlaneResources(plane_res);
	drmModeFreeResources(resources);
	close(fd);

	return 0;
}

/**
 * 本程序以演示流程为主，很多异常情况下，资源释放等善后工作均未考虑。
 */
