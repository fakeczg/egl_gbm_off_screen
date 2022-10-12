#include "log.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

struct egl {
    int card_fd;
    int render_fd;

    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLDeviceEXT device; // may be EGL_NO_DEVICE_EXT

    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;

    struct {
        PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
        PFNEGLQUERYWAYLANDBUFFERWL eglQueryWaylandBufferWL;
        PFNEGLBINDWAYLANDDISPLAYWL eglBindWaylandDisplayWL;
        PFNEGLUNBINDWAYLANDDISPLAYWL eglUnbindWaylandDisplayWL;
        PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT;
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;
        PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR;
        PFNEGLQUERYDISPLAYATTRIBEXTPROC eglQueryDisplayAttribEXT;
        PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT;
        PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT;
    } procs;

    struct {
        // Display extensions
        bool KHR_image_base;
        bool EXT_image_dma_buf_import;
        bool EXT_image_dma_buf_import_modifiers;
        bool IMG_context_priority;
        bool EGL_bind_display;

        // Device extensions
        bool EXT_device_drm;
        bool EXT_device_drm_render_node;

        // Client extensions
        bool EXT_device_query;
        bool KHR_platform_fake;
        bool EXT_platform_device;
    } exts;

    GLuint framebuffer;
    GLuint renderbuffer;
    GLuint texture_load;
    GLuint texture_render;

    // EGLImage image;
    uint16_t m_width;
    uint16_t m_height;
    // uint16_t m_handle;
    uint32_t m_frame_cnt;
    int m_data[4];
};
// struct
struct egl egl_fake;
// function
static bool check_egl_ext(const char *exts, const char *ext);
static void load_egl_proc(void *proc_ptr, const char *name);
static bool device_has_name(const drmDevice *device, const char *name);
// egl debug
static enum log_importance egl_log_importance(EGLint type);
static const char *egl_error_str(EGLint error);
static void egl_log(EGLenum error, const char *command, EGLint msg_type,
                    EGLLabelKHR thread, EGLLabelKHR obj, const char *msg);

EGLDeviceEXT get_egl_device_from_fd(void) {
    if (egl_fake.procs.eglQueryDevicesEXT == NULL) {
        fake_log(DEBUG, "EGL_EXT_device_enumeration not supported");
        return EGL_NO_DEVICE_EXT;
    }

    EGLint nb_devices = 0;
    // NULL -> to get supported devices num in the system
    if (!egl_fake.procs.eglQueryDevicesEXT(0, NULL, &nb_devices)) {
        fake_log(ERROR, "Failed to query EGL devices");
        return EGL_NO_DEVICE_EXT;
    }

    fake_log(INFO, "supported devices num is %d in the system", nb_devices);

    EGLDeviceEXT *devices = calloc(nb_devices, sizeof(EGLDeviceEXT));
    if (devices == NULL) {
        fake_log_errno(ERROR, "Failed to allocate EGL device list");
        return EGL_NO_DEVICE_EXT;
    }

    if (!egl_fake.procs.eglQueryDevicesEXT(nb_devices, devices, &nb_devices)) {
        fake_log(ERROR, "Failed to query EGL devices");
        return EGL_NO_DEVICE_EXT;
    }

    drmDevice *device = NULL;
    int ret = drmGetDevice(egl_fake.card_fd, &device);
    if (ret < 0) {
        fake_log(ERROR, "Failed to get DRM device: %s", strerror(-ret));
        return EGL_NO_DEVICE_EXT;
    }

    EGLDeviceEXT egl_device = NULL;
    for (int i = 0; i < nb_devices; i++) {
        const char *egl_device_name = egl_fake.procs.eglQueryDeviceStringEXT(
            devices[i], EGL_DRM_DEVICE_FILE_EXT);
        /* const char *egl_device_name = egl_fake.procs.eglQueryDeviceStringEXT(
         */
        /* 		devices[i], EGL_DRM_RENDER_NODE_FILE_EXT); */
        if (egl_device_name == NULL) {
            continue;
        }
        if (device_has_name(device, egl_device_name)) {
            fake_log(DEBUG, "Using EGL device %s", egl_device_name);
            egl_device = devices[i];
            break;
        }
    }

    drmFreeDevice(&device);
    free(devices);
    return egl_device;
}

