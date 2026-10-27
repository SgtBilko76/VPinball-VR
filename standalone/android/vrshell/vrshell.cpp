// VPX VR shell: shows the Android menu UI on a curved screen in an empty (black) VR space.
// Runs on Meta Quest and PICO from the same APK: it only needs core OpenXR plus
// XR_KHR_android_create_instance and XR_KHR_opengl_es_enable, which every Android runtime has.
//
// The menu is rendered by Android into a SurfaceTexture owned by this thread's GL context.
// Each new UI frame (and the pointer cursor) is drawn into a normal GLES swapchain, which is
// composited as a cylinder layer (XR_KHR_composition_layer_cylinder) or, without it, as a flat
// quad. No projection layer is submitted, so everything around the screen stays black (or shows
// the room with XR_FB_passthrough). The controller aim ray is intersected with the screen and
// reported to Java, which injects touch / hover / scroll events into the UI.

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VPX-VRShell", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VPX-VRShell", __VA_ARGS__)

#define XR_OK(call) \
   do { \
      XrResult r_ = (call); \
      if (XR_FAILED(r_)) { \
         LOGE("%s failed: %d", #call, (int)r_); \
         return false; \
      } \
   } while (0)

#ifndef XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME
#define XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_BD_controller_interaction"
#endif

namespace {

// Screen geometry: a cylinder around the viewer, slightly below eye level.
constexpr float kRadius = 2.0f;          // metres from the viewer
constexpr float kCentralAngle = 1.40f;   // ~80 degrees wide
constexpr float kCenterY = -0.10f;       // metres relative to the head start position
constexpr float kCursorRadiusPx = 13.f;  // cursor dot, in UI pixels

JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;
jclass g_cls = nullptr;
jmethodID g_onSurfaces = nullptr;
jmethodID g_onPointer = nullptr;
jmethodID g_onBack = nullptr;
jmethodID g_onModeChanged = nullptr;
jmethodID g_onFailed = nullptr;
jmethodID g_createTexture = nullptr;
jmethodID g_latchTexture = nullptr;
jmethodID g_releaseTexture = nullptr;
std::atomic<bool> g_mixedReality {false};

std::thread g_thread;
std::atomic<bool> g_stop {false};
bool g_started = false;
int g_width = 1920, g_height = 1080, g_gen = 0;

struct Vec3 { float x, y, z; };

Vec3 rotate(const XrQuaternionf& q, const Vec3& v)
{
   // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
   const Vec3 u {q.x, q.y, q.z};
   const Vec3 t {u.y * v.z - u.z * v.y + q.w * v.x, u.z * v.x - u.x * v.z + q.w * v.y, u.x * v.y - u.y * v.x + q.w * v.z};
   return {v.x + 2.f * (u.y * t.z - u.z * t.y), v.y + 2.f * (u.z * t.x - u.x * t.z), v.z + 2.f * (u.x * t.y - u.y * t.x)};
}

const char* kVertexShader = R"(#version 300 es
out vec2 vUv;
void main()
{
   // Full screen triangle, vUv in GL convention (0,0 = bottom left)
   vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
   vUv = p;
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTex;
uniform mat4 uMatrix;   // SurfaceTexture transform
uniform vec2 uSize;     // UI size in pixels
uniform vec3 uCursor;   // cursor x, y (pixels, top left origin), radius (0 = hidden)
uniform float uPressed;
uniform float uDecode;  // 1 = the swapchain is sRGB: decode the sRGB encoded UI pixels
in vec2 vUv;
out vec4 outColor;
void main()
{
   vec3 c = texture(uTex, (uMatrix * vec4(vUv, 0.0, 1.0)).xy).rgb;
   if (uCursor.z > 0.0)
   {
      float d = distance(vec2(vUv.x, 1.0 - vUv.y) * uSize, uCursor.xy);
      float outer = 1.0 - smoothstep(uCursor.z * 0.95 - 1.0, uCursor.z * 0.95, d);
      float inner = 1.0 - smoothstep(uCursor.z * 0.72 - 1.0, uCursor.z * 0.72, d);
      c = mix(c, vec3(0.0), outer * 0.63);
      c = mix(c, mix(vec3(1.0), vec3(0.902, 0.157, 0.157), uPressed), inner);
   }
   if (uDecode > 0.5)
      c = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
   outColor = vec4(c, 1.0);
}
)";

struct Shell
{
   EGLDisplay dpy = EGL_NO_DISPLAY;
   EGLContext ctx = EGL_NO_CONTEXT;
   EGLSurface pbuf = EGL_NO_SURFACE;
   EGLConfig config = nullptr;

   XrInstance instance = XR_NULL_HANDLE;
   XrSystemId system = XR_NULL_SYSTEM_ID;
   XrSession session = XR_NULL_HANDLE;
   XrSpace local = XR_NULL_HANDLE;
   XrSwapchain panel = XR_NULL_HANDLE;
   std::vector<XrSwapchainImageOpenGLESKHR> panelImages;
   bool panelValid = false; // an image has been released, so the layer can be submitted

   // UI copy
   GLuint oesTex = 0, program = 0, fbo = 0, vao = 0;
   GLint uMatrix = -1, uSize = -1, uCursor = -1, uPressed = -1, uDecode = -1, uTex = -1;
   bool srgbSwapchain = false;
   jobject surfaceTexture = nullptr; // global ref
   jfloatArray matrixArray = nullptr; // global ref
   float texMatrix[16] {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
   jlong lastFrame = -2;
   bool lastHit = false, lastPressed = false;
   float lastU = -1.f, lastV = -1.f;

   XrActionSet actionSet = XR_NULL_HANDLE;
   XrAction aim = XR_NULL_HANDLE, select = XR_NULL_HANDLE, back = XR_NULL_HANDLE, scroll = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE, toggleMode = XR_NULL_HANDLE;
   XrPath hand[2] {};
   XrSpace aimSpace[2] {XR_NULL_HANDLE, XR_NULL_HANDLE};

   bool hasCylinder = false; // XR_KHR_composition_layer_cylinder, else a flat quad
   bool hasPicoControllers = false; // XR_BD_controller_interaction
   bool hasPassthrough = false; // XR_FB_passthrough: mixed reality background
   XrPassthroughFB passthrough = XR_NULL_HANDLE;
   XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
   bool passthroughRunning = false;
   PFN_xrCreatePassthroughFB pfnCreatePassthrough = nullptr;
   PFN_xrDestroyPassthroughFB pfnDestroyPassthrough = nullptr;
   PFN_xrPassthroughStartFB pfnPassthroughStart = nullptr;
   PFN_xrPassthroughPauseFB pfnPassthroughPause = nullptr;
   PFN_xrCreatePassthroughLayerFB pfnCreatePassthroughLayer = nullptr;
   PFN_xrDestroyPassthroughLayerFB pfnDestroyPassthroughLayer = nullptr;
   PFN_xrPassthroughLayerResumeFB pfnPassthroughLayerResume = nullptr;
   PFN_xrPassthroughLayerPauseFB pfnPassthroughLayerPause = nullptr;

   XrSessionState state = XR_SESSION_STATE_UNKNOWN;
   bool running = false;
   bool exitRequested = false;

   int activeHand = 1;
   bool pressed[2] {false, false};

   JNIEnv* env = nullptr;

   bool jniFailed(const char* what)
   {
      if (!env->ExceptionCheck()) return false;
      LOGE("Java exception in %s", what);
      env->ExceptionDescribe();
      env->ExceptionClear();
      return true;
   }

   bool initEGL()
   {
      dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
      if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) { LOGE("eglInitialize failed"); return false; }
      const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
      EGLint count = 0;
      if (!eglChooseConfig(dpy, attribs, &config, 1, &count) || count < 1) { LOGE("eglChooseConfig failed"); return false; }
      const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
      ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxAttribs);
      if (ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
      const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
      pbuf = eglCreatePbufferSurface(dpy, config, pbAttribs);
      if (pbuf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, pbuf, pbuf, ctx)) { LOGE("eglMakeCurrent failed"); return false; }
      return true;
   }

   std::vector<XrExtensionProperties> extensions;

   bool hasExtension(const char* name)
   {
      for (const XrExtensionProperties& p : extensions)
         if (strcmp(p.extensionName, name) == 0) return true;
      return false;
   }

   bool initInstance()
   {
      PFN_xrInitializeLoaderKHR initLoader = nullptr;
      xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader);
      if (initLoader)
      {
         XrLoaderInitInfoAndroidKHR li {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
         li.applicationVM = g_vm;
         li.applicationContext = g_activity;
         initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&li);
      }

      uint32_t n = 0;
      XR_OK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
      extensions.assign(n, {XR_TYPE_EXTENSION_PROPERTIES});
      XR_OK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, extensions.data()));

      std::vector<const char*> enabled;
      for (const char* e : {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME})
      {
         if (!hasExtension(e)) { LOGE("Missing OpenXR extension %s", e); return false; }
         enabled.push_back(e);
      }
      hasCylinder = hasExtension(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
      if (hasCylinder) enabled.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
      hasPicoControllers = hasExtension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
      if (hasPicoControllers) enabled.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
      hasPassthrough = hasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME);
      if (hasPassthrough) enabled.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);

      XrInstanceCreateInfoAndroidKHR android {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
      android.applicationVM = g_vm;
      android.applicationActivity = g_activity;

      XrInstanceCreateInfo ci {XR_TYPE_INSTANCE_CREATE_INFO};
      ci.next = &android;
      strncpy(ci.applicationInfo.applicationName, "Visual Pinball Menu", XR_MAX_APPLICATION_NAME_SIZE - 1);
      strncpy(ci.applicationInfo.engineName, "VPX", XR_MAX_ENGINE_NAME_SIZE - 1);
      ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
      ci.enabledExtensionCount = (uint32_t)enabled.size();
      ci.enabledExtensionNames = enabled.data();
      XR_OK(xrCreateInstance(&ci, &instance));

      XrInstanceProperties ip {XR_TYPE_INSTANCE_PROPERTIES};
      if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip)))
         LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
      LOGI("Extensions: cylinder %d, pico controllers %d, passthrough %d", hasCylinder, hasPicoControllers, hasPassthrough);

      XrSystemGetInfo sgi {XR_TYPE_SYSTEM_GET_INFO};
      sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
      XR_OK(xrGetSystem(instance, &sgi, &system));

      PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = nullptr;
      XR_OK(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReqs));
      XrGraphicsRequirementsOpenGLESKHR reqs {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
      XR_OK(getReqs(instance, system, &reqs));
      return true;
   }

   bool initSession()
   {
      XrGraphicsBindingOpenGLESAndroidKHR gb {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
      gb.display = dpy;
      gb.config = config;
      gb.context = ctx;
      XrSessionCreateInfo sci {XR_TYPE_SESSION_CREATE_INFO};
      sci.next = &gb;
      sci.systemId = system;
      XR_OK(xrCreateSession(instance, &sci, &session));

      XrReferenceSpaceCreateInfo rsci {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
      rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
      rsci.poseInReferenceSpace.orientation.w = 1.f;
      XR_OK(xrCreateReferenceSpace(session, &rsci, &local));
      return true;
   }

   XrPath path(const char* s)
   {
      XrPath p = XR_NULL_PATH;
      xrStringToPath(instance, s, &p);
      return p;
   }

   bool createAction(const char* name, const char* localized, XrActionType type, XrAction& out)
   {
      XrActionCreateInfo aci {XR_TYPE_ACTION_CREATE_INFO};
      strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
      strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
      aci.actionType = type;
      aci.countSubactionPaths = 2;
      aci.subactionPaths = hand;
      XR_OK(xrCreateAction(actionSet, &aci, &out));
      return true;
   }

   // Non fatal: a runtime only has to accept the profiles of its own controllers.
   bool suggest(const char* profile, const XrActionSuggestedBinding* b, uint32_t count)
   {
      XrInteractionProfileSuggestedBinding s {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
      s.interactionProfile = path(profile);
      s.countSuggestedBindings = count;
      s.suggestedBindings = b;
      const XrResult r = xrSuggestInteractionProfileBindings(instance, &s);
      LOGI("Controller profile %s: %s", profile, XR_SUCCEEDED(r) ? "accepted" : "not supported");
      return XR_SUCCEEDED(r);
   }

   bool initActions()
   {
      hand[0] = path("/user/hand/left");
      hand[1] = path("/user/hand/right");

      XrActionSetCreateInfo asci {XR_TYPE_ACTION_SET_CREATE_INFO};
      strcpy(asci.actionSetName, "menu");
      strcpy(asci.localizedActionSetName, "Menu");
      XR_OK(xrCreateActionSet(instance, &asci, &actionSet));

      if (!createAction("aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, aim)) return false;
      if (!createAction("select", "Select", XR_ACTION_TYPE_FLOAT_INPUT, select)) return false;
      if (!createAction("back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT, back)) return false;
      if (!createAction("scroll", "Scroll", XR_ACTION_TYPE_VECTOR2F_INPUT, scroll)) return false;
      if (!createAction("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, haptic)) return false;
      if (!createAction("toggle_mode", "Switch MR / VR", XR_ACTION_TYPE_BOOLEAN_INPUT, toggleMode)) return false;

      // Meta Touch and the PICO controllers have the same buttons: X/Y left, A/B right, sticks, triggers.
      const XrActionSuggestedBinding touch[] = {
         {aim, path("/user/hand/left/input/aim/pose")}, {aim, path("/user/hand/right/input/aim/pose")},
         {select, path("/user/hand/left/input/trigger/value")}, {select, path("/user/hand/right/input/trigger/value")},
         {back, path("/user/hand/left/input/y/click")}, {back, path("/user/hand/right/input/b/click")},
         {scroll, path("/user/hand/left/input/thumbstick")}, {scroll, path("/user/hand/right/input/thumbstick")},
         {haptic, path("/user/hand/left/output/haptic")}, {haptic, path("/user/hand/right/output/haptic")},
         {toggleMode, path("/user/hand/left/input/thumbstick/click")},
      };
      const uint32_t touchCount = sizeof(touch) / sizeof(touch[0]);
      bool any = suggest("/interaction_profiles/oculus/touch_controller", touch, touchCount);
      if (hasPicoControllers)
      {
         any |= suggest("/interaction_profiles/bytedance/pico4_controller", touch, touchCount);
         any |= suggest("/interaction_profiles/bytedance/pico4s_controller", touch, touchCount);
         any |= suggest("/interaction_profiles/bytedance/pico_neo3_controller", touch, touchCount);
      }
      const XrActionSuggestedBinding simple[] = {
         {aim, path("/user/hand/left/input/aim/pose")}, {aim, path("/user/hand/right/input/aim/pose")},
         {select, path("/user/hand/left/input/select/click")}, {select, path("/user/hand/right/input/select/click")},
         {back, path("/user/hand/left/input/menu/click")}, {back, path("/user/hand/right/input/menu/click")},
         {haptic, path("/user/hand/left/output/haptic")}, {haptic, path("/user/hand/right/output/haptic")},
      };
      any |= suggest("/interaction_profiles/khr/simple_controller", simple, sizeof(simple) / sizeof(simple[0]));
      if (!any) { LOGE("No controller profile accepted"); return false; }

      XrSessionActionSetsAttachInfo ai {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
      ai.countActionSets = 1;
      ai.actionSets = &actionSet;
      XR_OK(xrAttachSessionActionSets(session, &ai));

      for (int i = 0; i < 2; i++)
      {
         XrActionSpaceCreateInfo sci {XR_TYPE_ACTION_SPACE_CREATE_INFO};
         sci.action = aim;
         sci.subactionPath = hand[i];
         sci.poseInActionSpace.orientation.w = 1.f;
         XR_OK(xrCreateActionSpace(session, &sci, &aimSpace[i]));
      }
      return true;
   }

   GLuint compile(GLenum type, const char* src)
   {
      GLuint s = glCreateShader(type);
      glShaderSource(s, 1, &src, nullptr);
      glCompileShader(s);
      GLint ok = 0;
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok)
      {
         char log[1024] {};
         glGetShaderInfoLog(s, sizeof(log), nullptr, log);
         LOGE("Shader compile failed: %s", log);
         glDeleteShader(s);
         return 0;
      }
      return s;
   }

   bool initRenderer()
   {
      const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
      const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
      if (!vs || !fs) return false;
      program = glCreateProgram();
      glAttachShader(program, vs);
      glAttachShader(program, fs);
      glLinkProgram(program);
      glDeleteShader(vs);
      glDeleteShader(fs);
      GLint ok = 0;
      glGetProgramiv(program, GL_LINK_STATUS, &ok);
      if (!ok) { LOGE("Shader link failed"); return false; }
      uTex = glGetUniformLocation(program, "uTex");
      uMatrix = glGetUniformLocation(program, "uMatrix");
      uSize = glGetUniformLocation(program, "uSize");
      uCursor = glGetUniformLocation(program, "uCursor");
      uPressed = glGetUniformLocation(program, "uPressed");
      uDecode = glGetUniformLocation(program, "uDecode");
      glGenVertexArrays(1, &vao);
      glGenFramebuffers(1, &fbo);

      glGenTextures(1, &oesTex);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTex);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
      return glGetError() == GL_NO_ERROR;
   }

   bool initSwapchains()
   {
      uint32_t n = 0;
      XR_OK(xrEnumerateSwapchainFormats(session, 0, &n, nullptr));
      std::vector<int64_t> formats(n);
      XR_OK(xrEnumerateSwapchainFormats(session, n, &n, formats.data()));
      int64_t format = 0;
      for (int64_t f : formats) if (f == GL_SRGB8_ALPHA8) { format = f; srgbSwapchain = true; }
      if (!format) for (int64_t f : formats) if (f == GL_RGBA8) format = f;
      if (!format && n) format = formats[0];
      LOGI("Panel swapchain format 0x%llx (%s)", (long long)format, srgbSwapchain ? "sRGB" : "linear");

      XrSwapchainCreateInfo sc {XR_TYPE_SWAPCHAIN_CREATE_INFO};
      sc.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
      sc.format = format;
      sc.sampleCount = 1;
      sc.width = g_width;
      sc.height = g_height;
      sc.faceCount = 1;
      sc.arraySize = 1;
      sc.mipCount = 1;
      XR_OK(xrCreateSwapchain(session, &sc, &panel));
      XR_OK(xrEnumerateSwapchainImages(panel, 0, &n, nullptr));
      panelImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
      XR_OK(xrEnumerateSwapchainImages(panel, n, &n, (XrSwapchainImageBaseHeader*)panelImages.data()));

      // The UI is drawn by Android into a SurfaceTexture bound to oesTex (created on this thread, context current).
      jobject st = env->CallStaticObjectMethod(g_cls, g_createTexture, (jint)oesTex, (jint)g_width, (jint)g_height);
      if (jniFailed("createPanelTexture") || !st) return false;
      surfaceTexture = env->NewGlobalRef(st);
      env->DeleteLocalRef(st);
      jfloatArray m = env->NewFloatArray(16);
      matrixArray = (jfloatArray)env->NewGlobalRef(m);
      env->DeleteLocalRef(m);

      env->CallStaticVoidMethod(g_cls, g_onSurfaces, g_gen, surfaceTexture);
      return !jniFailed("onSurfaces");
   }

   bool initPassthrough()
   {
      if (!hasPassthrough) return true;
#define XR_PFN(name, var) \
      if (XR_FAILED(xrGetInstanceProcAddr(instance, name, (PFN_xrVoidFunction*)&var)) || !var) { hasPassthrough = false; LOGE("No %s", name); return true; }
      XR_PFN("xrCreatePassthroughFB", pfnCreatePassthrough)
      XR_PFN("xrDestroyPassthroughFB", pfnDestroyPassthrough)
      XR_PFN("xrPassthroughStartFB", pfnPassthroughStart)
      XR_PFN("xrPassthroughPauseFB", pfnPassthroughPause)
      XR_PFN("xrCreatePassthroughLayerFB", pfnCreatePassthroughLayer)
      XR_PFN("xrDestroyPassthroughLayerFB", pfnDestroyPassthroughLayer)
      XR_PFN("xrPassthroughLayerResumeFB", pfnPassthroughLayerResume)
      XR_PFN("xrPassthroughLayerPauseFB", pfnPassthroughLayerPause)
#undef XR_PFN
      XrPassthroughCreateInfoFB pci {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
      if (XR_FAILED(pfnCreatePassthrough(session, &pci, &passthrough))) { hasPassthrough = false; LOGE("xrCreatePassthroughFB failed"); return true; }
      XrPassthroughLayerCreateInfoFB lci {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
      lci.passthrough = passthrough;
      lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
      if (XR_FAILED(pfnCreatePassthroughLayer(session, &lci, &passthroughLayer)))
      {
         pfnDestroyPassthrough(passthrough);
         passthrough = XR_NULL_HANDLE;
         hasPassthrough = false;
         LOGE("xrCreatePassthroughLayerFB failed");
         return true;
      }
      applyMode();
      return true;
   }

   // Starts or pauses the camera feed to match the selected mode (paused costs no battery).
   void applyMode()
   {
      if (!hasPassthrough) return;
      const bool want = g_mixedReality.load();
      if (want == passthroughRunning) return;
      if (want)
      {
         pfnPassthroughStart(passthrough);
         pfnPassthroughLayerResume(passthroughLayer);
      }
      else
      {
         pfnPassthroughLayerPause(passthroughLayer);
         pfnPassthroughPause(passthrough);
      }
      passthroughRunning = want;
      LOGI("Background: %s", want ? "mixed reality" : "virtual reality (black)");
   }

   void handleEvents()
   {
      XrEventDataBuffer ev {XR_TYPE_EVENT_DATA_BUFFER};
      while (xrPollEvent(instance, &ev) == XR_SUCCESS)
      {
         if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
         {
            state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
            LOGI("Session state %d", (int)state);
            if (state == XR_SESSION_STATE_READY)
            {
               XrSessionBeginInfo bi {XR_TYPE_SESSION_BEGIN_INFO};
               bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
               if (XR_SUCCEEDED(xrBeginSession(session, &bi))) running = true;
            }
            else if (state == XR_SESSION_STATE_STOPPING)
            {
               xrEndSession(session);
               running = false;
            }
            else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
            {
               running = false;
               exitRequested = true;
            }
         }
         else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            exitRequested = true;
         ev = {XR_TYPE_EVENT_DATA_BUFFER};
      }
   }

   float actionFloat(XrAction a, int h)
   {
      XrActionStateGetInfo gi {XR_TYPE_ACTION_STATE_GET_INFO};
      gi.action = a;
      gi.subactionPath = hand[h];
      XrActionStateFloat s {XR_TYPE_ACTION_STATE_FLOAT};
      xrGetActionStateFloat(session, &gi, &s);
      return s.isActive ? s.currentState : 0.f;
   }

   void buzz(int h)
   {
      XrHapticVibration v {XR_TYPE_HAPTIC_VIBRATION};
      v.amplitude = 0.35f;
      v.duration = 12000000; // 12 ms
      v.frequency = XR_FREQUENCY_UNSPECIFIED;
      XrHapticActionInfo hi {XR_TYPE_HAPTIC_ACTION_INFO};
      hi.action = haptic;
      hi.subactionPath = hand[h];
      xrApplyHapticFeedback(session, &hi, (const XrHapticBaseHeader*)&v);
   }

   // Screen size in metres: the cylinder's arc length, or the same width for the flat fallback.
   float screenWidth() const { return kRadius * kCentralAngle; }
   float screenHeight() const { return screenWidth() * (float)g_height / (float)g_width; }

   // Intersects the aim ray with the screen; returns true with (u, v) in [0, 1] (top left origin) on a hit.
   bool hitTest(const Vec3& o, const Vec3& d, float& u, float& v)
   {
      if (hasCylinder)
      {
         // Vertical cylinder around the origin (we are inside it).
         const float a = d.x * d.x + d.z * d.z;
         const float b = 2.f * (o.x * d.x + o.z * d.z);
         const float c = o.x * o.x + o.z * o.z - kRadius * kRadius;
         const float disc = b * b - 4.f * a * c;
         if (a <= 1e-6f || disc < 0.f) return false;
         const float t = (-b + std::sqrt(disc)) / (2.f * a);
         if (t <= 0.f) return false;
         const Vec3 p {o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
         u = 0.5f + std::atan2(p.x, -p.z) / kCentralAngle;
         v = 0.5f - (p.y - kCenterY) / screenHeight();
      }
      else
      {
         // Flat quad facing the viewer at z = -radius.
         if (d.z >= -1e-6f) return false;
         const float t = (-kRadius - o.z) / d.z;
         if (t <= 0.f) return false;
         u = 0.5f + (o.x + t * d.x) / screenWidth();
         v = 0.5f - (o.y + t * d.y - kCenterY) / screenHeight();
      }
      return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
   }

   // Polls the controllers, reports the pointer to Java and returns whether it is on the screen.
   bool updateInput(XrTime time, float& u, float& v)
   {
      if (state != XR_SESSION_STATE_FOCUSED)
      {
         env->CallStaticVoidMethod(g_cls, g_onPointer, g_gen, activeHand, JNI_FALSE, 0.f, 0.f, JNI_FALSE, 0.f);
         jniFailed("onPointer");
         return false;
      }

      XrActiveActionSet aas {actionSet, XR_NULL_PATH};
      XrActionsSyncInfo si {XR_TYPE_ACTIONS_SYNC_INFO};
      si.countActiveActionSets = 1;
      si.activeActionSets = &aas;
      xrSyncActions(session, &si);

      for (int h = 0; h < 2; h++)
      {
         const float t = actionFloat(select, h);
         const bool now = pressed[h] ? (t > 0.35f) : (t > 0.65f);
         if (now && !pressed[h])
         {
            if (h != activeHand) activeHand = h; // pressing a trigger makes that controller the pointer
            buzz(h);
         }
         pressed[h] = now;
      }

      {
         XrActionStateGetInfo gi {XR_TYPE_ACTION_STATE_GET_INFO};
         gi.action = back;
         XrActionStateBoolean b {XR_TYPE_ACTION_STATE_BOOLEAN};
         xrGetActionStateBoolean(session, &gi, &b);
         if (b.isActive && b.changedSinceLastSync && b.currentState)
         {
            env->CallStaticVoidMethod(g_cls, g_onBack, g_gen);
            jniFailed("onBack");
         }
      }

      if (hasPassthrough)
      {
         XrActionStateGetInfo gi {XR_TYPE_ACTION_STATE_GET_INFO};
         gi.action = toggleMode;
         XrActionStateBoolean b {XR_TYPE_ACTION_STATE_BOOLEAN};
         xrGetActionStateBoolean(session, &gi, &b);
         if (b.isActive && b.changedSinceLastSync && b.currentState)
         {
            g_mixedReality = !g_mixedReality.load();
            applyMode();
            buzz(0);
            env->CallStaticVoidMethod(g_cls, g_onModeChanged, g_gen, g_mixedReality.load() ? JNI_TRUE : JNI_FALSE);
            jniFailed("onModeChanged");
         }
      }

      float scrollY = 0.f;
      {
         XrActionStateGetInfo gi {XR_TYPE_ACTION_STATE_GET_INFO};
         gi.action = scroll;
         gi.subactionPath = hand[activeHand];
         XrActionStateVector2f s {XR_TYPE_ACTION_STATE_VECTOR2F};
         xrGetActionStateVector2f(session, &gi, &s);
         if (s.isActive && std::fabs(s.currentState.y) > 0.2f) scrollY = s.currentState.y;
      }

      bool hit = false;
      XrSpaceLocation loc {XR_TYPE_SPACE_LOCATION};
      if (XR_SUCCEEDED(xrLocateSpace(aimSpace[activeHand], local, time, &loc))
         && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
      {
         const Vec3 o {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
         hit = hitTest(o, rotate(loc.pose.orientation, {0.f, 0.f, -1.f}), u, v);
      }

      env->CallStaticVoidMethod(g_cls, g_onPointer, g_gen, activeHand, hit ? JNI_TRUE : JNI_FALSE, u, v,
         pressed[activeHand] ? JNI_TRUE : JNI_FALSE, scrollY);
      jniFailed("onPointer");
      return hit;
   }

   // Copies the newest UI frame and the cursor into the panel swapchain when anything changed.
   void updatePanel(bool hit, float u, float v)
   {
      const jlong frameId = env->CallStaticLongMethod(g_cls, g_latchTexture, surfaceTexture, matrixArray);
      if (jniFailed("latchPanelTexture")) return;
      const bool isPressed = hit && pressed[activeHand];
      const bool cursorMoved = hit != lastHit || isPressed != lastPressed
         || (hit && (std::fabs(u - lastU) * g_width > 0.25f || std::fabs(v - lastV) * g_height > 0.25f));
      if (panelValid && frameId == lastFrame && !cursorMoved) return;
      env->GetFloatArrayRegion(matrixArray, 0, 16, texMatrix);

      uint32_t index = 0;
      XrSwapchainImageAcquireInfo ai {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      if (XR_FAILED(xrAcquireSwapchainImage(panel, &ai, &index))) return;
      XrSwapchainImageWaitInfo wi {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wi.timeout = XR_INFINITE_DURATION;
      if (XR_FAILED(xrWaitSwapchainImage(panel, &wi))) return;

      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, panelImages[index].image, 0);
      glViewport(0, 0, g_width, g_height);
      glDisable(GL_BLEND);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_SCISSOR_TEST);
      glUseProgram(program);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTex);
      glUniform1i(uTex, 0);
      glUniformMatrix4fv(uMatrix, 1, GL_FALSE, texMatrix);
      glUniform2f(uSize, (float)g_width, (float)g_height);
      glUniform3f(uCursor, u * g_width, v * g_height, hit ? kCursorRadiusPx : 0.f);
      glUniform1f(uPressed, isPressed ? 1.f : 0.f);
      glUniform1f(uDecode, srgbSwapchain ? 1.f : 0.f);
      glBindVertexArray(vao);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glBindVertexArray(0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glFlush();

      XrSwapchainImageReleaseInfo ri {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      if (XR_SUCCEEDED(xrReleaseSwapchainImage(panel, &ri))) panelValid = true;
      lastFrame = frameId;
      lastHit = hit;
      lastPressed = isPressed;
      lastU = u;
      lastV = v;
   }

   void frame()
   {
      XrFrameWaitInfo wi {XR_TYPE_FRAME_WAIT_INFO};
      XrFrameState fs {XR_TYPE_FRAME_STATE};
      if (XR_FAILED(xrWaitFrame(session, &wi, &fs))) return;
      XrFrameBeginInfo bi {XR_TYPE_FRAME_BEGIN_INFO};
      if (XR_FAILED(xrBeginFrame(session, &bi))) return;

      float u = 0.f, v = 0.f;
      const bool hit = updateInput(fs.predictedDisplayTime, u, v);
      updatePanel(hit, u, v);

      XrCompositionLayerCylinderKHR cyl {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
      cyl.space = local;
      cyl.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      cyl.subImage.swapchain = panel;
      cyl.subImage.imageRect = {{0, 0}, {g_width, g_height}};
      cyl.subImage.imageArrayIndex = 0;
      cyl.pose.orientation.w = 1.f;
      cyl.pose.position = {0.f, kCenterY, 0.f};
      cyl.radius = kRadius;
      cyl.centralAngle = kCentralAngle;
      cyl.aspectRatio = (float)g_width / (float)g_height;

      XrCompositionLayerQuad quad {XR_TYPE_COMPOSITION_LAYER_QUAD};
      quad.space = local;
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      quad.subImage = cyl.subImage;
      quad.pose.orientation.w = 1.f;
      quad.pose.position = {0.f, kCenterY, -kRadius};
      quad.size = {screenWidth(), screenHeight()};

      XrCompositionLayerPassthroughFB pt {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
      pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      pt.space = XR_NULL_HANDLE;
      pt.layerHandle = passthroughLayer;

      const XrCompositionLayerBaseHeader* layers[2];
      uint32_t count = 0;
      if (passthroughRunning) layers[count++] = (const XrCompositionLayerBaseHeader*)&pt;
      if (panelValid) layers[count++] = hasCylinder ? (const XrCompositionLayerBaseHeader*)&cyl : (const XrCompositionLayerBaseHeader*)&quad;

      XrFrameEndInfo ei {XR_TYPE_FRAME_END_INFO};
      ei.displayTime = fs.predictedDisplayTime;
      ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
      ei.layerCount = fs.shouldRender ? count : 0;
      ei.layers = layers;
      xrEndFrame(session, &ei);
   }

   void run()
   {
      if (!initEGL() || !initInstance() || !initSession() || !initActions() || !initPassthrough() || !initRenderer() || !initSwapchains())
      {
         LOGE("VR shell failed to start");
         env->CallStaticVoidMethod(g_cls, g_onFailed, g_gen);
         jniFailed("onFailed");
         return;
      }
      LOGI("VR shell started (%dx%d, %s)", g_width, g_height, hasCylinder ? "curved screen" : "flat screen");
      while (!g_stop.load() && !exitRequested)
      {
         handleEvents();
         if (!running) { usleep(10000); continue; }
         frame();
      }
   }

   void shutdown()
   {
      if (surfaceTexture)
      {
         env->CallStaticVoidMethod(g_cls, g_releaseTexture, surfaceTexture);
         jniFailed("releasePanelTexture");
         env->DeleteGlobalRef(surfaceTexture);
         surfaceTexture = nullptr;
      }
      if (matrixArray) { env->DeleteGlobalRef(matrixArray); matrixArray = nullptr; }
      if (passthroughLayer != XR_NULL_HANDLE) pfnDestroyPassthroughLayer(passthroughLayer);
      if (passthrough != XR_NULL_HANDLE) pfnDestroyPassthrough(passthrough);
      for (XrSpace& s : aimSpace) if (s != XR_NULL_HANDLE) { xrDestroySpace(s); s = XR_NULL_HANDLE; }
      if (panel != XR_NULL_HANDLE) xrDestroySwapchain(panel);
      if (local != XR_NULL_HANDLE) xrDestroySpace(local);
      if (actionSet != XR_NULL_HANDLE) xrDestroyActionSet(actionSet);
      if (session != XR_NULL_HANDLE) xrDestroySession(session);
      if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
      if (dpy != EGL_NO_DISPLAY)
      {
         if (ctx != EGL_NO_CONTEXT)
         {
            if (oesTex) glDeleteTextures(1, &oesTex);
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (vao) glDeleteVertexArrays(1, &vao);
            if (program) glDeleteProgram(program);
         }
         eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
         if (pbuf != EGL_NO_SURFACE) eglDestroySurface(dpy, pbuf);
         if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
         eglTerminate(dpy);
      }
      LOGI("VR shell stopped");
   }
};

void threadMain()
{
   JNIEnv* env = nullptr;
   if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { LOGE("AttachCurrentThread failed"); return; }
   {
      Shell shell;
      shell.env = env;
      shell.run();
      shell.shutdown();
   }
   g_vm->DetachCurrentThread();
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL Java_org_vpinball_app_vr_VrShell_nativeStart(JNIEnv* env, jclass cls, jobject activity, jint width, jint height, jint gen, jboolean mixedReality)
{
   if (g_started) return;
   env->GetJavaVM(&g_vm);
   g_activity = env->NewGlobalRef(activity);
   if (!g_cls)
   {
      g_cls = (jclass)env->NewGlobalRef(cls);
      g_onSurfaces = env->GetStaticMethodID(g_cls, "onSurfaces", "(ILandroid/graphics/SurfaceTexture;)V");
      g_onPointer = env->GetStaticMethodID(g_cls, "onPointer", "(IIZFFZF)V");
      g_onBack = env->GetStaticMethodID(g_cls, "onBack", "(I)V");
      g_onModeChanged = env->GetStaticMethodID(g_cls, "onModeChanged", "(IZ)V");
      g_onFailed = env->GetStaticMethodID(g_cls, "onFailed", "(I)V");
      g_createTexture = env->GetStaticMethodID(g_cls, "createPanelTexture", "(III)Landroid/graphics/SurfaceTexture;");
      g_latchTexture = env->GetStaticMethodID(g_cls, "latchPanelTexture", "(Landroid/graphics/SurfaceTexture;[F)J");
      g_releaseTexture = env->GetStaticMethodID(g_cls, "releasePanelTexture", "(Landroid/graphics/SurfaceTexture;)V");
   }
   g_width = width;
   g_height = height;
   g_gen = gen;
   g_mixedReality = mixedReality == JNI_TRUE;
   g_stop = false;
   g_thread = std::thread(threadMain);
   g_started = true;
}

JNIEXPORT void JNICALL Java_org_vpinball_app_vr_VrShell_nativeStop(JNIEnv* env, jclass)
{
   if (!g_started) return;
   g_stop = true;
   if (g_thread.joinable()) g_thread.join();
   env->DeleteGlobalRef(g_activity);
   g_activity = nullptr;
   g_started = false;
}

}
