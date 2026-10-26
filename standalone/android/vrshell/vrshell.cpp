// VPX VR shell: shows the Android menu UI on a curved screen in an empty (black) VR space.
//
// The menu is rendered by Android into a Surface that belongs to an OpenXR "Android surface
// swapchain" and is composited as a cylinder layer. No projection layer is submitted, so
// everything around the screen stays black. The controller aim ray is intersected with the
// cylinder and reported to Java, which injects touch / hover / scroll events into the UI.

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

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

namespace {

// Screen geometry: a cylinder around the viewer, slightly below eye level.
constexpr float kRadius = 2.0f;          // metres from the viewer
constexpr float kCentralAngle = 1.40f;   // ~80 degrees wide
constexpr float kCenterY = -0.10f;       // metres relative to the head start position
constexpr float kCursorSize = 0.035f;    // metres
constexpr int kCursorPx = 128;

JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;
jclass g_cls = nullptr;
jmethodID g_onSurfaces = nullptr;
jmethodID g_onPointer = nullptr;
jmethodID g_onBack = nullptr;
jmethodID g_onModeChanged = nullptr;
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
   XrSwapchain cursor = XR_NULL_HANDLE;

   XrActionSet actionSet = XR_NULL_HANDLE;
   XrAction aim = XR_NULL_HANDLE, select = XR_NULL_HANDLE, back = XR_NULL_HANDLE, scroll = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE, toggleMode = XR_NULL_HANDLE;
   XrPath hand[2] {};
   XrSpace aimSpace[2] {XR_NULL_HANDLE, XR_NULL_HANDLE};

   bool hasFlip = false; // XR_FB_composition_layer_image_layout: Android surfaces arrive upside down
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

   bool hasExtension(const char* name)
   {
      uint32_t n = 0;
      xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
      XrExtensionProperties props[256];
      if (n > 256) n = 256;
      for (uint32_t i = 0; i < n; i++) props[i] = {XR_TYPE_EXTENSION_PROPERTIES};
      xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props);
      for (uint32_t i = 0; i < n; i++)
         if (strcmp(props[i].extensionName, name) == 0) return true;
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

      const char* exts[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
         XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME, XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME};
      for (const char* e : exts)
         if (!hasExtension(e)) { LOGE("Missing OpenXR extension %s", e); return false; }

