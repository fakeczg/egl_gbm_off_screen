#include "log.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/** A single DRM format, with a set of modifiers attached. */
struct drm_format {
    // The actual DRM format, from `drm_fourcc.h`
    uint32_t format;
    // The number of modifiers
    size_t len;
    // The capacity of the array; do not use.
    size_t capacity;
    // The actual modifiers
    uint64_t modifiers[];
};

struct drm_format_set {
    // The number of formats
    size_t len;
    // The capacity of the array; private to wlroots
    size_t capacity;
    // A pointer to an array of `struct wlr_drm_format *` of length `len`.
    struct drm_format **formats;
};

struct gles2_tex_shader {
    GLuint program;
    GLint proj;
    GLint tex;
    GLint alpha;
    GLint pos_attrib;
    GLint tex_attrib;
};

struct egl {
    int card_fd;
    int render_fd;

    EGLDisplay display;
    EGLContext context;
    EGLContext off_screen_context;
    EGLSurface window_surface;
    EGLDeviceEXT device; // may be EGL_NO_DEVICE_EXT

    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *gbm_bo;
    unsigned int handle;
    unsigned int pitch;
    unsigned int fb_id;
    uint64_t modifier;

    unsigned int connector_id;
    drmModeResPtr resources;
    drmModeConnectorPtr connector;
    drmModeModeInfo mode;
    drmModeEncoderPtr encoder;
    drmModeCrtcPtr crtc;

    bool has_modifiers;
    struct drm_format_set dmabuf_texture_formats;
    struct drm_format_set dmabuf_render_formats;

    struct {
        PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
        PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC
            eglCreatePlatformWindowSurfaceEXT;
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
        bool KHR_platform_gbm;
        bool EXT_platform_device;
    } exts;

    // FBO
    GLuint fbo;
    GLuint texture_target_1;
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

struct gles_renderer {
    float projection[9];
    struct egl *egl;
    int drm_fd;

    const char *exts_str;
    struct {
        bool EXT_read_format_bgra;
        bool KHR_debug;
        bool OES_egl_image_external;
        bool OES_egl_image;
        bool EXT_texture_type_2_10_10_10_REV;
        bool OES_texture_half_float_linear;
        bool EXT_texture_norm16;
    } exts;

    struct {
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
        PFNGLDEBUGMESSAGECALLBACKKHRPROC glDebugMessageCallbackKHR;
        PFNGLDEBUGMESSAGECONTROLKHRPROC glDebugMessageControlKHR;
        PFNGLPOPDEBUGGROUPKHRPROC glPopDebugGroupKHR;
        PFNGLPUSHDEBUGGROUPKHRPROC glPushDebugGroupKHR;
        PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
            glEGLImageTargetRenderbufferStorageOES;
    } procs;

