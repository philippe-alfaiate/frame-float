// vr-cube-overlay: the cube as a SteamVR overlay instead of a VR scene app.
// Overlays float over whatever is running (desktop mode, passthrough), so
// only the cube appears. The background is transparent and each eye gets its
// own rendering (side-by-side stereo), so the cube looks 3D. Point the
// SteamVR laser at it: a quick click changes its color, holding the click grabs
// it so it follows your hand, and the stick (sent as scrolling) pushes/pulls it. Outside desktop mode L1/R1 change color too, and
// L2/R2 grab with the stick pushing/pulling (desktop mode keeps those buttons).
#include <math.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <unistd.h>

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "model.h"
#include "third_party/openvr/openvr.h"

static volatile sig_atomic_t quit_requested = 0;
static void on_signal(int) { quit_requested = 1; }

// Overlay layout: a square per eye, side by side in one texture.
static const int EYE_PX = 512;
static const float OVERLAY_WIDTH_M = 0.50f;  // physical width of the overlay (the "window")
static const float VIEW_DIST_M = 0.80f;      // how far in front of you it appears
static const float HALF_IPD_M = 0.032f;
static const float MODEL_RADIUS_M = 0.15f;  // every model is scaled to fit this sphere
// Closest the object comes to the controller: its surface touching it.
static const float CLOSEST_M = MODEL_RADIUS_M + 0.02f;
// Keep pulling an object that's already touching the controller this long and
// it closes; a ring around it fills up meanwhile.
static const float CLOSE_HOLD_S = 1.5f;

static const float PALETTE[][3] = {
    {0.95f, 0.25f, 0.25f}, {1.00f, 0.60f, 0.15f}, {0.98f, 0.90f, 0.20f},
    {0.30f, 0.85f, 0.35f}, {0.20f, 0.80f, 0.90f}, {0.25f, 0.45f, 0.95f},
    {0.65f, 0.35f, 0.95f}, {0.95f, 0.45f, 0.75f}};
static const char *PALETTE_NAMES[] = {"red", "orange", "yellow", "green",
                                      "cyan", "blue", "purple", "pink"};
static const int NCOLORS = sizeof PALETTE / sizeof PALETTE[0];

// ---------- small math (column-major 4x4) ----------
struct mat4 { float m[16]; };

static mat4 mmul(const mat4 &a, const mat4 &b) {
  mat4 r;
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = s;
    }
  return r;
}

static mat4 translate(float x, float y, float z) {
  return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}};
}

static mat4 rot_y(float a) {
  float c = cosf(a), s = sinf(a);
  return {{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1}};
}

static mat4 scale(float s) {
  return {{s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1}};
}

static mat4 frustum(float l, float r, float b, float t, float n, float f) {
  mat4 m = {{0}};
  m.m[0] = 2 * n / (r - l);
  m.m[5] = 2 * n / (t - b);
  m.m[8] = (r + l) / (r - l);
  m.m[9] = (t + b) / (t - b);
  m.m[10] = -(f + n) / (f - n);
  m.m[11] = -1;
  m.m[14] = -2 * f * n / (f - n);
  return m;
}

// Treat the overlay as a window: the overlay is a square of OVERLAY_WIDTH_M
// centered at the origin in the z=0 plane (overlay space, +Z toward you), and
// the eye sits at (ex, ey, ez). Drawing each eye from its real position keeps
// the 3D effect right as you move. With flip_y the rows come out top-first,
// as SetOverlayRaw expects; GL textures are bottom-up.
static mat4 window_view_proj(float ex, float ey, float ez, bool flip_y) {
  const float n = 0.02f, k = n / ez, hw = OVERLAY_WIDTH_M / 2;
  mat4 p = frustum((-hw - ex) * k, (hw - ex) * k, (-hw - ey) * k, (hw - ey) * k, n, 20);
  if (flip_y)
    for (int c = 0; c < 4; c++) p.m[c * 4 + 1] = -p.m[c * 4 + 1];
  return mmul(p, translate(-ex, -ey, -ez));
}

// ---------- GL ----------
static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm;\n"
    "layout(location=2) in vec3 aCol; layout(location=3) in vec2 aUV;\n"
    "uniform mat4 uMVP; uniform mat4 uModel; out vec3 vN; out vec3 vCol; out vec2 vUV;\n"
    "void main(){ vN = mat3(uModel)*aNrm; vCol = aCol; vUV = aUV;\n"
    "  gl_Position = uMVP*vec4(aPos,1.0); }\n";