      const char* enabled[8];
      uint32_t enabledCount = 0;
      for (const char* e : exts) enabled[enabledCount++] = e;
      hasFlip = hasExtension(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
      if (hasFlip) enabled[enabledCount++] = XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME;
      else LOGE("XR_FB_composition_layer_image_layout missing, menu will be upside down");
      hasPassthrough = hasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME);
      if (hasPassthrough) enabled[enabledCount++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
      else LOGE("XR_FB_passthrough missing, mixed reality is not available");

      XrInstanceCreateInfoAndroidKHR android {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
      android.applicationVM = g_vm;
      android.applicationActivity = g_activity;

      XrInstanceCreateInfo ci {XR_TYPE_INSTANCE_CREATE_INFO};
      ci.next = &android;
      strncpy(ci.applicationInfo.applicationName, "Visual Pinball Menu", XR_MAX_APPLICATION_NAME_SIZE - 1);
      strncpy(ci.applicationInfo.engineName, "VPX", XR_MAX_ENGINE_NAME_SIZE - 1);
      ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
      ci.enabledExtensionCount = enabledCount;
      ci.enabledExtensionNames = enabled;
      XR_OK(xrCreateInstance(&ci, &instance));

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

      {
         const XrActionSuggestedBinding b[] = {
            {aim, path("/user/hand/left/input/aim/pose")}, {aim, path("/user/hand/right/input/aim/pose")},
            {select, path("/user/hand/left/input/trigger/value")}, {select, path("/user/hand/right/input/trigger/value")},
            {back, path("/user/hand/left/input/y/click")}, {back, path("/user/hand/right/input/b/click")},
            {scroll, path("/user/hand/left/input/thumbstick")}, {scroll, path("/user/hand/right/input/thumbstick")},
            {haptic, path("/user/hand/left/output/haptic")}, {haptic, path("/user/hand/right/output/haptic")},
            {toggleMode, path("/user/hand/left/input/thumbstick/click")},
         };
         XrInteractionProfileSuggestedBinding s {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
         s.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
         s.countSuggestedBindings = sizeof(b) / sizeof(b[0]);
         s.suggestedBindings = b;
         XR_OK(xrSuggestInteractionProfileBindings(instance, &s));
      }
      {
         const XrActionSuggestedBinding b[] = {
            {aim, path("/user/hand/left/input/aim/pose")}, {aim, path("/user/hand/right/input/aim/pose")},
            {select, path("/user/hand/left/input/select/click")}, {select, path("/user/hand/right/input/select/click")},
            {back, path("/user/hand/left/input/menu/click")}, {back, path("/user/hand/right/input/menu/click")},
            {haptic, path("/user/hand/left/output/haptic")}, {haptic, path("/user/hand/right/output/haptic")},
         };
         XrInteractionProfileSuggestedBinding s {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
         s.interactionProfile = path("/interaction_profiles/khr/simple_controller");
         s.countSuggestedBindings = sizeof(b) / sizeof(b[0]);
         s.suggestedBindings = b;
         XR_OK(xrSuggestInteractionProfileBindings(instance, &s));
      }

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

   bool initSwapchains()
   {
      PFN_xrCreateSwapchainAndroidSurfaceKHR createSurfaceSwapchain = nullptr;
      XR_OK(xrGetInstanceProcAddr(instance, "xrCreateSwapchainAndroidSurfaceKHR", (PFN_xrVoidFunction*)&createSurfaceSwapchain));

      XrSwapchainCreateInfo sc {XR_TYPE_SWAPCHAIN_CREATE_INFO};
      sc.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
      // Android surface swapchains: the runtime owns the buffer format, these must be zero.
      sc.format = 0;
      sc.sampleCount = 0;
      sc.faceCount = 0;
      sc.arraySize = 0;
      sc.mipCount = 0;

      jobject panelSurface = nullptr, cursorSurface = nullptr;
      sc.width = g_width;
      sc.height = g_height;
      XR_OK(createSurfaceSwapchain(session, &sc, &panel, &panelSurface));
      sc.width = kCursorPx;
      sc.height = kCursorPx;
      XR_OK(createSurfaceSwapchain(session, &sc, &cursor, &cursorSurface));

      env->CallStaticVoidMethod(g_cls, g_onSurfaces, g_gen, panelSurface, cursorSurface);
      return true;
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

   // Returns true and fills the cursor pose when the active controller points at the screen.
   bool updateInput(XrTime time, XrPosef& cursorPose)
   {
      if (state != XR_SESSION_STATE_FOCUSED)
      {
         env->CallStaticVoidMethod(g_cls, g_onPointer, g_gen, activeHand, JNI_FALSE, 0.f, 0.f, JNI_FALSE, 0.f);
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
            env->CallStaticVoidMethod(g_cls, g_onBack, g_gen);
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
         }
      }

      float scrollY = 0.f;
      {
         XrActionStateGetInfo gi {XR_TYPE_ACTION_STATE_GET_INFO};
         gi.action = scroll;
         gi.subactionPath = hand[activeHand];
         XrActionStateVector2f v {XR_TYPE_ACTION_STATE_VECTOR2F};
         xrGetActionStateVector2f(session, &gi, &v);
         if (v.isActive && std::fabs(v.currentState.y) > 0.2f) scrollY = v.currentState.y;
      }

      bool hit = false;
      float u = 0.f, v = 0.f;
      XrSpaceLocation loc {XR_TYPE_SPACE_LOCATION};
      if (XR_SUCCEEDED(xrLocateSpace(aimSpace[activeHand], local, time, &loc))
         && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
      {
         const Vec3 o {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
         const Vec3 d = rotate(loc.pose.orientation, {0.f, 0.f, -1.f});
         // Intersect with the vertical cylinder around the origin (we are inside it).
         const float a = d.x * d.x + d.z * d.z;
         const float b = 2.f * (o.x * d.x + o.z * d.z);
         const float c = o.x * o.x + o.z * o.z - kRadius * kRadius;
         const float disc = b * b - 4.f * a * c;
         if (a > 1e-6f && disc >= 0.f)
         {
            const float t = (-b + std::sqrt(disc)) / (2.f * a);
            if (t > 0.f)
            {
               const Vec3 p {o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
               const float theta = std::atan2(p.x, -p.z);
               const float aspect = (float)g_width / (float)g_height;
               const float height = kRadius * kCentralAngle / aspect;
               u = 0.5f + theta / kCentralAngle;
               v = 0.5f - (p.y - kCenterY) / height;
               if (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f)
               {
                  hit = true;
                  const float k = (kRadius - 0.01f) / kRadius;
                  cursorPose.position = {p.x * k, p.y, p.z * k};
                  const float yaw = -theta;
                  cursorPose.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
               }
            }
         }
      }

      env->CallStaticVoidMethod(g_cls, g_onPointer, g_gen, activeHand, hit ? JNI_TRUE : JNI_FALSE, u, v,
         pressed[activeHand] ? JNI_TRUE : JNI_FALSE, scrollY);
      return hit;
   }

   void frame()
   {
      XrFrameWaitInfo wi {XR_TYPE_FRAME_WAIT_INFO};
      XrFrameState fs {XR_TYPE_FRAME_STATE};
      if (XR_FAILED(xrWaitFrame(session, &wi, &fs))) return;
      XrFrameBeginInfo bi {XR_TYPE_FRAME_BEGIN_INFO};
      if (XR_FAILED(xrBeginFrame(session, &bi))) return;

      XrPosef cursorPose {};
      const bool showCursor = updateInput(fs.predictedDisplayTime, cursorPose);

      XrCompositionLayerImageLayoutFB flip {XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
      flip.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;

      XrCompositionLayerCylinderKHR cyl {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
      if (hasFlip) cyl.next = &flip;
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
      quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      quad.space = local;
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      quad.subImage.swapchain = cursor;
      quad.subImage.imageRect = {{0, 0}, {kCursorPx, kCursorPx}};
      quad.pose = cursorPose;
      quad.size = {kCursorSize, kCursorSize};

      XrCompositionLayerPassthroughFB pt {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
      pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      pt.space = XR_NULL_HANDLE;
      pt.layerHandle = passthroughLayer;

      const XrCompositionLayerBaseHeader* layers[3];
      uint32_t count = 0;
      if (passthroughRunning) layers[count++] = (const XrCompositionLayerBaseHeader*)&pt;
      layers[count++] = (const XrCompositionLayerBaseHeader*)&cyl;
      if (showCursor) layers[count++] = (const XrCompositionLayerBaseHeader*)&quad;

      XrFrameEndInfo ei {XR_TYPE_FRAME_END_INFO};
      ei.displayTime = fs.predictedDisplayTime;
      ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
      ei.layerCount = fs.shouldRender ? count : 0;
      ei.layers = layers;
      xrEndFrame(session, &ei);
   }

   void run()
   {
      if (!initEGL() || !initInstance() || !initSession() || !initActions() || !initPassthrough() || !initSwapchains())
      {
         LOGE("VR shell failed to start");
         return;
      }
      LOGI("VR shell started (%dx%d)", g_width, g_height);
      while (!g_stop.load() && !exitRequested)
      {
         handleEvents();
         if (!running) { usleep(10000); continue; }
         frame();
      }
   }

   void shutdown()
   {
      if (passthroughLayer != XR_NULL_HANDLE) pfnDestroyPassthroughLayer(passthroughLayer);
      if (passthrough != XR_NULL_HANDLE) pfnDestroyPassthrough(passthrough);
      for (XrSpace& s : aimSpace) if (s != XR_NULL_HANDLE) { xrDestroySpace(s); s = XR_NULL_HANDLE; }
      if (cursor != XR_NULL_HANDLE) xrDestroySwapchain(cursor);
      if (panel != XR_NULL_HANDLE) xrDestroySwapchain(panel);
      if (local != XR_NULL_HANDLE) xrDestroySpace(local);
      if (actionSet != XR_NULL_HANDLE) xrDestroyActionSet(actionSet);
      if (session != XR_NULL_HANDLE) xrDestroySession(session);
      if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
      if (dpy != EGL_NO_DISPLAY)
      {
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
      g_onSurfaces = env->GetStaticMethodID(g_cls, "onSurfaces", "(ILandroid/view/Surface;Landroid/view/Surface;)V");
      g_onPointer = env->GetStaticMethodID(g_cls, "onPointer", "(IIZFFZF)V");
      g_onBack = env->GetStaticMethodID(g_cls, "onBack", "(I)V");
      g_onModeChanged = env->GetStaticMethodID(g_cls, "onModeChanged", "(IZ)V");
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
