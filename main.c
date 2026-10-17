// frame-float: a cube floats in front of you in VR; point at it and pull the
// trigger (or pinch) to change its color. Runs as a normal desktop process
// through SteamVR's OpenXR runtime, so the desktop stays usable.
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define CHECK(call)                                                        \
  do {                                                                     \
    XrResult r_ = (call);                                                  \
    if (XR_FAILED(r_)) {                                                   \
      char s_[XR_MAX_RESULT_STRING_SIZE] = "";                             \
      if (instance) xrResultToString(instance, r_, s_);                    \
      fprintf(stderr, "%s failed: %s (%d)\n", #call, s_, r_);              \
      exit(1);                                                             \
    }                                                                      \
  } while (0)

static XrInstance instance = XR_NULL_HANDLE;
static volatile sig_atomic_t quit_requested = 0;
static void on_sigint(int s) { (void)s; quit_requested = 1; }

// ---------- small math ----------
typedef struct { float m[16]; } mat4;  // column-major
typedef struct { float x, y, z; } vec3;

static vec3 v3(float x, float y, float z) { return (vec3){x, y, z}; }
static vec3 vadd(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 vscale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }

static vec3 qrot(XrQuaternionf q, vec3 v) {
  vec3 u = v3(q.x, q.y, q.z);
  vec3 t = vscale(v3(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                     u.x * v.y - u.y * v.x), 2.0f);
  vec3 c = v3(u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x);
  return vadd(vadd(v, vscale(t, q.w)), c);
}

static mat4 mmul(mat4 a, mat4 b) {
  mat4 r;
  for (int c = 0; c < 4; c++)
    for (int rI = 0; rI < 4; rI++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + rI] * b.m[c * 4 + k];
      r.m[c * 4 + rI] = s;
    }
  return r;
}

// Rotation + translation from an OpenXR pose, then non-uniform scale.
static mat4 from_pose(XrPosef p, vec3 scale) {
  float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
  mat4 r = {{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
             2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
             2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
             p.position.x, p.position.y, p.position.z, 1}};
  for (int i = 0; i < 3; i++) {
    r.m[0 + i] *= scale.x;
    r.m[4 + i] *= scale.y;
    r.m[8 + i] *= scale.z;
  }
  return r;
}

static mat4 view_from_pose(XrPosef p) {
  XrQuaternionf q = p.orientation;
  XrQuaternionf qi = {-q.x, -q.y, -q.z, q.w};
  vec3 t = qrot(qi, v3(-p.position.x, -p.position.y, -p.position.z));
  XrPosef inv = {qi, {t.x, t.y, t.z}};
  return from_pose(inv, v3(1, 1, 1));
}

static mat4 projection(XrFovf f, float n, float fa) {
  float l = tanf(f.angleLeft), r = tanf(f.angleRight);
  float u = tanf(f.angleUp), d = tanf(f.angleDown);
  float w = r - l, h = u - d;
  mat4 m = {{0}};
  m.m[0] = 2 / w;
  m.m[5] = 2 / h;
  m.m[8] = (r + l) / w;
  m.m[9] = (u + d) / h;
  m.m[10] = -(fa + n) / (fa - n);
  m.m[11] = -1;
  m.m[14] = -2 * fa * n / (fa - n);
  return m;
}

static XrQuaternionf yaw_quat(float a) {
  return (XrQuaternionf){0, sinf(a / 2), 0, cosf(a / 2)};
}

// Ray vs cube centered at `c`, rotated by `yaw` around Y, half-size `hs`.
// Returns hit distance or -1.
static float ray_cube(vec3 o, vec3 dir, vec3 c, float yaw, float hs) {
  XrQuaternionf inv = yaw_quat(-yaw);
  vec3 lo = qrot(inv, vsub(o, c)), ld = qrot(inv, dir);
  float tmin = -1e9f, tmax = 1e9f;
  float os[3] = {lo.x, lo.y, lo.z}, ds[3] = {ld.x, ld.y, ld.z};
  for (int i = 0; i < 3; i++) {
    if (fabsf(ds[i]) < 1e-6f) {
      if (os[i] < -hs || os[i] > hs) return -1;
      continue;
    }
    float t1 = (-hs - os[i]) / ds[i], t2 = (hs - os[i]) / ds[i];
    if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
    if (t1 > tmin) tmin = t1;
    if (t2 < tmax) tmax = t2;
  }
  if (tmax < 0 || tmin > tmax) return -1;
  return tmin > 0 ? tmin : 0;
}