static bool egl_init_display(EGLDisplay display) {
    egl_fake.display = display;
    EGLint major, minor;
    if (eglInitialize(egl_fake.display, &major, &minor) == EGL_FALSE) {
        fake_log(ERROR, "Failed to initialize EGL");
        return false;
    }

    fake_log(INFO, "Use Display Egl: major %d minor %d", major, minor);

    const char *display_exts_str =
        eglQueryString(egl_fake.display, EGL_EXTENSIONS);
    if (display_exts_str == NULL) {
        fake_log(ERROR, "Failed to query EGL display extensions");
        return false;
    }

    if (check_egl_ext(display_exts_str, "EGL_KHR_image_base")) {
        egl_fake.exts.KHR_image_base = true;
        load_egl_proc(&egl_fake.procs.eglCreateImageKHR, "eglCreateImageKHR");
        load_egl_proc(&egl_fake.procs.eglDestroyImageKHR, "eglDestroyImageKHR");
    }

    egl_fake.exts.EXT_image_dma_buf_import =
        check_egl_ext(display_exts_str, "EGL_EXT_image_dma_buf_import");
    if (check_egl_ext(display_exts_str,
                      "EGL_EXT_image_dma_buf_import_modifiers")) {
        egl_fake.exts.EXT_image_dma_buf_import_modifiers = true;
        load_egl_proc(&egl_fake.procs.eglQueryDmaBufFormatsEXT,
                      "eglQueryDmaBufFormatsEXT");
        load_egl_proc(&egl_fake.procs.eglQueryDmaBufModifiersEXT,
                      "eglQueryDmaBufModifiersEXT");
    }

    const char *device_exts_str = NULL, *driver_name = NULL;
    if (egl_fake.exts.EXT_device_query) {
        EGLAttrib device_attrib;
        if (!egl_fake.procs.eglQueryDisplayAttribEXT(
                egl_fake.display, EGL_DEVICE_EXT, &device_attrib)) {
            fake_log(ERROR, "eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed");
            return false;
        }
        egl_fake.device = (EGLDeviceEXT)device_attrib;
        device_exts_str = egl_fake.procs.eglQueryDeviceStringEXT(
            egl_fake.device, EGL_EXTENSIONS);
        if (device_exts_str == NULL) {
            fake_log(ERROR, "eglQueryDeviceStringEXT(EGL_EXTENSIONS) failed");
            return false;
        }
#ifdef EGL_DRIVER_NAME_EXT
        if (check_egl_ext(device_exts_str, "EGL_EXT_device_persistent_id")) {
            driver_name = egl_fake.procs.eglQueryDeviceStringEXT(
                egl_fake.device, EGL_DRIVER_NAME_EXT);
        }
#endif
        egl_fake.exts.EXT_device_drm =
            check_egl_ext(device_exts_str, "EGL_EXT_device_drm");
        egl_fake.exts.EXT_device_drm_render_node =
            check_egl_ext(device_exts_str, "EGL_EXT_device_drm_render_node");
    }

    fake_log(INFO, "Using EGL %d.%d", (int)major, (int)minor);
    fake_log(INFO, "Supported EGL client extensions:\n %s", display_exts_str);
    if (device_exts_str != NULL) {
        fake_log(INFO, "Supported EGL device extensions: %s", device_exts_str);
    }
    fake_log(INFO, "EGL vendor: %s",
             eglQueryString(egl_fake.display, EGL_VENDOR));
    if (driver_name != NULL) {
        fake_log(INFO, "EGL driver name: %s", driver_name);
    }

    return true;
}