// Two-sided lighting: imported meshes don't always wind their triangles the
// same way.
static const char *FS =
    "#version 330 core\n"
    "in vec3 vN; in vec3 vCol; in vec2 vUV; out vec4 o;\n"
    "uniform vec3 uTint; uniform sampler2D uTex; uniform float uUseTex; uniform float uUnlit;\n"
    "void main(){ vec3 n = normalize(vN);\n"
    "  float d = abs(dot(n, normalize(vec3(0.4,0.9,0.6))));\n"
    "  vec3 base = vCol * mix(vec3(1.0), texture(uTex, vUV).rgb, uUseTex);\n"
    "  o = vec4(base*uTint*mix(0.35+0.65*d, 1.0, uUnlit), 1.0); }\n";

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

// Attribute layout of Mesh::verts, for the currently bound VAO/VBO.
static void set_vertex_layout() {
  const GLsizei stride = Mesh::FLOATS_PER_VERTEX * sizeof(float);
  const int sizes[4] = {3, 3, 3, 2};
  for (int a = 0, offset = 0; a < 4; offset += sizes[a], a++) {
    glVertexAttribPointer(a, sizes[a], GL_FLOAT, GL_FALSE, stride, (void *)(offset * sizeof(float)));
    glEnableVertexAttribArray(a);
  }
}

// The "pull in to close" ring, in overlay space (facing you) around the
// object: a dim full track plus an arc filled clockwise from the top.
static void build_close_ring(float progress, std::vector<float> &out) {
  out.clear();
  const int SEG = 96;
  const float R_IN = MODEL_RADIUS_M * 1.10f, R_OUT = MODEL_RADIUS_M * 1.20f;
  auto quad_strip = [&](float from, float to, float r, float g, float b, float z) {
    int n = std::max(1, (int)(SEG * (to - from)));
    for (int i = 0; i < n; i++) {
      float a0 = 1.5707963f - 6.2831853f * (from + (to - from) * i / n);
      float a1 = 1.5707963f - 6.2831853f * (from + (to - from) * (i + 1) / n);
      float pts[4][2] = {{cosf(a0) * R_IN, sinf(a0) * R_IN}, {cosf(a0) * R_OUT, sinf(a0) * R_OUT},
                         {cosf(a1) * R_OUT, sinf(a1) * R_OUT}, {cosf(a1) * R_IN, sinf(a1) * R_IN}};
      const int order[6] = {0, 1, 2, 0, 2, 3};
      for (int k : order) {
        float v[Mesh::FLOATS_PER_VERTEX] = {pts[k][0], pts[k][1], z, 0, 0, 1, r, g, b, 0, 0};
        out.insert(out.end(), v, v + Mesh::FLOATS_PER_VERTEX);
      }
    }
  };
  quad_strip(0, 1, 0.30f, 0.30f, 0.32f, 0);  // track
  float t = fminf(1, progress);
  quad_strip(0, t, 1.0f, 1.0f - 0.7f * t, 1.0f - 0.7f * t, 0.001f);  // white -> red as it fills
}

// Upload a mesh (and its textures) to the GPU.
static GLuint make_mesh_vao(const Mesh &mesh, std::vector<GLuint> &textures) {
  GLuint vao, vbo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, mesh.verts.size() * sizeof(float), mesh.verts.data(), GL_STATIC_DRAW);
  set_vertex_layout();
  for (const Mesh::Texture &t : mesh.textures) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    textures.push_back(tex);
  }
  return vao;
}

static bool init_gl_context() {
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) return false;
  eglBindAPI(EGL_OPENGL_API);
  EGLint cattr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
  EGLConfig cfg;
  EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, cattr, &cfg, 1, &ncfg) || ncfg < 1) return false;
  EGLint ctxattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
  return ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
}

// Place the overlay VIEW_DIST_M in front of the headset, facing it.
struct vec3 { float x, y, z; };
static vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float length(vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static vec3 position_of(const vr::HmdMatrix34_t &m) { return {m.m[0][3], m.m[1][3], m.m[2][3]}; }
// Rotate a vector by the pose's rotation (to world) or its inverse (to local).
static vec3 rotate(const vr::HmdMatrix34_t &m, vec3 v) {
  return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
          m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
          m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}
static vec3 unrotate(const vr::HmdMatrix34_t &m, vec3 v) {
  return {m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z,
          m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
          m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z};
}

// Seconds until the frame we are about to draw shows up on the display.
static float predicted_seconds() {
  float since_vsync = 0;
  vr::VRSystem()->GetTimeSinceLastVsync(&since_vsync, NULL);
  float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(0, vr::Prop_DisplayFrequency_Float);
  float to_photons = vr::VRSystem()->GetFloatTrackedDeviceProperty(0, vr::Prop_SecondsFromVsyncToPhotons_Float);
  float frame = hz > 1 ? 1 / hz : 0.011f;
  return fmaxf(0, frame - since_vsync + to_photons);
}

static bool head_pose(vr::HmdMatrix34_t *out) {
  vr::TrackedDevicePose_t poses[1];
  vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, predicted_seconds(), poses, 1);
  if (!poses[0].bPoseIsValid) return false;
  *out = poses[0].mDeviceToAbsoluteTracking;
  return true;
}