// ---------- GL ----------
static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm;\n"
    "uniform mat4 uMVP; uniform mat4 uModel; out vec3 vN;\n"
    "void main(){ vN = mat3(uModel)*aNrm; gl_Position = uMVP*vec4(aPos,1.0); }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec3 vN; uniform vec3 uColor; uniform float uLit; out vec4 o;\n"
    "void main(){ vec3 n = normalize(vN);\n"
    "  float d = max(dot(n, normalize(vec3(0.4,0.9,0.5))), 0.0);\n"
    "  vec3 c = uColor*mix(1.0, 0.35+0.65*d, uLit); o = vec4(c,1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "shader error: %s\n", log);
    exit(1);
  }
  return s;
}

static GLuint prog, vao;
static GLint uMVP, uModel, uColor, uLit;

static void gl_init(void) {
  prog = glCreateProgram();
  glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
  glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
  glLinkProgram(prog);
  uMVP = glGetUniformLocation(prog, "uMVP");
  uModel = glGetUniformLocation(prog, "uModel");
  uColor = glGetUniformLocation(prog, "uColor");
  uLit = glGetUniformLocation(prog, "uLit");

  // Unit cube (-0.5..0.5): 6 faces x 2 triangles, position + normal.
  static const float n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  float verts[36 * 6];
  int k = 0;
  for (int f = 0; f < 6; f++) {
    vec3 N = v3(n[f][0], n[f][1], n[f][2]);
    vec3 U = fabsf(N.y) > 0.5f ? v3(1, 0, 0) : v3(0, 1, 0);
    vec3 V = v3(N.y * U.z - N.z * U.y, N.z * U.x - N.x * U.z, N.x * U.y - N.y * U.x);
    float corners[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    for (int i = 0; i < 6; i++) {
      vec3 p = vadd(vscale(N, 0.5f), vadd(vscale(U, 0.5f * corners[i][0]),
                                          vscale(V, 0.5f * corners[i][1])));
      float *o = &verts[k++ * 6];
      o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = N.x; o[4] = N.y; o[5] = N.z;
    }
  }
  GLuint vbo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
}

static void draw_box(mat4 vp, mat4 model, const float *rgb, float lit) {
  mat4 mvp = mmul(vp, model);
  glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
  glUniformMatrix4fv(uModel, 1, GL_FALSE, model.m);
  glUniform3fv(uColor, 1, rgb);
  glUniform1f(uLit, lit);
  glDrawArrays(GL_TRIANGLES, 0, 36);
}

// ---------- game state ----------
static const float PALETTE[][3] = {
    {0.95f, 0.25f, 0.25f}, {1.00f, 0.60f, 0.15f}, {0.98f, 0.90f, 0.20f},
    {0.30f, 0.85f, 0.35f}, {0.20f, 0.80f, 0.90f}, {0.25f, 0.45f, 0.95f},
    {0.65f, 0.35f, 0.95f}, {0.95f, 0.45f, 0.75f}};
static const char *PALETTE_NAMES[] = {"red", "orange", "yellow", "green",
                                      "cyan", "blue", "purple", "pink"};
#define NCOLORS (int)(sizeof PALETTE / sizeof PALETTE[0])
#define CUBE_SIZE 0.25f

int main(void) {
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);

  // --- EGL + OpenGL context with no window ---
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
  eglBindAPI(EGL_OPENGL_API);
  EGLint cattr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, cattr, &cfg, 1, &ncfg) || ncfg < 1) { fprintf(stderr, "no EGL config\n"); return 1; }
  EGLint ctxattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
    fprintf(stderr, "could not create a GL context\n");
    return 1;
  }
  printf("GL: %s\n", glGetString(GL_RENDERER));

  // --- OpenXR instance ---
  const char *wanted[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME, XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
                          XR_EXT_HAND_INTERACTION_EXTENSION_NAME};
  uint32_t next = 0;
  xrEnumerateInstanceExtensionProperties(NULL, 0, &next, NULL);
  XrExtensionProperties *props = calloc(next, sizeof *props);
  for (uint32_t i = 0; i < next; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
  xrEnumerateInstanceExtensionProperties(NULL, next, &next, props);
  const char *enabled[3];
  uint32_t nenabled = 0;
  bool has_hand_interaction = false;
  for (int w = 0; w < 3; w++)
    for (uint32_t i = 0; i < next; i++)
      if (!strcmp(props[i].extensionName, wanted[w])) {
        enabled[nenabled++] = wanted[w];
        if (w == 2) has_hand_interaction = true;
      }
  free(props);

  XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy(ici.applicationInfo.applicationName, "Frame Float");
  ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ici.enabledExtensionCount = nenabled;
  ici.enabledExtensionNames = enabled;
  CHECK(xrCreateInstance(&ici, &instance));

  XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system;
  CHECK(xrGetSystem(instance, &sgi, &system));

  PFN_xrGetOpenGLGraphicsRequirementsKHR getReq;
  CHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq));
  XrGraphicsRequirementsOpenGLKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
  CHECK(getReq(instance, system, &req));

  XrGraphicsBindingEGLMNDX binding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
  binding.getProcAddress = (PFN_xrEglGetProcAddressMNDX)eglGetProcAddress;
  binding.display = dpy;
  binding.config = cfg;
  binding.context = ctx;
  XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = system;
  XrSession session;
  CHECK(xrCreateSession(instance, &sci, &session));

  XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.poseInReferenceSpace.orientation.w = 1;
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XrSpace local, viewSpace;
  CHECK(xrCreateReferenceSpace(session, &rsci, &local));
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  CHECK(xrCreateReferenceSpace(session, &rsci, &viewSpace));

  // --- input: "select" (trigger / pinch) and "aim" pose per hand ---
  XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy(asci.actionSetName, "game");
  strcpy(asci.localizedActionSetName, "Game");
  XrActionSet actionSet;
  CHECK(xrCreateActionSet(instance, &asci, &actionSet));

  XrPath hands[2];
  CHECK(xrStringToPath(instance, "/user/hand/left", &hands[0]));
  CHECK(xrStringToPath(instance, "/user/hand/right", &hands[1]));

  XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
  aci.countSubactionPaths = 2;
  aci.subactionPaths = hands;
  XrAction selectAction, aimAction;
  aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
  strcpy(aci.actionName, "select");
  strcpy(aci.localizedActionName, "Select");
  CHECK(xrCreateAction(actionSet, &aci, &selectAction));
  aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
  strcpy(aci.actionName, "aim");
  strcpy(aci.localizedActionName, "Aim");
  CHECK(xrCreateAction(actionSet, &aci, &aimAction));

  struct { const char *profile, *select; } profiles[] = {
      {"/interaction_profiles/khr/simple_controller", "select/click"},
      {"/interaction_profiles/valve/index_controller", "trigger/click"},
      {"/interaction_profiles/valve/frame_controller", "trigger/click"},
      {"/interaction_profiles/oculus/touch_controller", "trigger/value"},
      {"/interaction_profiles/ext/hand_interaction_ext", "pinch_ext/value"},
  };
  for (size_t p = 0; p < sizeof profiles / sizeof profiles[0]; p++) {
    if (strstr(profiles[p].profile, "hand_interaction") && !has_hand_interaction) continue;
    XrActionSuggestedBinding b[4];
    const char *side[2] = {"left", "right"};
    for (int h = 0; h < 2; h++) {
      char path[128];
      b[h * 2].action = selectAction;
      snprintf(path, sizeof path, "/user/hand/%s/input/%s", side[h], profiles[p].select);
      xrStringToPath(instance, path, &b[h * 2].binding);
      b[h * 2 + 1].action = aimAction;
      snprintf(path, sizeof path, "/user/hand/%s/input/aim/pose", side[h]);
      xrStringToPath(instance, path, &b[h * 2 + 1].binding);
    }
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    xrStringToPath(instance, profiles[p].profile, &sb.interactionProfile);
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = 4;
    // Profiles this runtime doesn't know are simply skipped.
    xrSuggestInteractionProfileBindings(instance, &sb);
  }
  XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &actionSet;
  CHECK(xrAttachSessionActionSets(session, &attach));

  XrSpace aimSpace[2];
  for (int h = 0; h < 2; h++) {
    XrActionSpaceCreateInfo asc = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asc.action = aimAction;
    asc.subactionPath = hands[h];
    asc.poseInActionSpace.orientation.w = 1;
    CHECK(xrCreateActionSpace(session, &asc, &aimSpace[h]));
  }

  // --- swapchains, one per eye ---
  uint32_t nviews = 0;
  CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nviews, NULL));
  if (nviews > 2) nviews = 2;
  XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nviews, &nviews, vcv));

  uint32_t nfmt = 0;
  CHECK(xrEnumerateSwapchainFormats(session, 0, &nfmt, NULL));
  int64_t *fmts = calloc(nfmt, sizeof *fmts);
  CHECK(xrEnumerateSwapchainFormats(session, nfmt, &nfmt, fmts));
  int64_t format = fmts[0];
  for (uint32_t i = 0; i < nfmt; i++)
    if (fmts[i] == GL_SRGB8_ALPHA8) { format = fmts[i]; break; }
  free(fmts);

  gl_init();
  if (format == GL_SRGB8_ALPHA8) glEnable(GL_FRAMEBUFFER_SRGB);

  XrSwapchain swap[2];
  uint32_t nimg[2];
  XrSwapchainImageOpenGLKHR *imgs[2];
  GLuint *fbos[2], depth[2];
  for (uint32_t v = 0; v < nviews; v++) {
    XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swci.format = format;
    swci.sampleCount = 1;
    swci.width = vcv[v].recommendedImageRectWidth;
    swci.height = vcv[v].recommendedImageRectHeight;
    swci.faceCount = swci.arraySize = swci.mipCount = 1;
    CHECK(xrCreateSwapchain(session, &swci, &swap[v]));
    CHECK(xrEnumerateSwapchainImages(swap[v], 0, &nimg[v], NULL));
    imgs[v] = calloc(nimg[v], sizeof **imgs);
    for (uint32_t i = 0; i < nimg[v]; i++) imgs[v][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    CHECK(xrEnumerateSwapchainImages(swap[v], nimg[v], &nimg[v], (XrSwapchainImageBaseHeader *)imgs[v]));

    glGenRenderbuffers(1, &depth[v]);
    glBindRenderbuffer(GL_RENDERBUFFER, depth[v]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, swci.width, swci.height);
    fbos[v] = calloc(nimg[v], sizeof(GLuint));
    glGenFramebuffers(nimg[v], fbos[v]);
    for (uint32_t i = 0; i < nimg[v]; i++) {
      glBindFramebuffer(GL_FRAMEBUFFER, fbos[v][i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imgs[v][i].image, 0);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth[v]);
    }
  }
  printf("Ready. Put on the headset: point at the cube and pull the trigger (or pinch). Ctrl+C here to quit.\n");
  fflush(stdout);

  // --- main loop ---
  bool running = false, placed = false, prev_select[2] = {false, false};
  int color = 0;
  vec3 cube_pos = v3(0, 0, -0.7f);
  float yaw = 0, pulse = 0;
  XrTime last_time = 0;

  while (true) {
    if (quit_requested && running) { xrRequestExitSession(session); quit_requested = 0; }

    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    bool exit_loop = false;
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
      if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        XrSessionState st = ((XrEventDataSessionStateChanged *)&ev)->state;
        if (st == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO};
          sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          CHECK(xrBeginSession(session, &sbi));
          running = true;
        } else if (st == XR_SESSION_STATE_STOPPING) {
          xrEndSession(session);
          running = false;
        } else if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) {
          exit_loop = true;
        }
      } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
        exit_loop = true;
      }
      ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (exit_loop) break;
    if (!running) {
      if (quit_requested) break;
      struct timespec ts = {0, 50 * 1000 * 1000};
      nanosleep(&ts, NULL);
      continue;
    }

    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    CHECK(xrWaitFrame(session, NULL, &fs));
    CHECK(xrBeginFrame(session, NULL));
    float dt = last_time ? (fs.predictedDisplayTime - last_time) * 1e-9f : 0;
    last_time = fs.predictedDisplayTime;

    XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader *layers[1] = {(XrCompositionLayerBaseHeader *)&layer};
    uint32_t nlayers = 0;

    if (fs.shouldRender) {
      // Place the cube in front of the head the first time we know where it is.
      XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
      xrLocateSpace(viewSpace, local, fs.predictedDisplayTime, &head);
      bool head_ok = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                     (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
      if (!placed && head_ok) {
        vec3 fwd = qrot(head.pose.orientation, v3(0, 0, -1));
        fwd.y = 0;
        float len = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
        fwd = len > 1e-3f ? vscale(fwd, 1 / len) : v3(0, 0, -1);
        vec3 hp = v3(head.pose.position.x, head.pose.position.y, head.pose.position.z);
        cube_pos = vadd(hp, vadd(vscale(fwd, 0.7f), v3(0, -0.1f, 0)));
        placed = true;
      }

      XrActiveActionSet aas = {actionSet, XR_NULL_PATH};
      XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
      sync.countActiveActionSets = 1;
      sync.activeActionSets = &aas;
      xrSyncActions(session, &sync);

      yaw += dt * 0.5f;
      pulse = pulse > 0 ? pulse - dt * 4 : 0;
      XrPosef aim[2];
      bool aim_ok[2];
      float hit[2];
      for (int h = 0; h < 2; h++) {
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(aimSpace[h], local, fs.predictedDisplayTime, &loc);
        aim_ok[h] = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        aim[h] = loc.pose;
        hit[h] = -1;
        if (aim_ok[h]) {
          vec3 o = v3(aim[h].position.x, aim[h].position.y, aim[h].position.z);
          vec3 d = qrot(aim[h].orientation, v3(0, 0, -1));
          hit[h] = ray_cube(o, d, cube_pos, yaw, CUBE_SIZE / 2);
        }
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = selectAction;
        gi.subactionPath = hands[h];
        XrActionStateBoolean bs = {XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(session, &gi, &bs);
        bool down = bs.isActive && bs.currentState;
        if (down && !prev_select[h] && hit[h] >= 0) {
          color = (color + 1) % NCOLORS;
          pulse = 1;
          printf("Cube is now %s\n", PALETTE_NAMES[color]);
          fflush(stdout);
        }
        prev_select[h] = down;
      }

      uint32_t nv = 0;
      XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
      XrViewState vs = {XR_TYPE_VIEW_STATE};
      XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
      vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      vli.displayTime = fs.predictedDisplayTime;
      vli.space = local;
      CHECK(xrLocateViews(session, &vli, &vs, nviews, &nv, views));

      glEnable(GL_DEPTH_TEST);
      glUseProgram(prog);
      glBindVertexArray(vao);
      for (uint32_t v = 0; v < nv; v++) {
        uint32_t idx;
        CHECK(xrAcquireSwapchainImage(swap[v], NULL, &idx));
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        CHECK(xrWaitSwapchainImage(swap[v], &wi));

        int w = vcv[v].recommendedImageRectWidth, hgt = vcv[v].recommendedImageRectHeight;
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[v][idx]);
        glViewport(0, 0, w, hgt);
        glClearColor(0.05f, 0.06f, 0.10f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        mat4 vp = mmul(projection(views[v].fov, 0.05f, 100), view_from_pose(views[v].pose));

        // Floor grid so the space has some depth cues.
        static const float grid[3] = {0.18f, 0.20f, 0.26f};
        for (int i = -10; i <= 10; i++) {
          XrPosef px = {{0, 0, 0, 1}, {i * 0.5f, -1.6f, 0}};
          draw_box(vp, from_pose(px, v3(0.01f, 0.002f, 10)), grid, 0);
          XrPosef pz = {{0, 0, 0, 1}, {0, -1.6f, i * 0.5f}};
          draw_box(vp, from_pose(pz, v3(10, 0.002f, 0.01f)), grid, 0);
        }

        bool aimed = hit[0] >= 0 || hit[1] >= 0;
        float s = CUBE_SIZE * (1 + 0.15f * pulse + (aimed ? 0.05f : 0));
        XrPosef cp = {yaw_quat(yaw), {cube_pos.x, cube_pos.y, cube_pos.z}};
        draw_box(vp, from_pose(cp, v3(s, s, s)), PALETTE[color], 1);

        // Pointer rays from each hand.
        for (int h = 0; h < 2; h++) {
          if (!aim_ok[h]) continue;
          float len = hit[h] >= 0 ? hit[h] : 2.0f;
          vec3 o = v3(aim[h].position.x, aim[h].position.y, aim[h].position.z);
          vec3 mid = vadd(o, qrot(aim[h].orientation, v3(0, 0, -len / 2)));
          XrPosef rp = {aim[h].orientation, {mid.x, mid.y, mid.z}};
          static const float ray_idle[3] = {0.8f, 0.8f, 0.9f}, ray_hit[3] = {1, 1, 1};
          draw_box(vp, from_pose(rp, v3(0.004f, 0.004f, len)), hit[h] >= 0 ? ray_hit : ray_idle, 0);
        }

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CHECK(xrReleaseSwapchainImage(swap[v], &ri));

        pviews[v].pose = views[v].pose;
        pviews[v].fov = views[v].fov;
        pviews[v].subImage.swapchain = swap[v];
        pviews[v].subImage.imageRect.extent = (XrExtent2Di){w, hgt};
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      layer.space = local;
      layer.viewCount = nv;
      layer.views = pviews;
      nlayers = 1;
    }

    XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = nlayers;
    fei.layers = layers;
    CHECK(xrEndFrame(session, &fei));
  }

  printf("Bye!\n");
  xrDestroySession(session);
  xrDestroyInstance(instance);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);
  return 0;
}