static bool egl_init(EGLenum platform, void *remote_display) {
    EGLDisplay display =
        egl_fake.procs.eglGetPlatformDisplayEXT(platform, remote_display, NULL);
    if (display == EGL_NO_DISPLAY) {
        fake_log(ERROR, "Failed to create EGL display");
        return false;
    }
    if (!egl_init_display(display)) {
        eglTerminate(display);
        return false;
    }
    return true;
}

bool init_egl() {

    // create device
    // way1
    if (egl_fake.exts.EXT_platform_device) {
        /*
         * Search for the EGL device matching the DRM fd using the
         * EXT_device_enumeration extension.
         */
        EGLDeviceEXT egl_device = get_egl_device_from_fd();
        if (egl_device != EGL_NO_DEVICE_EXT) {
            if (egl_init(EGL_PLATFORM_DEVICE_EXT, egl_device)) {
                fake_log(DEBUG, "Using EGL_PLATFORM_DEVICE_EXT");
                return true;
            }
        }

    } else {
        fake_log(DEBUG, "EXT_platform_device not supported");
    }

    return false;
}

bool init_gbm(const char *path) {

    egl_fake.render_fd = open(path, O_RDWR | O_CLOEXEC);
    assert(0 != egl_fake.render_fd);

    egl_fake.gbm_device = gbm_create_device(egl_fake.render_fd);
    assert(0 != egl_fake.gbm_device);

    // https://mlog.club/article/1818788
    // https://cgit.freedesktop.org/mesa/mesa/commit/src/egl/main?id=468cc866b4b308cee40470f06b31002c6c56da96
    // 1. + 2. = 3. + 4.eglGetPlatformDisplay
    /* 1. eglGetDisplay((EGLNativeDisplayType)egl_fake.gbm_device);  */
    /* 2. eglCreateWindowSurface(egl_dpy, egl_config,
     * EGLNativeWindowType)my_fake_surface, NULL); */
    /* 3. eglGetPlatformDisplayEXT(EGL_PLATFORM_fake_MESA, my_fake_device,
     * NULL); */
    /* 4. eglCreatePlatformWindowSurfaceEXT(egl_dpy, egl_config,
     * my_fake_surface, NULL); */

    return true;
}

void check_egl() {
    const char *client_exts_str =
        eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (client_exts_str == NULL) {
        if (eglGetError() == EGL_BAD_DISPLAY) {
            fake_log(ERROR, "EGL_EXT_client_extensions not supported");
        } else {
            fake_log(ERROR, "Failed to query EGL client extensions");
        }
    }

    fake_log(INFO, "Supported EGL client extensions:\n %s", client_exts_str);

    if (!check_egl_ext(client_exts_str, "EGL_EXT_platform_base")) {
        fake_log(ERROR, " EGL_EXT_platform_base not supported");
    }

    load_egl_proc(&egl_fake.procs.eglGetPlatformDisplayEXT,
                  "eglGetPlatformDisplayEXT");

    egl_fake.exts.KHR_platform_fake =
        check_egl_ext(client_exts_str, "EGL_KHR_platform_fake");

    egl_fake.exts.EXT_platform_device =
        check_egl_ext(client_exts_str, "EGL_EXT_platform_device");

    if (check_egl_ext(client_exts_str, "EGL_EXT_device_base") ||
        check_egl_ext(client_exts_str, "EGL_EXT_device_enumeration")) {
        load_egl_proc(&egl_fake.procs.eglQueryDevicesEXT, "eglQueryDevicesEXT");
    }

    if (check_egl_ext(client_exts_str, "EGL_EXT_device_base") ||
        check_egl_ext(client_exts_str, "EGL_EXT_device_query")) {
        egl_fake.exts.EXT_device_query = true;
        load_egl_proc(&egl_fake.procs.eglQueryDeviceStringEXT,
                      "eglQueryDeviceStringEXT");
        load_egl_proc(&egl_fake.procs.eglQueryDisplayAttribEXT,
                      "eglQueryDisplayAttribEXT");
    }

    if (check_egl_ext(client_exts_str, "EGL_KHR_debug")) {
        load_egl_proc(&egl_fake.procs.eglDebugMessageControlKHR,
                      "eglDebugMessageControlKHR");

        static const EGLAttrib debug_attribs[] = {
            EGL_DEBUG_MSG_CRITICAL_KHR,
            EGL_TRUE,
            EGL_DEBUG_MSG_ERROR_KHR,
            EGL_TRUE,
            EGL_DEBUG_MSG_WARN_KHR,
            EGL_TRUE,
            EGL_DEBUG_MSG_INFO_KHR,
            EGL_TRUE,
            EGL_NONE,
        };
        egl_fake.procs.eglDebugMessageControlKHR(egl_log, debug_attribs);
    }

    assert(EGL_FALSE != eglBindAPI(EGL_OPENGL_ES_API));
}