// Spot VIEW_DIST_M in front of the headset, slightly below eye level.
static vec3 in_front_of(const vr::HmdMatrix34_t &h) {
  float fx = -h.m[0][2], fz = -h.m[2][2];  // headset forward, flattened
  float len = sqrtf(fx * fx + fz * fz);
  if (len < 1e-3f) { fx = 0; fz = -1; } else { fx /= len; fz /= len; }
  return position_of(h) + vec3{fx * VIEW_DIST_M, -0.10f, fz * VIEW_DIST_M};
}

// Put the overlay at `pos`, turned (around the vertical axis) to face the head.
static vr::HmdMatrix34_t set_overlay_pose(vr::VROverlayHandle_t overlay, vec3 pos, const vr::HmdMatrix34_t &h) {
  vec3 to_head = position_of(h) - pos;
  float yaw = atan2f(to_head.x, to_head.z);  // overlay +Z faces the viewer
  float c = cosf(yaw), s = sinf(yaw);
  vr::HmdMatrix34_t t = {{{c, 0, s, pos.x}, {0, 1, 0, pos.y}, {-s, 0, c, pos.z}}};
  vr::VROverlay()->SetOverlayTransformAbsolute(overlay, vr::TrackingUniverseStanding, &t);
  return t;
}

// Directory of this executable, so we find actions.json wherever we run from.
static void exe_dir(char *out, size_t size) {
  ssize_t n = readlink("/proc/self/exe", out, size - 1);
  out[n > 0 ? n : 0] = 0;
  if (char *slash = strrchr(out, '/')) *slash = 0;
}