    struct {
        struct {
            GLuint program;
            GLint proj;
            GLint color;
            GLint pos_attrib;
        } quad;
        struct gles2_tex_shader tex_rgba;
        struct gles2_tex_shader tex_rgbx;
        struct gles2_tex_shader tex_ext;
    } shaders;
    uint32_t viewport_width, viewport_height;
};

// struct
struct egl egl_gbm;
struct gles_renderer gles_fake;
// function
static bool check_gl_ext(const char *exts, const char *ext);
static void load_gl_proc(void *proc_ptr, const char *name);
bool egl_make_current(struct egl *egl);
static bool check_egl_ext(const char *exts, const char *ext);
static void load_egl_proc(void *proc_ptr, const char *name);
static bool device_has_name(const drmDevice *device, const char *name);
static bool env_parse_bool(const char *option);
static int get_egl_dmabuf_formats(struct egl *egl, int **formats);
static int get_egl_dmabuf_modifiers(struct egl *egl, int format,
        uint64_t **modifiers,
        EGLBoolean **external_only);
static struct drm_format **format_set_get_ref(struct drm_format_set *set,
        uint32_t format);

// egl debug
static enum log_importance egl_log_importance(EGLint type);
static const char *egl_error_str(EGLint error);
static void egl_log(EGLenum error, const char *command, EGLint msg_type,
        EGLLabelKHR thread, EGLLabelKHR obj, const char *msg);

struct drm_format *drm_format_create(uint32_t format) {
    size_t capacity = 4;
    struct drm_format *fmt =
        calloc(1, sizeof(*fmt) + sizeof(fmt->modifiers[0]) * capacity);
    if (!fmt) {
        fake_log(ERROR, "Allocation failed");
        return NULL;
    }
    fmt->format = format;
    fmt->capacity = capacity;
    return fmt;
}

bool drm_format_has(const struct drm_format *fmt, uint64_t modifier) {
    for (size_t i = 0; i < fmt->len; ++i) {
        if (fmt->modifiers[i] == modifier) {
            return true;
        }
    }
    return false;
}

bool drm_format_add(struct drm_format **fmt_ptr, uint64_t modifier) {
    struct drm_format *fmt = *fmt_ptr;

    if (drm_format_has(fmt, modifier)) {
        return true;
    }

    if (fmt->len == fmt->capacity) {
        size_t capacity = fmt->capacity ? fmt->capacity * 2 : 4;

        fmt = realloc(fmt, sizeof(*fmt) + sizeof(fmt->modifiers[0]) * capacity);
        if (!fmt) {
            fake_log(ERROR, "Allocation failed");
            return false;
        }

        fmt->capacity = capacity;
        *fmt_ptr = fmt;
    }

    fmt->modifiers[fmt->len++] = modifier;
    return true;
}

bool drm_format_set_add(struct drm_format_set *set, uint32_t format,
        uint64_t modifier) {
    assert(format != DRM_FORMAT_INVALID);

    struct drm_format **ptr = format_set_get_ref(set, format);
    if (ptr) {
        return drm_format_add(ptr, modifier);
    }

    struct drm_format *fmt = drm_format_create(format);
    if (!fmt) {
        return false;
    }

    if (!drm_format_add(&fmt, modifier)) {
        return false;
    }

    if (set->len == set->capacity) {
        size_t new = set->capacity ? set->capacity * 2 : 4;

        struct drm_format **tmp = realloc(
                set->formats, sizeof(*fmt) + sizeof(fmt->modifiers[0]) * new);
        if (!tmp) {
            fake_log(ERROR, "Allocation failed");
            free(fmt);
            return false;
        }

        set->capacity = new;
        set->formats = tmp;
    }

    set->formats[set->len++] = fmt;
    return true;
}

static void init_dmabuf_formats(struct egl *egl) {
    int *formats;
    int formats_len = get_egl_dmabuf_formats(egl, &formats);
    if (formats_len < 0) {
        return;
    }
    fake_log(ERROR, "egl support formats num = %d", formats_len);

    bool has_modifiers = false;
    for (int i = 0; i < formats_len; i++) {
        uint32_t fmt = formats[i];

        uint64_t *modifiers;
        EGLBoolean *external_only;
        int modifiers_len =
            get_egl_dmabuf_modifiers(egl, fmt, &modifiers, &external_only);
        if (modifiers_len < 0) {
            continue;
        }

        has_modifiers = has_modifiers || modifiers_len > 0;

        // EGL始终支持隐式修饰符
        drm_format_set_add(&egl_gbm.dmabuf_texture_formats, fmt,
                DRM_FORMAT_MOD_INVALID);
        drm_format_set_add(&egl->dmabuf_render_formats, fmt,
                DRM_FORMAT_MOD_INVALID);

        // 如果驱动程序没有明确说明，则假设支持线性布局
        if (modifiers_len == 0) {
            // Asume the linear layout is supported if the driver doesn't
            // explicitly say otherwise
            drm_format_set_add(&egl->dmabuf_texture_formats, fmt,
                    DRM_FORMAT_MOD_LINEAR);
            drm_format_set_add(&egl->dmabuf_render_formats, fmt,
                    DRM_FORMAT_MOD_LINEAR);
        }

        for (int j = 0; j < modifiers_len; j++) {
            drm_format_set_add(&egl->dmabuf_texture_formats, fmt, modifiers[j]);
            if (!external_only[j]) {
                drm_format_set_add(&egl->dmabuf_render_formats, fmt,
                        modifiers[j]);
            }
        }

        free(modifiers);
        free(external_only);
    }

    char *str_formats = malloc(formats_len * 5 + 1);
    if (str_formats == NULL) {
        goto out;
    }
    for (int i = 0; i < formats_len; i++) {
        snprintf(&str_formats[i * 5], (formats_len - i) * 5 + 1, "%.4s ",
                (char *)&formats[i]);
    }
    fake_log(INFO, "Supported DMA-BUF formats: %s", str_formats);
    fake_log(INFO, "EGL DMA-BUF format modifiers %s",
            has_modifiers ? "supported" : "unsupported");
    free(str_formats);

    egl->has_modifiers = has_modifiers;

out:
    free(formats);
}

static int open_render_node(int drm_fd) {
    char *render_name = drmGetRenderDeviceNameFromFd(drm_fd);

    if (render_name == NULL) {
        // This can happen on split render/display platforms, fallback to
        // primary node
        render_name = drmGetPrimaryDeviceNameFromFd(drm_fd);
        if (render_name == NULL) {
            fake_log(ERROR, "drmGetPrimaryDeviceNameFromFd failed");
            return -1;
        }
        fake_log(DEBUG,
                "DRM device '%s' has no render node, "
                "falling back to primary node",
                render_name);
    }

    int render_fd = open(render_name, O_RDWR | O_CLOEXEC);
    if (render_fd < 0) {
        fake_log(ERROR, "Failed to open DRM node '%s'", render_name);
    }
    free(render_name);
    return render_fd;
}

EGLDeviceEXT get_egl_device_from_fd(int fd) {
    if (egl_gbm.procs.eglQueryDevicesEXT == NULL) {
        fake_log(DEBUG, "EGL_EXT_device_enumeration not supported");
        return EGL_NO_DEVICE_EXT;
    }

    EGLint nb_devices = 0;
    // NULL -> to get supported devices num in the system
    if (!egl_gbm.procs.eglQueryDevicesEXT(0, NULL, &nb_devices)) {
        fake_log(ERROR, "Failed to query EGL devices");
        return EGL_NO_DEVICE_EXT;
    }

    fake_log(INFO, "supported devices num is %d in the system", nb_devices);

    EGLDeviceEXT *devices = calloc(nb_devices, sizeof(EGLDeviceEXT));
    if (devices == NULL) {
        fake_log_errno(ERROR, "Failed to allocate EGL device list");
        return EGL_NO_DEVICE_EXT;
    }

    if (!egl_gbm.procs.eglQueryDevicesEXT(nb_devices, devices, &nb_devices)) {
        fake_log(ERROR, "Failed to query EGL devices");
        return EGL_NO_DEVICE_EXT;
    }

    drmDevice *device = NULL;
    int ret = drmGetDevice(fd, &device);
    if (ret < 0) {
        fake_log(ERROR, "Failed to get DRM device: %s", strerror(-ret));
        return EGL_NO_DEVICE_EXT;
    }

    EGLDeviceEXT egl_device = NULL;
    for (int i = 0; i < nb_devices; i++) {
        const char *egl_device_name = egl_gbm.procs.eglQueryDeviceStringEXT(
                devices[i], EGL_DRM_DEVICE_FILE_EXT);
        /* const char *egl_device_name = egl_gbm.procs.eglQueryDeviceStringEXT(
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
    egl_gbm.display = display;

    EGLint major, minor;
    if (eglInitialize(egl_gbm.display, &major, &minor) == EGL_FALSE) {
        fake_log(ERROR, "Failed to initialize EGL");
        return false;
    }

    const char *display_exts_str =
        eglQueryString(egl_gbm.display, EGL_EXTENSIONS);
    if (display_exts_str == NULL) {
        fake_log(ERROR, "Failed to query EGL display extensions");
        return false;
    }

    if (check_egl_ext(display_exts_str, "EGL_KHR_image_base")) {
        egl_gbm.exts.KHR_image_base = true;
        load_egl_proc(&egl_gbm.procs.eglCreateImageKHR, "eglCreateImageKHR");
        load_egl_proc(&egl_gbm.procs.eglDestroyImageKHR, "eglDestroyImageKHR");
    }

    egl_gbm.exts.EXT_image_dma_buf_import =
        check_egl_ext(display_exts_str, "EGL_EXT_image_dma_buf_import");
    if (check_egl_ext(display_exts_str,
                "EGL_EXT_image_dma_buf_import_modifiers")) {
        egl_gbm.exts.EXT_image_dma_buf_import_modifiers = true;
        load_egl_proc(&egl_gbm.procs.eglQueryDmaBufFormatsEXT,
                "eglQueryDmaBufFormatsEXT");
        load_egl_proc(&egl_gbm.procs.eglQueryDmaBufModifiersEXT,
                "eglQueryDmaBufModifiersEXT");
    }

    const char *device_exts_str = NULL, *driver_name = NULL;
    if (egl_gbm.exts.EXT_device_query) {
        EGLAttrib device_attrib;
        if (!egl_gbm.procs.eglQueryDisplayAttribEXT(
                    egl_gbm.display, EGL_DEVICE_EXT, &device_attrib)) {
            fake_log(ERROR, "eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed");
            return false;
        }
        egl_gbm.device = (EGLDeviceEXT)device_attrib;

        device_exts_str = egl_gbm.procs.eglQueryDeviceStringEXT(egl_gbm.device,
                EGL_EXTENSIONS);
        if (device_exts_str == NULL) {
            fake_log(ERROR, "eglQueryDeviceStringEXT(EGL_EXTENSIONS) failed");
            return false;
        }

        if (check_egl_ext(device_exts_str, "EGL_MESA_device_software")) {
            if (env_parse_bool("EGL_RENDERER_ALLOW_SOFTWARE")) {
                fake_log(INFO, "Using software rendering");
            } else {
                fake_log(ERROR,
                        "Software rendering detected, please use "
                        "the WLR_RENDERER_ALLOW_SOFTWARE environment variable "
                        "to proceed");
                return false;
            }
        }

#ifdef EGL_DRIVER_NAME_EXT
        if (check_egl_ext(device_exts_str, "EGL_EXT_device_persistent_id")) {
            driver_name = egl_gbm.procs.eglQueryDeviceStringEXT(
                    egl_gbm.device, EGL_DRIVER_NAME_EXT);
        }
#endif
        egl_gbm.exts.EXT_device_drm =
            check_egl_ext(device_exts_str, "EGL_EXT_device_drm");
        egl_gbm.exts.EXT_device_drm_render_node =
            check_egl_ext(device_exts_str, "EGL_EXT_device_drm_render_node");
    }

    if (!check_egl_ext(display_exts_str, "EGL_KHR_no_config_context") &&
            !check_egl_ext(display_exts_str, "EGL_MESA_configless_context")) {
        fake_log(ERROR, "EGL_KHR_no_config_context or "
                "EGL_MESA_configless_context not supported");
        return false;
    }

    if (!check_egl_ext(display_exts_str, "EGL_KHR_surfaceless_context")) {
        fake_log(ERROR, "EGL_KHR_surfaceless_context not supported");
        return false;
    }

    egl_gbm.exts.IMG_context_priority =
        check_egl_ext(display_exts_str, "EGL_IMG_context_priority");

    fake_log(INFO, "Using EGL %d.%d", (int)major, (int)minor);
    fake_log(INFO, "Supported EGL display extensions:\n %s", display_exts_str);
    if (device_exts_str != NULL) {
        fake_log(INFO, "Supported EGL device extensions: %s", device_exts_str);
    }
    fake_log(INFO, "EGL vendor: %s",
            eglQueryString(egl_gbm.display, EGL_VENDOR));
    if (driver_name != NULL) {
        fake_log(INFO, "EGL driver name: %s", driver_name);
    }

    init_dmabuf_formats(&egl_gbm);

    return true;
}

static int match_config_to_visual(EGLDisplay egl_display, EGLint visual_id,
        EGLConfig *configs, int count) {

    EGLint id;
    for (int i = 0; i < count; ++i) {
        if (!eglGetConfigAttrib(egl_display, configs[i], EGL_NATIVE_VISUAL_ID,
                    &id))
            continue;
        if (id == visual_id)
            return i;
    }
    return -1;
}

static bool egl_init(EGLenum platform, void *remote_display) {
    EGLDisplay display =
        egl_gbm.procs.eglGetPlatformDisplayEXT(platform, remote_display, NULL);
    if (display == EGL_NO_DISPLAY) {
        fake_log(ERROR, "Failed to create EGL Display");
        return false;
    }
    if (!egl_init_display(display)) {
        eglTerminate(display);
        return false;
    }
    // use surface specify config
    const EGLint attribList[] = {
        EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
        EGL_NONE,
    };
    const EGLint config_attribs[] = {
        EGL_BUFFER_SIZE, 32, // color component bit 32
        EGL_DEPTH_SIZE, EGL_DONT_CARE,
        EGL_STENCIL_SIZE, EGL_DONT_CARE,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLint max_num_configs, num_configs, config_index;
    if (!eglGetConfigs(display, NULL, 0, &max_num_configs)) {
        fake_log(ERROR, "Failed to get display configs");
        return false;
    }
    fake_log(INFO, "Display config max num = %d", max_num_configs);
    EGLConfig *configs = malloc(num_configs * sizeof(EGLConfig));
    if (!eglChooseConfig(display, config_attribs, configs, max_num_configs,
                &num_configs)) {
        fake_log(ERROR, "Failed to choose specify configs");
        return false;
    }
    fake_log(INFO, "匹配 config_attribs Display choose config num = %d",
            num_configs);
    config_index = match_config_to_visual(display, GBM_FORMAT_ARGB8888, configs,
            num_configs);
    fake_log(INFO, "index = %d", config_index);
    // 1. egl_gbm.window_surface = eglCreateWindowSurface(egl_gbm.display,
    // configs[config_index], (EGLNativeWindowType)egl_gbm.gbm_surface,
    // attribList);
    // 2. egl_gbm.window_surface =
    // eglCreatePlatformWindowSurface(egl_gbm.display, configs[config_index],
    // egl_gbm.gbm_surface, (EGLAttrib *)attribList);
    egl_gbm.window_surface = egl_gbm.procs.eglCreatePlatformWindowSurfaceEXT(
            egl_gbm.display, configs[config_index], egl_gbm.gbm_surface,
            attribList);
    if (egl_gbm.window_surface == EGL_NO_SURFACE) {
        fake_log(ERROR, "Failed to create EGL Surface");
        return false;
    }

    size_t atti = 0;
    EGLint attribs[5];
    attribs[atti++] = EGL_CONTEXT_CLIENT_VERSION;
    attribs[atti++] = 2;

    // Request a high priority context if possible
    // TODO: only do this if we're running as the DRM master
    bool request_high_priority = egl_gbm.exts.IMG_context_priority;

    // Try to reschedule all of our rendering to be completed first. If it
    // fails, it will fallback to the default priority (MEDIUM).
    if (request_high_priority) {
        attribs[atti++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
        attribs[atti++] = EGL_CONTEXT_PRIORITY_HIGH_IMG;
    }

    attribs[atti++] = EGL_NONE;
    assert(atti <= sizeof(attribs) / sizeof(attribs[0]));

    egl_gbm.context = eglCreateContext(egl_gbm.display, configs[config_index],
            EGL_NO_CONTEXT, attribs);
    if (egl_gbm.context == EGL_NO_CONTEXT) {
        fake_log(ERROR, "Failed to create EGL context");
        return false;
    }

    if (request_high_priority) {
        EGLint priority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
        eglQueryContext(egl_gbm.display, egl_gbm.context,
                EGL_CONTEXT_PRIORITY_LEVEL_IMG, &priority);
        if (priority != EGL_CONTEXT_PRIORITY_HIGH_IMG) {
            fake_log(INFO, "Failed to obtain a high priority context");
        } else {
            fake_log(DEBUG, "Obtained high priority context");
        }
    }

    return true;
}

bool check_basic_egl() {
    const char *client_exts_str =
        eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (client_exts_str == NULL) {
        if (eglGetError() == EGL_BAD_DISPLAY) {
            fake_log(ERROR, "EGL_EXT_client_extensions not supported");
        } else {
            fake_log(ERROR, "Failed to query EGL client extensions");
        }
        return false;
    }

    fake_log(INFO, "Supported EGL client extensions:\n %s", client_exts_str);

    if (!check_egl_ext(client_exts_str, "EGL_EXT_platform_base")) {
        fake_log(ERROR, " EGL_EXT_platform_base not supported");
        return false;
    }

    load_egl_proc(&egl_gbm.procs.eglGetPlatformDisplayEXT,
            "eglGetPlatformDisplayEXT");

    load_egl_proc(&egl_gbm.procs.eglCreatePlatformWindowSurfaceEXT,
            "eglCreatePlatformWindowSurfaceEXT");

    egl_gbm.exts.KHR_platform_gbm =
        check_egl_ext(client_exts_str, "EGL_KHR_platform_gbm");

    egl_gbm.exts.EXT_platform_device =
        check_egl_ext(client_exts_str, "EGL_EXT_platform_device");

    if (check_egl_ext(client_exts_str, "EGL_EXT_device_base") ||
            check_egl_ext(client_exts_str, "EGL_EXT_device_enumeration")) {
        load_egl_proc(&egl_gbm.procs.eglQueryDevicesEXT, "eglQueryDevicesEXT");
    }

    if (check_egl_ext(client_exts_str, "EGL_EXT_device_base") ||
            check_egl_ext(client_exts_str, "EGL_EXT_device_query")) {
        egl_gbm.exts.EXT_device_query = true;
        load_egl_proc(&egl_gbm.procs.eglQueryDeviceStringEXT,
                "eglQueryDeviceStringEXT");
        load_egl_proc(&egl_gbm.procs.eglQueryDisplayAttribEXT,
                "eglQueryDisplayAttribEXT");
    }

    if (check_egl_ext(client_exts_str, "EGL_KHR_debug")) {
        load_egl_proc(&egl_gbm.procs.eglDebugMessageControlKHR,
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
        egl_gbm.procs.eglDebugMessageControlKHR(egl_log, debug_attribs);
    }

    if (EGL_FALSE == eglBindAPI(EGL_OPENGL_ES_API)) {
        fake_log(ERROR, "Failed to bind to the OpenGL ES API");
        return false;
    }

    return true;
}

bool init_egl() {

    // basic check egl
    if (!check_basic_egl()) {
        return false;
    }

    // create egl device
    egl_gbm.exts.EXT_platform_device = false;
    if (egl_gbm.exts.EXT_platform_device) {
        /*
         * Search for the EGL device matching the DRM fd using the
         * EXT_device_enumeration extension.
         */
        EGLDeviceEXT egl_device = get_egl_device_from_fd(egl_gbm.card_fd);
        if (egl_device != EGL_NO_DEVICE_EXT) {
            if (egl_init(EGL_PLATFORM_DEVICE_EXT, egl_device)) {
                fake_log(DEBUG, "Using EGL_PLATFORM_DEVICE_EXT");
                return true;
            }
            goto error;
        }

    } else {
        fake_log(DEBUG, "EXT_platform_device not supported");
    }

    if (egl_gbm.exts.KHR_platform_gbm) {
        // we use egl swapbuffer to set crtc muse card fd
        int gbm_fd = egl_gbm.card_fd;//open_render_node(egl_gbm.card_fd);
        if (gbm_fd < 0) {
            fake_log(ERROR, "Failed to open DRM render node");
            goto error;
        }

        egl_gbm.gbm_device = gbm_create_device(gbm_fd);
        if (!egl_gbm.gbm_device) {
            close(gbm_fd);
            fake_log(ERROR, "Failed to create GBM device");
            goto error;
        }

        // use gbm surface to gen window surface
        egl_gbm.gbm_surface = gbm_surface_create(
                egl_gbm.gbm_device, egl_gbm.mode.hdisplay, egl_gbm.mode.vdisplay,
                GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT |  GBM_BO_USE_RENDERING);
        if (!egl_gbm.gbm_surface) {
            gbm_device_destroy(egl_gbm.gbm_device);
            close(gbm_fd);
            fake_log(ERROR, "Failed to create GBM Surface");
            goto error;
        }

        // 这里注意，后面需要有一些显卡需要用card节点创建
        // 比如Mali-G76是要用car0来创建gbm_device，才能拿到EGL display的
        // 后面在修改代码；
        if (egl_init(EGL_PLATFORM_GBM_KHR, egl_gbm.gbm_device)) {
            fake_log(DEBUG, "Using EGL_PLATFORM_GBM_KHR");
            return true;
        }

        gbm_surface_destroy(egl_gbm.gbm_surface);
        gbm_device_destroy(egl_gbm.gbm_device);
        close(gbm_fd);
    } else {
        fake_log(DEBUG, "KHR_platform_gbm not supported");
    }

error:
    fake_log(ERROR, "Failed to initialize EGL context");
    if (egl_gbm.display) {
        eglMakeCurrent(egl_gbm.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                EGL_NO_CONTEXT);
        eglTerminate(egl_gbm.display);
    }
    eglReleaseThread();

    return false;
}

bool init_gbm(const char *path) {

    egl_gbm.render_fd = open(path, O_RDWR | O_CLOEXEC);
    assert(-1 != egl_gbm.render_fd);

    egl_gbm.gbm_device = gbm_create_device(egl_gbm.render_fd);
    assert(NULL != egl_gbm.gbm_device);

    // https://mlog.club/article/1818788
    // https://cgit.freedesktop.org/mesa/mesa/commit/src/egl/main?id=468cc866b4b308cee40470f06b31002c6c56da96
    // use surface to manage buffer
    egl_gbm.gbm_surface = gbm_surface_create(
            egl_gbm.gbm_device, egl_gbm.mode.hdisplay, egl_gbm.mode.vdisplay,
            GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    assert(NULL != egl_gbm.gbm_surface);

    return true;
}

static drmModeConnector *find_connector(int fd, drmModeRes *resources) {

    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *connector =
            drmModeGetConnector(fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED) {
            return connector;
        }
        drmModeFreeConnector(connector);
    }
    return NULL; // if no connector found
}

static drmModeEncoder *find_encoder(int fd, drmModeRes *resources,
        drmModeConnector *connector) {

    if (connector->encoder_id) {
        return drmModeGetEncoder(fd, connector->encoder_id);
    }
    return NULL; // if no encoder found
}

bool init_kms(const char *path) {
    egl_gbm.card_fd = open(path, O_RDWR | O_CLOEXEC);
    assert(-1 != egl_gbm.card_fd);

    egl_gbm.resources = drmModeGetResources(egl_gbm.card_fd);
    assert(NULL != egl_gbm.resources);

    egl_gbm.connector = find_connector(egl_gbm.card_fd, egl_gbm.resources);
    assert(NULL != egl_gbm.connector);

    fake_log(INFO, "Modes list:");
    for (int i = 0; i < egl_gbm.connector->count_modes; i++) {
        fake_log(INFO, "    Mode %d: %s", i, egl_gbm.connector->modes[i].name);
    }

    egl_gbm.connector_id = egl_gbm.connector->connector_id;
    egl_gbm.mode = egl_gbm.connector->modes[0];

    egl_gbm.encoder =
        find_encoder(egl_gbm.card_fd, egl_gbm.resources, egl_gbm.connector);
    assert(NULL != egl_gbm.encoder);

    egl_gbm.crtc = drmModeGetCrtc(egl_gbm.card_fd, egl_gbm.encoder->crtc_id);
    assert(NULL != egl_gbm.crtc);

    drmModeFreeEncoder(egl_gbm.encoder);
    drmModeFreeConnector(egl_gbm.connector);
    drmModeFreeResources(egl_gbm.resources);

    return true;
}

bool init_opengles(struct egl *egl) {
    if (!egl_make_current(egl)) {
        goto error;
    }

    const char *exts_str = (const char *)glGetString(GL_EXTENSIONS);
    if (exts_str == NULL) {
        fake_log(ERROR, "Failed to get GL_EXTENSIONS");
        goto error;
    }
    gles_fake.egl = egl;
    gles_fake.exts_str = exts_str;
    gles_fake.drm_fd = -1;

    if (!gles_fake.egl->exts.EXT_image_dma_buf_import) {
        fake_log(ERROR, "EGL_EXT_image_dma_buf_import not supported");
        goto error;
    }

    if (!check_gl_ext(exts_str, "GL_EXT_texture_format_BGRA8888")) {
        fake_log(ERROR, "BGRA8888 format not supported by GLES2");
        goto error;
    }
    if (!check_gl_ext(exts_str, "GL_EXT_unpack_subimage")) {
        fake_log(ERROR, "GL_EXT_unpack_subimage not supported");
        goto error;
    }

    gles_fake.exts.EXT_read_format_bgra =
        check_gl_ext(exts_str, "GL_EXT_read_format_bgra");

    gles_fake.exts.EXT_texture_type_2_10_10_10_REV =
        check_gl_ext(exts_str, "GL_EXT_texture_type_2_10_10_10_REV");

    gles_fake.exts.OES_texture_half_float_linear =
        check_gl_ext(exts_str, "GL_OES_texture_half_float_linear");

    gles_fake.exts.EXT_texture_norm16 =
        check_gl_ext(exts_str, "GL_EXT_texture_norm16");

    if (check_gl_ext(exts_str, "GL_KHR_debug")) {
        gles_fake.exts.KHR_debug = true;
        load_gl_proc(&gles_fake.procs.glDebugMessageCallbackKHR,
                "glDebugMessageCallbackKHR");
        load_gl_proc(&gles_fake.procs.glDebugMessageControlKHR,
                "glDebugMessageControlKHR");
    }

    if (check_gl_ext(exts_str, "GL_OES_EGL_image_external")) {
        gles_fake.exts.OES_egl_image_external = true;
        load_gl_proc(&gles_fake.procs.glEGLImageTargetTexture2DOES,
                "glEGLImageTargetTexture2DOES");
    }

    if (check_gl_ext(exts_str, "GL_OES_EGL_image")) {
        gles_fake.exts.OES_egl_image = true;
        load_gl_proc(&gles_fake.procs.glEGLImageTargetRenderbufferStorageOES,
                "glEGLImageTargetRenderbufferStorageOES");
    }

    fake_log(INFO, "Using %s", glGetString(GL_VERSION));
    fake_log(INFO, "GL vendor: %s", glGetString(GL_VENDOR));
    fake_log(INFO, "GL renderer: %s", glGetString(GL_RENDERER));
    fake_log(INFO, "Supported GLES2 extensions: %s", exts_str);

    return true;

error:
    return false;
}

static void draw_color_use_window_surface() {
    eglMakeCurrent(egl_gbm.display, egl_gbm.window_surface,
            egl_gbm.window_surface, egl_gbm.context);
    glClearColor(1.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(egl_gbm.display, egl_gbm.window_surface);
    eglMakeCurrent(egl_gbm.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
            EGL_NO_CONTEXT);
}

static void read_draw_to_file(EGLSurface draw, EGLSurface read, EGLContext context)
{
    eglMakeCurrent(egl_gbm.display, draw, read , context);
    static FILE *file = NULL;
    static GLbyte *pbits = NULL;  /* CPU memory to save image */
    static uint32_t frame_cnt = 0;
    uint32_t frame_size = 10 * 10 * 4;
    if (!file) {
        file = fopen("rgba.bin", "w+");
        assert(file);
        pbits = (GLbyte *)malloc(frame_size);
        assert(pbits);
    }
    glReadPixels(0, 0, 10, 10, GL_RGBA, GL_UNSIGNED_BYTE, pbits);
    size_t ret = fwrite(pbits, 1, frame_size, file);
}

static void scan_output_surface_to_display()
{
    eglMakeCurrent(egl_gbm.display, egl_gbm.window_surface,
            egl_gbm.window_surface, egl_gbm.context);
    egl_gbm.gbm_bo = gbm_surface_lock_front_buffer(egl_gbm.gbm_surface);
    egl_gbm.handle = gbm_bo_get_handle(egl_gbm.gbm_bo).u32;
    egl_gbm.pitch =
        gbm_bo_get_stride(egl_gbm.gbm_bo); // pitch = mode.hdisplay * 4
    // fake_log(ERROR, "handle = %d pitch = %d", egl_gbm.handle, egl_gbm.pitch);
    drmModeAddFB(egl_gbm.card_fd, egl_gbm.mode.hdisplay, egl_gbm.mode.vdisplay,
            24, 32, egl_gbm.pitch, egl_gbm.handle, &egl_gbm.fb_id);
    drmModeSetCrtc(egl_gbm.card_fd, egl_gbm.crtc->crtc_id, egl_gbm.fb_id, 0, 0,
            &egl_gbm.connector_id, 1, &egl_gbm.mode);
    getchar();
}

static void draw_color_to_fbo_texture(){

    // Texture
    //off_screen_context
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    egl_gbm.off_screen_context = eglCreateContext(egl_gbm.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs);
    eglMakeCurrent(egl_gbm.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
            egl_gbm.off_screen_context);

    glGenTextures(1, &egl_gbm.texture_target_1);
    glBindTexture(GL_TEXTURE_2D, egl_gbm.texture_target_1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, egl_gbm.mode.hdisplay, egl_gbm.mode.vdisplay, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    // Fbo
    glGenFramebuffers(1, &egl_gbm.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, egl_gbm.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, egl_gbm.texture_target_1, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO creation failed\n");
    }


    glClearColor(0.0f, 1.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();
    read_draw_to_file(EGL_NO_SURFACE, EGL_NO_SURFACE, egl_gbm.off_screen_context);


    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    eglMakeCurrent(egl_gbm.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
            EGL_NO_CONTEXT);

}
int main(int argc, char **argv) {

    log_init(DEBUG, NULL);

    fake_log(ERROR, "hello check!");

    init_kms("/dev/dri/card0");

    // gbm init move to init_egl
    // init_gbm("/dev/dri/renderD128");

    if (!init_egl())
        fake_log(ERROR, "The current device egl cannot meet the operating "
                "conditions of wlroots!!!");

    if (!init_opengles(&egl_gbm))
        fake_log(ERROR, "The current device opengles cannot meet the operating "
                "conditions of wlroots!!!");

    fake_log(ERROR, "hello world!");
    fake_log(ERROR, "start off-scrren draw!!!");
    draw_color_use_window_surface();
    //scan_output_surface_to_display();
    read_draw_to_file(egl_gbm.window_surface, egl_gbm.window_surface, egl_gbm.context);

    //draw_color_to_fbo_texture();
    return 0;
}

bool egl_make_current(struct egl *egl) {
    if (!eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                egl->context)) {
        fake_log(ERROR, "eglMakeCurrent failed");
        return false;
    }
    return true;
}

static bool check_gl_ext(const char *exts, const char *ext) {
    size_t extlen = strlen(ext);
    const char *end = exts + strlen(exts);

    while (exts < end) {
        if (exts[0] == ' ') {
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

static void load_gl_proc(void *proc_ptr, const char *name) {
    void *proc = (void *)eglGetProcAddress(name);
    if (proc == NULL) {
        fake_log(ERROR, "glGetProcAddress(%s) failed", name);
        abort();
    }
    *(void **)proc_ptr = proc;
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

static bool env_parse_bool(const char *option) {
    const char *env = getenv(option);
    if (env) {
        fake_log(INFO, "Loading %s option: %s", option, env);
    }

    if (!env || strcmp(env, "0") == 0) {
        return false;
    } else if (strcmp(env, "1") == 0) {
        return true;
    }

    fake_log(ERROR, "Unknown %s option: %s", option, env);
    return false;
}

static int get_egl_dmabuf_formats(struct egl *egl, int **formats) {
    if (!egl->exts.EXT_image_dma_buf_import) {
        fake_log(DEBUG, "DMA-BUF import extension not present");
        return -1;
    }

    // 当我们只有image_dmabuf_import扩展时，我们无法查询支持哪些格式。
    // DRM_FORMAT_ARGB8888和DRM_FORMAT_XRGB8888这两个是一直被支持的,这是尝试创建缓冲区的预定方式。
    // 当然只是一个猜测，但总比完全不支持dmabufs好，因为修改器扩展并不是到处都支持。
    if (!egl->exts.EXT_image_dma_buf_import_modifiers) {
        static const int fallback_formats[] = {
            DRM_FORMAT_ARGB8888,
            DRM_FORMAT_XRGB8888,
        };
        static unsigned num =
            sizeof(fallback_formats) / sizeof(fallback_formats[0]);

        *formats = calloc(num, sizeof(int));
        if (!*formats) {
            fake_log(ERROR, "Allocation failed");
            return -1;
        }

        memcpy(*formats, fallback_formats, num * sizeof(**formats));
        return num;
    }

    EGLint num;
    if (!egl->procs.eglQueryDmaBufFormatsEXT(egl->display, 0, NULL, &num)) {
        fake_log(ERROR, "Failed to query number of dmabuf formats");
        return -1;
    }

    *formats = calloc(num, sizeof(int));
    if (*formats == NULL) {
        fake_log(ERROR, "Allocation failed: %s", strerror(errno));
        return -1;
    }

    if (!egl->procs.eglQueryDmaBufFormatsEXT(egl->display, num, *formats,
                &num)) {
        fake_log(ERROR, "Failed to query dmabuf format");
        free(*formats);
        return -1;
    }
    return num;
}

static int get_egl_dmabuf_modifiers(struct egl *egl, int format,
        uint64_t **modifiers,
        EGLBoolean **external_only) {
    *modifiers = NULL;
    *external_only = NULL;

    if (!egl->exts.EXT_image_dma_buf_import) {
        fake_log(DEBUG, "DMA-BUF extension not present");
        return -1;
    }
    if (!egl->exts.EXT_image_dma_buf_import_modifiers) {
        return 0;
    }

    EGLint num;
    if (!egl->procs.eglQueryDmaBufModifiersEXT(egl->display, format, 0, NULL,
                NULL, &num)) {
        fake_log(ERROR, "Failed to query dmabuf number of modifiers");
        return -1;
    }
    if (num == 0) {
        return 0;
    }

    *modifiers = calloc(num, sizeof(uint64_t));
    if (*modifiers == NULL) {
        fake_log(ERROR, "Allocation failed");
        return -1;
    }
    *external_only = calloc(num, sizeof(EGLBoolean));
    if (*external_only == NULL) {
        fake_log(ERROR, "Allocation failed");
        free(*modifiers);
        *modifiers = NULL;
        return -1;
    }

    if (!egl->procs.eglQueryDmaBufModifiersEXT(
                egl->display, format, num, *modifiers, *external_only, &num)) {
        fake_log(ERROR, "Failed to query dmabuf modifiers");
        free(*modifiers);
        free(*external_only);
        return -1;
    }
    return num;
}

static struct drm_format **format_set_get_ref(struct drm_format_set *set,
        uint32_t format) {
    for (size_t i = 0; i < set->len; ++i) {
        if (set->formats[i]->format == format) {
            return &set->formats[i];
        }
    }

    return NULL;
}