bool init_kms(const char *path) {
    egl_fake.card_fd = open(path, O_RDWR | O_CLOEXEC);
    assert(-1 != egl_fake.card_fd);

    return true;
}

int main(int argc, char **argv) {

    log_init(DEBUG, NULL);

    fake_log(ERROR, "hello check!");

    init_kms("/dev/dri/card0");

    check_egl();

    // init_gbm("/dev/dri/renderD128");
    // init_fake("/dev/dri/card0");
    init_egl();

    fake_log(ERROR, "hello world!\r\n");
    return 0;
}

static bool check_egl_ext(const char *exts, const char *ext) {
    size_t extlen = strlen(ext);
    const char *end = exts + strlen(exts);

    while (exts < end) {
        if (*exts == ' ') {
            exts++;
            continue;
        }
        size_t n = strcspn(exts, " ");
        if (n == extlen && strncmp(ext, exts, n) == 0) {
            return true;
        }
        exts += n;
    }
    return false;
}

static void load_egl_proc(void *proc_ptr, const char *name) {
    void *proc = (void *)eglGetProcAddress(name);
    if (proc == NULL) {
        fake_log(ERROR, "eglGetProcAddress(%s) failed", name);
        abort();
    }
    *(void **)proc_ptr = proc;
}

static bool device_has_name(const drmDevice *device, const char *name) {
    for (size_t i = 0; i < DRM_NODE_MAX; i++) {
        if (!(device->available_nodes & (1 << i))) {
            continue;
        }
        if (strcmp(device->nodes[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static enum log_importance egl_log_importance(EGLint type) {
    switch (type) {
    case EGL_DEBUG_MSG_CRITICAL_KHR:
        return ERROR;
    case EGL_DEBUG_MSG_ERROR_KHR:
        return ERROR;
    case EGL_DEBUG_MSG_WARN_KHR:
        return ERROR;
    case EGL_DEBUG_MSG_INFO_KHR:
        return INFO;
    default:
        return INFO;
    }
}
static const char *egl_error_str(EGLint error) {
    switch (error) {
    case EGL_SUCCESS:
        return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
        return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
        return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
        return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
        return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:
        return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:
        return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:
        return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
        return "EGL_BAD_DISPLAY";
    case EGL_BAD_DEVICE_EXT:
        return "EGL_BAD_DEVICE_EXT";
    case EGL_BAD_SURFACE:
        return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:
        return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:
        return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:
        return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
        return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:
        return "EGL_CONTEXT_LOST";
    }
    return "unknown error";
}

static void egl_log(EGLenum error, const char *command, EGLint msg_type,
                    EGLLabelKHR thread, EGLLabelKHR obj, const char *msg) {
    _debug_log(egl_log_importance(msg_type),
               "[EGL] command: %s, error: %s (0x%x), message: \"%s\"", command,
               egl_error_str(error), error, msg);
}