int main(int argc, char **argv) {
  if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
    fprintf(stderr, "usage: %s [model.stl|.obj|.ply|.gltf|.glb]\n", argv[0]);
    return 2;
  }
  // The model to show: a file from the command line, or the cube.
  Mesh mesh;
  if (argc == 2) {
    std::string load_err;
    if (!load_model(argv[1], mesh, load_err)) {
      fprintf(stderr, "could not load %s: %s\n", argv[1], load_err.c_str());
      return 1;
    }
    printf("Loaded %s: %zu triangles\n", argv[1], mesh.vertex_count() / 3);
  } else {
    mesh = make_cube_mesh();
  }
  fit_mesh(mesh, MODEL_RADIUS_M);
  const char *object = argc == 2 ? "Model" : "Cube";

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  vr::EVRInitError err = vr::VRInitError_None;
  vr::VR_Init(&err, vr::VRApplication_Overlay);
  if (err != vr::VRInitError_None) {
    fprintf(stderr, "SteamVR init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
    return 1;
  }

  // Controller input: bumpers change color, triggers grab, stick pushes/pulls.
  char manifest[4096];
  exe_dir(manifest, sizeof manifest - 16);
  strcat(manifest, "/actions.json");
  vr::EVRInputError ierr = vr::VRInput()->SetActionManifestPath(manifest);
  if (ierr != vr::VRInputError_None) fprintf(stderr, "SetActionManifestPath(%s) failed: %d\n", manifest, ierr);
  vr::VRActionSetHandle_t action_set;
  vr::VRActionHandle_t act_color, act_grab, act_stick, act_hand;
  vr::VRInputValueHandle_t hand_src[2];
  vr::VRInput()->GetActionSetHandle("/actions/cube", &action_set);
  vr::VRInput()->GetActionHandle("/actions/cube/in/color", &act_color);
  vr::VRInput()->GetActionHandle("/actions/cube/in/grab", &act_grab);
  vr::VRInput()->GetActionHandle("/actions/cube/in/stick", &act_stick);
  vr::VRInput()->GetActionHandle("/actions/cube/in/hand", &act_hand);
  vr::VRInput()->GetInputSourceHandle("/user/hand/left", &hand_src[0]);
  vr::VRInput()->GetInputSourceHandle("/user/hand/right", &hand_src[1]);

  if (!init_gl_context()) {
    fprintf(stderr, "could not create a GL context\n");
    return 1;
  }

  GLuint prog = glCreateProgram();
  glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
  glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
  glLinkProgram(prog);
  GLint uMVP = glGetUniformLocation(prog, "uMVP");
  GLint uModel = glGetUniformLocation(prog, "uModel");
  GLint uTint = glGetUniformLocation(prog, "uTint");
  GLint uUseTex = glGetUniformLocation(prog, "uUseTex");
  GLint uUnlit = glGetUniformLocation(prog, "uUnlit");
  glUseProgram(prog);
  glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
  std::vector<GLuint> mesh_textures;
  GLuint vao = make_mesh_vao(mesh, mesh_textures);
  GLuint ring_vao, ring_vbo;
  glGenVertexArrays(1, &ring_vao);
  glBindVertexArray(ring_vao);
  glGenBuffers(1, &ring_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, ring_vbo);
  set_vertex_layout();
  std::vector<float> ring_verts;
  mesh.verts = std::vector<float>();  // on the GPU now
  for (Mesh::Texture &t : mesh.textures) t.rgba = std::vector<unsigned char>();

  const int W = EYE_PX * 2, H = EYE_PX;
  // Two color textures used in turn, so we never draw into the one SteamVR
  // is currently showing. VRCUBE_RAW=1 uses SetOverlayRaw uploads instead.
  bool use_raw = getenv("VRCUBE_RAW") != NULL;
  GLuint fbo[2], color_tex[2], depth_rb;
  glGenRenderbuffers(1, &depth_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
  glGenTextures(2, color_tex);
  glGenFramebuffers(2, fbo);
  for (int i = 0; i < 2; i++) {
    glBindTexture(GL_TEXTURE_2D, color_tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex[i], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
  }
  int cur = 0;  // index of the most recently finished frame
  unsigned char *pixels = (unsigned char *)malloc((size_t)W * H * 4);

  vr::VROverlayHandle_t overlay;
  // A key per process, so several models can be open at once.
  char overlay_key[64], overlay_name[256];
  snprintf(overlay_key, sizeof overlay_key, "vrcube.overlay.%d", (int)getpid());
  const char *base = argc == 2 ? strrchr(argv[1], '/') : NULL;
  snprintf(overlay_name, sizeof overlay_name, "VR Cube: %s", argc == 2 ? (base ? base + 1 : argv[1]) : "cube");
  vr::EVROverlayError oerr = vr::VROverlay()->CreateOverlay(overlay_key, overlay_name, &overlay);
  if (oerr != vr::VROverlayError_None) {
    fprintf(stderr, "could not create overlay: %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(oerr));
    return 1;
  }
  vr::VROverlay()->SetOverlayWidthInMeters(overlay, OVERLAY_WIDTH_M);
  vr::VROverlay()->SetOverlayFlag(overlay, vr::VROverlayFlags_SideBySide_Parallel, true);
  vr::VROverlay()->SetOverlayFlag(overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
  vr::VROverlay()->SetOverlayInputMethod(overlay, vr::VROverlayInputMethod_Mouse);
  vr::VROverlay()->SetOverlayFlag(overlay, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);
  // Only smooth scrolling: discrete scroll "notches" make SteamVR buzz the
  // controller on every tick.
  vr::HmdVector2_t mouse_scale = {1.0f, 1.0f};  // mouse events in 0..1 overlay coords
  vr::VROverlay()->SetOverlayMouseScale(overlay, &mouse_scale);

  bool placed = false;
  vec3 cube_pos = {0, 0, 0};
  int color = 0;
  bool hover = false;
  float cube_yaw = 0;  // cube's turn around the vertical axis, in room space
  bool manual_turn = false;  // stick took over turning during this grab
  float grab_dist = 1e9f;          // current hand-to-object distance while grabbed
  double last_close_pull = -1;     // last time the stick pulled while already touching
  float close_progress = 0;        // 0..1, closes the object at 1
  float pulse = 0;
  int grab_hand = -1;        // which hand is holding the cube, -1 = none
  vec3 grab_offset_local;    // cube position in the grabbing hand's frame
  vec3 grab_from_local;      // where the cube was when the grab started (hand frame)
  float grab_glide = 1;      // 0..1 progress of gliding onto the laser
  vr::HmdMatrix34_t overlay_xf = {};  // overlay pose last frame (overlay -> world)
  vec3 laser_hit = {0, 0, 0};         // where the laser last touched the overlay (world)
  bool laser_hit_ok = false;
  float grab_extra = 0;      // how far the stick has pushed (+) / pulled (-) it
  // Desktop mode keeps the controllers and only forwards laser clicks to
  // overlays (every button arrives as mouse button 1), so the laser does it all:
  // a quick click changes color, holding the click grabs.
  const float HOLD_TO_GRAB_S = 0.30f;
  bool laser_pressed = false;
  double laser_press_time = 0;  // when the press happened (from the event's own timestamp)
  vr::TrackedDeviceIndex_t laser_device = vr::k_unTrackedDeviceIndexInvalid;
  vr::TrackedDeviceIndex_t grab_device = vr::k_unTrackedDeviceIndexInvalid;
  auto device_pose = [](vr::TrackedDeviceIndex_t dev, vr::HmdMatrix34_t *out) {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses,
                                                    vr::k_unMaxTrackedDeviceCount);
    if (dev >= vr::k_unMaxTrackedDeviceCount || !poses[dev].bPoseIsValid) return false;
    *out = poses[dev].mDeviceToAbsoluteTracking;
    return true;
  };
  auto next_color = [&] {
    color = (color + 1) % NCOLORS;
    pulse = 1;
    printf("%s is now %s\n", object, color == 0 && mesh.has_own_colors ? "original" : PALETTE_NAMES[color]);
    fflush(stdout);
  };
  printf("VR Cube overlay running. Point the laser at the object: click to change color, "
         "hold to grab and move it, stick up/down to push/pull. Ctrl+C to quit.\n");
  fflush(stdout);

  timespec last;
  clock_gettime(CLOCK_MONOTONIC, &last);
  while (!quit_requested) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    float dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) * 1e-9f;
    double now_s = now.tv_sec + now.tv_nsec * 1e-9;
    last = now;

    vr::HmdMatrix34_t head;
    bool head_ok = head_pose(&head);
    if (!placed && head_ok) {
      cube_pos = in_front_of(head);
      // Start turned a bit so you see two faces, not a flat square.
      vec3 to_head = position_of(head) - cube_pos;
      cube_yaw = atan2f(to_head.x, to_head.z) + 0.6f;
      placed = true;
      vr::VROverlay()->ShowOverlay(overlay);
    }

    // Controllers.
    vr::VRActiveActionSet_t active = {};
    active.ulActionSet = action_set;
    // Global overlay priority lets us get the controller buttons while desktop
    // mode (or a game) has focus. Needs SteamVR's "globalActionSetPriority"
    // developer setting (Settings > Developer > experimental overlay input).
    active.nPriority = vr::k_nActionSetOverlayGlobalPriorityMin;
    vr::VRInput()->UpdateActionState(&active, sizeof active, 1);
    for (int h = 0; h < 2; h++) {
      vr::InputDigitalActionData_t color_btn, grab_btn;
      vr::VRInput()->GetDigitalActionData(act_color, &color_btn, sizeof color_btn, hand_src[h]);
      vr::VRInput()->GetDigitalActionData(act_grab, &grab_btn, sizeof grab_btn, hand_src[h]);
      if (color_btn.bActive && color_btn.bChanged && color_btn.bState) next_color();

      vr::InputPoseActionData_t pose;
      vr::VRInput()->GetPoseActionDataForNextFrame(act_hand, vr::TrackingUniverseStanding, &pose,
                                                   sizeof pose, hand_src[h]);
      bool pose_ok = pose.bActive && pose.pose.bPoseIsValid;
      const vr::HmdMatrix34_t &hand = pose.pose.mDeviceToAbsoluteTracking;

      if (getenv("VRCUBE_DEBUG")) {
        static int last_dbg[2] = {-1, -1};
        int dbg = color_btn.bActive | color_btn.bState << 1 | grab_btn.bActive << 2 |
                  grab_btn.bState << 3 | pose_ok << 4;
        if (dbg != last_dbg[h]) {
          printf("[debug] %s hand: color active=%d down=%d | grab active=%d down=%d | pose=%d\n",
                 h ? "right" : "left", color_btn.bActive, color_btn.bState, grab_btn.bActive,
                 grab_btn.bState, pose_ok);
          fflush(stdout);
          last_dbg[h] = dbg;
        }
      }
      bool grab_down = grab_btn.bActive && grab_btn.bState;
      if (grab_down && grab_btn.bChanged && grab_hand < 0 && pose_ok && placed) {
        grab_hand = h;
        grab_offset_local = unrotate(hand, cube_pos - position_of(hand));
        grab_extra = 0;
      } else if (!grab_down && grab_hand == h) {
        grab_hand = -1;
      }

      if (grab_hand == h && pose_ok) {
        vr::InputAnalogActionData_t stick;
        vr::VRInput()->GetAnalogActionData(act_stick, &stick, sizeof stick, hand_src[h]);
        if (stick.bActive && fabsf(stick.y) > 0.15f) grab_extra += stick.y * 1.2f * dt;  // m/s
        // Keep the grab offset, then slide along the hand->cube line.
        vec3 arm = rotate(hand, grab_offset_local);
        float dist = length(arm);
        float wanted = fmaxf(CLOSEST_M, dist + grab_extra);
        grab_extra = wanted - dist;  // don't build up push past the minimum
        if (dist > 1e-3f) arm = arm * (wanted / dist);
        cube_pos = position_of(hand) + arm;
      }
    }
    // Laser: holding the click on the cube for a moment grabs it.
    if (laser_pressed) {
      vr::HmdMatrix34_t hand;
      if (grab_device == vr::k_unTrackedDeviceIndexInvalid && now_s - laser_press_time >= HOLD_TO_GRAB_S &&
          device_pose(laser_device, &hand)) {
        grab_device = laser_device;
        // Snap the cube onto the laser: keep its distance, but put its center
        // on the line from the controller through where the laser hit.
        vec3 to_cube = cube_pos - position_of(hand);
        vec3 to_hit = laser_hit - position_of(hand);
        float hit_len = length(to_hit);
        grab_from_local = unrotate(hand, to_cube);
        if (laser_hit_ok && hit_len > 1e-3f) to_cube = to_hit * (length(to_cube) / hit_len);
        grab_offset_local = unrotate(hand, to_cube);
        grab_glide = 0;
        grab_extra = 0;
        pulse = 0.6f;
        printf("Grabbed the %s\n", argc == 2 ? "model" : "cube");
        fflush(stdout);
      }
    }
    if (grab_device != vr::k_unTrackedDeviceIndexInvalid) {
      vr::HmdMatrix34_t hand;
      if (device_pose(grab_device, &hand)) {
        // Glide onto the laser instead of jumping (~0.25 s, eased in and out),
        // keeping the distance while the direction swings over.
        const float GLIDE_S = 0.25f;
        grab_glide = fminf(1, grab_glide + dt / GLIDE_S);
        float t = grab_glide * grab_glide * (3 - 2 * grab_glide);
        vec3 local = grab_from_local * (1 - t) + grab_offset_local * t;
        float want_len = length(grab_offset_local), got_len = length(local);
        if (got_len > 1e-4f) local = local * (want_len / got_len);
        vec3 arm = rotate(hand, local);
        float dist = length(arm);
        float wanted = fmaxf(CLOSEST_M, dist + grab_extra);
        grab_extra = wanted - dist;
        grab_dist = wanted;
        if (dist > 1e-3f) arm = arm * (wanted / dist);
        cube_pos = position_of(hand) + arm;
      }
    }

    // Where each eye is, in overlay space, for the window projection.
    float eye_local[2][3] = {{-HALF_IPD_M, 0, VIEW_DIST_M}, {HALF_IPD_M, 0, VIEW_DIST_M}};
    if (placed && head_ok) {
      vr::HmdMatrix34_t ov = set_overlay_pose(overlay, cube_pos, head);
      overlay_xf = ov;
      for (int e = 0; e < 2; e++) {
        vr::HmdMatrix34_t e2h = vr::VRSystem()->GetEyeToHeadTransform(e ? vr::Eye_Right : vr::Eye_Left);
        vec3 eye_world = position_of(head) + rotate(head, position_of(e2h));
        vec3 l = unrotate(ov, eye_world - position_of(ov));
        if (l.z > 0.05f) {  // only while you're in front of the window
          eye_local[e][0] = l.x;
          eye_local[e][1] = l.y;
          eye_local[e][2] = l.z;
        }
      }
    }

    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
      if (ev.eventType == vr::VREvent_Quit) {
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        quit_requested = 1;
      }
    while (vr::VROverlay()->PollNextOverlayEvent(overlay, &ev, sizeof ev)) {
      if (getenv("VRCUBE_DEBUG")) {
        if (ev.eventType == vr::VREvent_MouseButtonDown || ev.eventType == vr::VREvent_MouseButtonUp)
          printf("[debug] %.3f mouse %s button=%u device=%u\n", now.tv_sec % 1000 + now.tv_nsec * 1e-9,
                 ev.eventType == vr::VREvent_MouseButtonDown ? "down" : "up", ev.data.mouse.button,
                 ev.trackedDeviceIndex);
        else if (ev.eventType == vr::VREvent_ScrollDiscrete)
          printf("[debug] scroll %s x=%.2f y=%.2f device=%u\n",
                 ev.eventType == vr::VREvent_ScrollSmooth ? "smooth" : "discrete", ev.data.scroll.xdelta,
                 ev.data.scroll.ydelta, ev.trackedDeviceIndex);
        else if (ev.eventType == vr::VREvent_TouchPadMove)
          printf("[debug] touchpad x=%.2f y=%.2f\n", ev.data.touchPadMove.fValueXRaw, ev.data.touchPadMove.fValueYRaw);
        else if (ev.eventType != vr::VREvent_MouseMove)
          printf("[debug] overlay event %u\n", ev.eventType);
        fflush(stdout);
      }
      // Desktop mode turns the stick into smooth scrolling while the laser is
      // on the cube: up pushes the cube away, down pulls it closer, along the
      // line from the controller to the cube.
      if (ev.eventType == vr::VREvent_ScrollSmooth) {
        const float METERS_PER_SCROLL = 0.05f;  // full stick ~0.22/event -> ~1 m/s
        // While grabbing, left/right turns the cube (around the vertical axis
        // only, so it stays level). With hysteresis: small nudges are ignored;
        // past the threshold the stick takes over from the auto-spin until
        // you let go.
        const float RADIANS_PER_SCROLL = 0.16f;  // full stick ~0.22/event -> ~180 deg/s
        const float TAKE_OVER = 0.12f;           // about half of a full push
        const float DEADZONE = 0.03f;
        float x = ev.data.scroll.xdelta;
        if (grab_device != vr::k_unTrackedDeviceIndexInvalid) {
          if (!manual_turn && fabsf(x) > TAKE_OVER) manual_turn = true;
          if (manual_turn && fabsf(x) > DEADZONE) cube_yaw -= x * RADIANS_PER_SCROLL;
        }
        float y = fabsf(ev.data.scroll.ydelta) > 0.06f ? ev.data.scroll.ydelta : 0;
        float push = y * METERS_PER_SCROLL;
        if (grab_device != vr::k_unTrackedDeviceIndexInvalid) {
          grab_extra += push;  // applied in the grab update next frame
          if (push < 0 && grab_dist <= CLOSEST_M + 0.005f) last_close_pull = now_s;
        } else {
          vr::HmdMatrix34_t hand;
          if (device_pose(ev.trackedDeviceIndex, &hand)) {
            vec3 arm = cube_pos - position_of(hand);
            float dist = length(arm);
            float wanted = fmaxf(CLOSEST_M, dist + push);
            if (dist > 1e-3f) cube_pos = position_of(hand) + arm * (wanted / dist);
          }
        }
        continue;
      }
      // Desktop mode moves its on-screen keyboard to whichever overlay the
      // laser focuses, treating the cube like a text box. The cube never needs
      // a keyboard, so close one that opens for it; yours comes back when you
      // return to your window.
      if (ev.eventType == vr::VREvent_KeyboardOpened_Global) {
        bool ours = ev.data.keyboard.overlayHandle == overlay;
        if (getenv("VRCUBE_DEBUG"))
          printf("[debug] keyboard opened for %s overlay\n", ours ? "our" : "another");
        if (ours) vr::VROverlay()->HideKeyboard();
        continue;
      }
      if (ev.eventType == vr::VREvent_MouseButtonUp && laser_pressed) {
        // Time the hold with the events' timestamps, not our frames: when
        // desktop mode is busy (e.g. its keyboard is open) we can get the press
        // and release in the same batch.
        double held = (now_s - ev.eventAgeSeconds) - laser_press_time;
        bool was_grab = grab_device != vr::k_unTrackedDeviceIndexInvalid;
        if (was_grab) printf("Released the %s\n", argc == 2 ? "model" : "cube");
        else if (held < HOLD_TO_GRAB_S) next_color();  // quick click
        fflush(stdout);
        laser_pressed = false;
        grab_device = vr::k_unTrackedDeviceIndexInvalid;
        manual_turn = false;  // auto-spin resumes
        continue;
      }
      if (ev.eventType != vr::VREvent_MouseMove && ev.eventType != vr::VREvent_MouseButtonDown) {
        if (ev.eventType == vr::VREvent_FocusLeave) hover = false;
        continue;
      }
      // Hit test against what we drew: is there cube (alpha) under the pointer
      // in either eye's image? Mouse y is bottom-up, like GL rows; raw mode
      // renders upside down for SetOverlayRaw, so flip back there.
      float u = ev.data.mouse.x, v = ev.data.mouse.y;
      // Laser hit point in the world: mouse 0..1 across the (square) overlay.
      laser_hit = position_of(overlay_xf) +
                  rotate(overlay_xf, vec3{(u - 0.5f) * OVERLAY_WIDTH_M, (v - 0.5f) * OVERLAY_WIDTH_M, 0});
      laser_hit_ok = true;
      int px = (int)(u * EYE_PX), py = (int)((use_raw ? 1 - v : v) * EYE_PX);
      bool on_cube = false;
      if (px >= 0 && px < EYE_PX && py >= 0 && py < EYE_PX) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[cur]);
        for (int eye = 0; eye < 2; eye++) {
          unsigned char rgba[4];
          glReadPixels(eye * EYE_PX + px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
          if (rgba[3] > 0) on_cube = true;
        }
      }
      hover = on_cube;
      if (ev.eventType == vr::VREvent_MouseButtonDown && on_cube) {
        // A new press always starts fresh, even if we never saw the last release.
        grab_device = vr::k_unTrackedDeviceIndexInvalid;
        manual_turn = false;
        laser_pressed = true;
        laser_press_time = now_s - ev.eventAgeSeconds;
        laser_device = ev.trackedDeviceIndex;
      }
    }

    if (!manual_turn) cube_yaw += dt * 0.6f;  // auto-spin

    // Pull in to close: keep pulling while the object touches the controller.
    bool pulling_in = grab_device != vr::k_unTrackedDeviceIndexInvalid && now_s - last_close_pull < 0.15;
    if (pulling_in) close_progress += dt / CLOSE_HOLD_S;
    else close_progress = fmaxf(0, close_progress - dt * 1.5f);  // drains if you stop
    if (close_progress >= 1 && !quit_requested) {
      printf("Closed: pulled into the controller\n");
      fflush(stdout);
      quit_requested = 1;
    }
    pulse = pulse > 0 ? pulse - dt * 4 : 0;
    bool highlight = hover || grab_hand >= 0 || grab_device != vr::k_unTrackedDeviceIndexInvalid;
    float s = 1 + 0.15f * pulse + (highlight ? 0.06f : 0);
    // The panel turns to face you, so subtract its turn: the cube keeps its
    // own direction in the room as you walk around it.
    float panel_yaw = atan2f(overlay_xf.m[0][2], overlay_xf.m[2][2]);
    mat4 model = mmul(rot_y(cube_yaw - panel_yaw), scale(s));  // top stays level
    // Models with their own colors start in their original look (white tint),
    // then cycle through the palette as tints.
    static const float WHITE[3] = {1, 1, 1};
    const float *tint = (mesh.has_own_colors && color == 0) ? WHITE : PALETTE[color];
    float rgb[3];
    for (int i = 0; i < 3; i++) rgb[i] = fminf(1, tint[i] * (highlight ? 1.15f : 1));

    int next = 1 - cur;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[next]);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);  // fully transparent: only the cube shows
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glUniformMatrix4fv(uModel, 1, GL_FALSE, model.m);
    glUniform3fv(uTint, 1, rgb);
    for (int eye = 0; eye < 2; eye++) {  // left half = left eye
      glViewport(eye * EYE_PX, 0, EYE_PX, EYE_PX);
      const float *el = eye_local[eye];
      mat4 mvp = mmul(window_view_proj(el[0], el[1], el[2], use_raw), model);
      glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
      for (const Mesh::Draw &d : mesh.draws) {
        bool textured = d.texture >= 0 && d.texture < (int)mesh_textures.size();
        glBindTexture(GL_TEXTURE_2D, textured ? mesh_textures[d.texture] : 0);
        glUniform1f(uUseTex, textured ? 1.0f : 0.0f);
        glDrawArrays(GL_TRIANGLES, (GLint)d.first, (GLsizei)d.count);
      }
      if (close_progress > 0) {  // the ring sits in front, unlit, facing you
        if (eye == 0) {
          build_close_ring(close_progress, ring_verts);
          glBindBuffer(GL_ARRAY_BUFFER, ring_vbo);
          glBufferData(GL_ARRAY_BUFFER, ring_verts.size() * sizeof(float), ring_verts.data(), GL_STREAM_DRAW);
        }
        static const float IDENTITY[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        static const float WHITE_TINT[3] = {1, 1, 1};
        mat4 ring_mvp = window_view_proj(el[0], el[1], el[2], use_raw);
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(ring_vao);
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, ring_mvp.m);
        glUniformMatrix4fv(uModel, 1, GL_FALSE, IDENTITY);
        glUniform3fv(uTint, 1, WHITE_TINT);
        glUniform1f(uUseTex, 0);
        glUniform1f(uUnlit, 1);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(ring_verts.size() / Mesh::FLOATS_PER_VERTEX));
        glUniform1f(uUnlit, 0);
        glUniformMatrix4fv(uModel, 1, GL_FALSE, model.m);
        glUniform3fv(uTint, 1, rgb);
        glBindVertexArray(vao);
        glEnable(GL_DEPTH_TEST);
      }
    }
    glFinish();  // SteamVR reads the texture from another process
    cur = next;

    bool dump = false;
    static bool dumped = false;
    if (!dumped && getenv("VRCUBE_DUMP")) dump = dumped = true;
    if (use_raw || dump) glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (placed && use_raw) {
      vr::VROverlay()->SetOverlayRaw(overlay, pixels, W, H, 4);
    } else if (placed) {
      vr::Texture_t tex = {(void *)(uintptr_t)color_tex[cur], vr::TextureType_OpenGL, vr::ColorSpace_Auto};
      vr::EVROverlayError e = vr::VROverlay()->SetOverlayTexture(overlay, &tex);
      if (e != vr::VROverlayError_None) {
        fprintf(stderr, "SetOverlayTexture failed (%s); falling back to raw uploads\n",
                vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
        use_raw = true;
      }
    }

    // VRCUBE_DUMP=file.pam saves one frame (with alpha) for debugging.
    if (dump) {
      if (FILE *f = fopen(getenv("VRCUBE_DUMP"), "wb")) {
        fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", W, H);
        fwrite(pixels, 1, (size_t)W * H * 4, f);
        fclose(f);
      }
    }

    // Draw once per headset frame; fall back to ~60 fps if sync isn't available.
    if (vr::VROverlay()->WaitFrameSync(100) != vr::VROverlayError_None) {
      timespec ts = {0, 16 * 1000 * 1000};
      nanosleep(&ts, NULL);
    }
  }

  vr::VROverlay()->DestroyOverlay(overlay);
  vr::VR_Shutdown();
  printf("Bye!\n");
  return 0;
}
