// Model loaders for vr-cube. STL, OBJ and PLY are parsed here; glTF/GLB goes
// through cgltf, and its textures through stb_image.
#include "model.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "third_party/stb/stb_image.h"

namespace {

struct V3 { float x, y, z; };
static V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V3 normalize(V3 a) {
  float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
  return l > 1e-20f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 1, 0};
}

static void push_vertex(Mesh &m, V3 p, V3 n, V3 c = {1, 1, 1}, float u = 0, float v = 0) {
  float f[Mesh::FLOATS_PER_VERTEX] = {p.x, p.y, p.z, n.x, n.y, n.z, c.x, c.y, c.z, u, v};
  m.verts.insert(m.verts.end(), f, f + Mesh::FLOATS_PER_VERTEX);
}

static bool read_file(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

static std::string lower_ext(const std::string &path) {
  size_t dot = path.rfind('.');
  std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
  for (char &c : e) c = (char)tolower((unsigned char)c);
  return e;
}

// Smooth normals for an indexed mesh: average the face normals around each
// position.
static std::vector<V3> smooth_normals(const std::vector<V3> &pos, const std::vector<uint32_t> &tris) {
  std::vector<V3> n(pos.size(), V3{0, 0, 0});
  for (size_t i = 0; i + 2 < tris.size(); i += 3) {
    V3 a = pos[tris[i]], b = pos[tris[i + 1]], c = pos[tris[i + 2]];
    V3 fn = cross(sub(b, a), sub(c, a));  // area-weighted
    for (int k = 0; k < 3; k++) n[tris[i + k]] = add(n[tris[i + k]], fn);
  }
  for (V3 &v : n) v = normalize(v);
  return n;
}

// ---------- STL (binary and ASCII) ----------
// STL is usually Z-up; we use Y-up, so (x, y, z) -> (x, z, -y).
static V3 stl_up(V3 p) { return {p.x, p.z, -p.y}; }

static bool load_stl(const std::string &path, Mesh &m, std::string &err) {
  std::string data;
  if (!read_file(path, data)) { err = "cannot read file"; return false; }
  // Binary STL: 80-byte header, triangle count, 50 bytes per triangle. Many
  // binary files start with "solid" too, so trust the size, not the header.
  if (data.size() >= 84) {
    uint32_t n;
    memcpy(&n, data.data() + 80, 4);
    if (84 + (uint64_t)n * 50 == data.size()) {
      const char *p = data.data() + 84;
      for (uint32_t t = 0; t < n; t++, p += 50) {
        float f[12];
        memcpy(f, p, sizeof f);
        V3 v[3] = {stl_up({f[3], f[4], f[5]}), stl_up({f[6], f[7], f[8]}), stl_up({f[9], f[10], f[11]})};
        V3 fn = normalize(cross(sub(v[1], v[0]), sub(v[2], v[0])));  // stored normals are often junk
        for (V3 &q : v) push_vertex(m, q, fn);
      }
      return n > 0 || (err = "empty STL", false);
    }
  }
  // ASCII STL: "vertex x y z" lines, three per facet.
  std::istringstream in(data);
  std::string word;
  std::vector<V3> v;
  while (in >> word) {
    if (word != "vertex") continue;
    V3 p;
    in >> p.x >> p.y >> p.z;
    v.push_back(stl_up(p));
    if (v.size() == 3) {
      V3 fn = normalize(cross(sub(v[1], v[0]), sub(v[2], v[0])));
      for (V3 &q : v) push_vertex(m, q, fn);
      v.clear();
    }
  }
  if (m.verts.empty()) { err = "no triangles found (not an STL file?)"; return false; }
  return true;
}

// ---------- OBJ ----------
static bool load_obj(const std::string &path, Mesh &m, std::string &err) {
  std::ifstream in(path);
  if (!in) { err = "cannot read file"; return false; }
  std::vector<V3> pos, col, nrm;
  std::vector<float> uv;  // pairs
  struct Corner { int p, t, n; };
  std::vector<Corner> corners;  // three per triangle
  bool any_color = false;
  auto fix = [](int i, size_t count) { return i < 0 ? (int)count + i : i - 1; };  // 1-based, negative = from end

  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string tag;
    if (!(ls >> tag)) continue;
    if (tag == "v") {
      V3 p, c = {1, 1, 1};
      ls >> p.x >> p.y >> p.z;
      if (ls >> c.x >> c.y >> c.z) any_color = true;  // optional "v x y z r g b"
      pos.push_back(p);
      col.push_back(c);
    } else if (tag == "vt") {
      float u = 0, v = 0;
      ls >> u >> v;
      uv.push_back(u);
      uv.push_back(1 - v);  // OBJ v goes up, our textures go down
    } else if (tag == "vn") {
      V3 n;
      ls >> n.x >> n.y >> n.z;
      nrm.push_back(n);
    } else if (tag == "f") {
      std::vector<Corner> face;
      std::string tok;
      while (ls >> tok) {
        Corner c = {0, -1, -1};
        int vals[3] = {0, 0, 0};
        bool has[3] = {false, false, false};
        size_t start = 0;
        for (int k = 0; k < 3 && start <= tok.size(); k++) {
          size_t slash = tok.find('/', start);
          std::string part = tok.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
          if (!part.empty()) { vals[k] = atoi(part.c_str()); has[k] = true; }
          if (slash == std::string::npos) break;
          start = slash + 1;
        }
        if (!has[0]) continue;
        c.p = fix(vals[0], pos.size());
        if (has[1]) c.t = fix(vals[1], uv.size() / 2);
        if (has[2]) c.n = fix(vals[2], nrm.size());
        face.push_back(c);
      }
      for (size_t i = 1; i + 1 < face.size(); i++) {  // fan-triangulate polygons
        corners.push_back(face[0]);
        corners.push_back(face[i]);
        corners.push_back(face[i + 1]);
      }
    }
  }
  if (corners.empty()) { err = "no faces found (not an OBJ file?)"; return false; }

  // Normals: use the file's where given, otherwise smooth ones per position.
  std::vector<uint32_t> tris;
  for (const Corner &c : corners) tris.push_back((uint32_t)std::max(0, std::min(c.p, (int)pos.size() - 1)));
  std::vector<V3> smooth = smooth_normals(pos, tris);
  for (const Corner &c : corners) {
    if (c.p < 0 || c.p >= (int)pos.size()) { err = "face uses a missing vertex"; return false; }
    V3 n = (c.n >= 0 && c.n < (int)nrm.size()) ? normalize(nrm[c.n]) : smooth[c.p];
    float u = 0, v = 0;
    if (c.t >= 0 && (size_t)c.t * 2 + 1 < uv.size()) { u = uv[c.t * 2]; v = uv[c.t * 2 + 1]; }
    push_vertex(m, pos[c.p], n, col[c.p], u, v);
  }
  m.has_own_colors = any_color;
  return true;
}

// ---------- PLY (ASCII, binary little/big endian) ----------
struct PlyProp {
  std::string name;
  std::string type;        // scalar type, or item type for lists
  std::string count_type;  // non-empty for list properties
};
struct PlyElement {
  std::string name;
  size_t count = 0;
  std::vector<PlyProp> props;
};

static int ply_type_size(const std::string &t) {
  if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
  if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
  if (t == "int" || t == "uint" || t == "float" || t == "int32" || t == "uint32" || t == "float32") return 4;
  if (t == "double" || t == "float64") return 8;
  return 0;
}

// Reads PLY values in whichever encoding the file uses.
struct PlyReader {
  const std::string &data;
  size_t pos;
  int mode;  // 0 ascii, 1 little endian, 2 big endian
  bool ok = true;

  double next(const std::string &type) {
    if (mode == 0) {
      while (pos < data.size() && isspace((unsigned char)data[pos])) pos++;
      if (pos >= data.size()) { ok = false; return 0; }
      char *end;
      double v = strtod(data.c_str() + pos, &end);
      if (end == data.c_str() + pos) { ok = false; return 0; }
      pos = end - data.c_str();
      return v;
    }
    int size = ply_type_size(type);
    if (size == 0 || pos + size > data.size()) { ok = false; return 0; }
    unsigned char b[8];
    memcpy(b, data.data() + pos, size);
    pos += size;
    if (mode == 2) std::reverse(b, b + size);  // this machine is little endian
    if (type == "char" || type == "int8") return (int8_t)b[0];
    if (type == "uchar" || type == "uint8") return b[0];
    if (type == "short" || type == "int16") { int16_t v; memcpy(&v, b, 2); return v; }
    if (type == "ushort" || type == "uint16") { uint16_t v; memcpy(&v, b, 2); return v; }
    if (type == "int" || type == "int32") { int32_t v; memcpy(&v, b, 4); return v; }
    if (type == "uint" || type == "uint32") { uint32_t v; memcpy(&v, b, 4); return v; }
    if (type == "float" || type == "float32") { float v; memcpy(&v, b, 4); return v; }
    double v;
    memcpy(&v, b, 8);
    return v;
  }
};

static bool load_ply(const std::string &path, Mesh &m, std::string &err) {
  std::string data;
  if (!read_file(path, data)) { err = "cannot read file"; return false; }
  size_t header_end = data.find("end_header");
  if (data.compare(0, 3, "ply") != 0 || header_end == std::string::npos) { err = "not a PLY file"; return false; }
  size_t body = data.find('\n', header_end);
  if (body == std::string::npos) { err = "truncated PLY header"; return false; }
  body++;

  int mode = -1;
  std::vector<PlyElement> elements;
  std::istringstream hs(data.substr(0, header_end));
  std::string line;
  while (std::getline(hs, line)) {
    std::istringstream ls(line);
    std::string kw;
    ls >> kw;
    if (kw == "format") {
      std::string f;
      ls >> f;
      mode = f == "ascii" ? 0 : f == "binary_little_endian" ? 1 : f == "binary_big_endian" ? 2 : -1;
    } else if (kw == "element") {
      PlyElement e;
      ls >> e.name >> e.count;
      elements.push_back(e);
    } else if (kw == "property" && !elements.empty()) {
      PlyProp p;
      std::string t;
      ls >> t;
      if (t == "list") ls >> p.count_type >> p.type >> p.name;
      else { p.type = t; ls >> p.name; }
      elements.back().props.push_back(p);
    }
  }
  if (mode < 0) { err = "unknown PLY format"; return false; }

  std::vector<V3> pos, nrm, col;
  std::vector<uint32_t> tris;
  bool has_normals = false, has_colors = false;
  PlyReader r{data, body, mode};
  for (const PlyElement &e : elements) {
    bool is_vertex = e.name == "vertex", is_face = e.name == "face";
    for (size_t i = 0; i < e.count && r.ok; i++) {
      V3 p = {0, 0, 0}, n = {0, 0, 0}, c = {1, 1, 1};
      for (const PlyProp &pr : e.props) {
        if (!pr.count_type.empty()) {
          size_t cnt = (size_t)r.next(pr.count_type);
          std::vector<uint32_t> idx(cnt);
          for (size_t k = 0; k < cnt; k++) idx[k] = (uint32_t)r.next(pr.type);
          if (is_face && (pr.name == "vertex_indices" || pr.name == "vertex_index"))
            for (size_t k = 1; k + 1 < cnt; k++) {  // fan-triangulate
              tris.push_back(idx[0]);
              tris.push_back(idx[k]);
              tris.push_back(idx[k + 1]);
            }
          continue;
        }
        double v = r.next(pr.type);
        if (!is_vertex) continue;
        float scale = (pr.type == "uchar" || pr.type == "uint8") ? 1 / 255.0f : 1;
        if (pr.name == "x") p.x = (float)v;
        else if (pr.name == "y") p.y = (float)v;
        else if (pr.name == "z") p.z = (float)v;
        else if (pr.name == "nx") { n.x = (float)v; has_normals = true; }
        else if (pr.name == "ny") n.y = (float)v;
        else if (pr.name == "nz") n.z = (float)v;
        else if (pr.name == "red" || pr.name == "r") { c.x = (float)v * scale; has_colors = true; }
        else if (pr.name == "green" || pr.name == "g") c.y = (float)v * scale;
        else if (pr.name == "blue" || pr.name == "b") c.z = (float)v * scale;
      }
      if (is_vertex) {
        pos.push_back(p);
        nrm.push_back(n);
        col.push_back(c);
      }
    }
  }
  if (!r.ok) { err = "PLY data ended early or is malformed"; return false; }
  if (tris.empty()) { err = "PLY has no faces (point clouds aren't supported)"; return false; }
  for (uint32_t i : tris)
    if (i >= pos.size()) { err = "PLY face uses a missing vertex"; return false; }

  std::vector<V3> normals = has_normals ? nrm : smooth_normals(pos, tris);
  for (uint32_t i : tris) push_vertex(m, pos[i], normalize(normals[i]), col[i]);
  m.has_own_colors = has_colors;
  return true;
}

// ---------- glTF / GLB ----------
static int load_gltf_texture(const cgltf_image *img, const std::string &path, const cgltf_options &opt,
                             Mesh &m, std::map<const cgltf_image *, int> &cache) {
  auto it = cache.find(img);
  if (it != cache.end()) return it->second;
  int w = 0, h = 0, comp = 0;
  unsigned char *pixels = NULL;
  if (img->buffer_view && img->buffer_view->buffer->data) {  // embedded (GLB)
    const unsigned char *src = (const unsigned char *)img->buffer_view->buffer->data + img->buffer_view->offset;
    pixels = stbi_load_from_memory(src, (int)img->buffer_view->size, &w, &h, &comp, 4);
  } else if (img->uri && strncmp(img->uri, "data:", 5) == 0) {  // base64 data URI
    const char *b64 = strchr(img->uri, ',');
    if (b64) {
      b64++;
      size_t len = strlen(b64), size = len / 4 * 3;
      if (len >= 1 && b64[len - 1] == '=') size--;
      if (len >= 2 && b64[len - 2] == '=') size--;
      void *raw = NULL;
      if (cgltf_load_buffer_base64(&opt, size, b64, &raw) == cgltf_result_success) {
        pixels = stbi_load_from_memory((const unsigned char *)raw, (int)size, &w, &h, &comp, 4);
        free(raw);
      }
    }
  } else if (img->uri) {  // file next to the .gltf
    std::string uri = img->uri;
    std::vector<char> decoded(uri.begin(), uri.end());
    decoded.push_back(0);
    cgltf_decode_uri(decoded.data());
    size_t slash = path.rfind('/');
    std::string file = (slash == std::string::npos ? "" : path.substr(0, slash + 1)) + decoded.data();
    pixels = stbi_load(file.c_str(), &w, &h, &comp, 4);
  }
  int index = -1;
  if (pixels) {
    Mesh::Texture t;
    t.width = w;
    t.height = h;
    t.rgba.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    m.textures.push_back(std::move(t));
    index = (int)m.textures.size() - 1;
  }
  cache[img] = index;
  return index;
}

static void add_gltf_node(const cgltf_node *node, const std::string &path, const cgltf_options &opt, Mesh &m,
                          std::map<const cgltf_image *, int> &tex_cache) {
  if (node->mesh) {
    float world[16];
    cgltf_node_transform_world(node, world);  // column-major
    auto xform = [&](V3 p, float w) {
      return V3{world[0] * p.x + world[4] * p.y + world[8] * p.z + world[12] * w,
                world[1] * p.x + world[5] * p.y + world[9] * p.z + world[13] * w,
                world[2] * p.x + world[6] * p.y + world[10] * p.z + world[14] * w};
    };
    for (size_t pi = 0; pi < node->mesh->primitives_count; pi++) {
      const cgltf_primitive &prim = node->mesh->primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles) continue;
      const cgltf_accessor *a_pos = NULL, *a_nrm = NULL, *a_col = NULL, *a_uv = NULL;
      for (size_t ai = 0; ai < prim.attributes_count; ai++) {
        const cgltf_attribute &at = prim.attributes[ai];
        if (at.type == cgltf_attribute_type_position) a_pos = at.data;
        else if (at.type == cgltf_attribute_type_normal) a_nrm = at.data;
        else if (at.type == cgltf_attribute_type_color && at.index == 0) a_col = at.data;
        else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) a_uv = at.data;
      }
      if (!a_pos) continue;

      float base[4] = {1, 1, 1, 1};
      int texture = -1;
      if (prim.material && prim.material->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness &pbr = prim.material->pbr_metallic_roughness;
        memcpy(base, pbr.base_color_factor, sizeof base);
        if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image)
          texture = load_gltf_texture(pbr.base_color_texture.texture->image, path, opt, m, tex_cache);
        m.has_own_colors = true;
      }
      if (a_col) m.has_own_colors = true;

      size_t count = prim.indices ? prim.indices->count : a_pos->count;
      Mesh::Draw d;
      d.first = m.vertex_count();
      d.texture = texture;
      for (size_t i = 0; i + 2 < count; i += 3) {
        size_t idx[3];
        V3 p[3];
        for (int k = 0; k < 3; k++) {
          idx[k] = prim.indices ? cgltf_accessor_read_index(prim.indices, i + k) : i + k;
          float f[4] = {0, 0, 0, 1};
          cgltf_accessor_read_float(a_pos, idx[k], f, 3);
          p[k] = xform({f[0], f[1], f[2]}, 1);
        }
        V3 flat = normalize(cross(sub(p[1], p[0]), sub(p[2], p[0])));
        for (int k = 0; k < 3; k++) {
          V3 n = flat;
          if (a_nrm) {
            float f[3];
            cgltf_accessor_read_float(a_nrm, idx[k], f, 3);
            n = normalize(xform({f[0], f[1], f[2]}, 0));
          }
          V3 c = {base[0], base[1], base[2]};
          if (a_col) {
            float f[4] = {1, 1, 1, 1};
            cgltf_accessor_read_float(a_col, idx[k], f, 4);
            c = {c.x * f[0], c.y * f[1], c.z * f[2]};
          }
          float uv[2] = {0, 0};
          if (a_uv) cgltf_accessor_read_float(a_uv, idx[k], uv, 2);
          push_vertex(m, p[k], n, c, uv[0], uv[1]);
        }
      }
      d.count = m.vertex_count() - d.first;
      if (d.count) m.draws.push_back(d);
    }
  }
  for (size_t i = 0; i < node->children_count; i++) add_gltf_node(node->children[i], path, opt, m, tex_cache);
}

static bool load_gltf(const std::string &path, Mesh &m, std::string &err) {
  cgltf_options opt = {};
  cgltf_data *data = NULL;
  if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) { err = "cannot parse glTF"; return false; }
  if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) {
    cgltf_free(data);
    err = "cannot load glTF buffers";
    return false;
  }
  std::map<const cgltf_image *, int> tex_cache;
  const cgltf_scene *scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : NULL);
  if (scene) {
    for (size_t i = 0; i < scene->nodes_count; i++) add_gltf_node(scene->nodes[i], path, opt, m, tex_cache);
  } else {
    for (size_t i = 0; i < data->nodes_count; i++)
      if (!data->nodes[i].parent) add_gltf_node(&data->nodes[i], path, opt, m, tex_cache);
  }
  cgltf_free(data);
  if (m.verts.empty()) { err = "glTF has no triangle meshes"; return false; }
  return true;
}

}  // namespace

bool load_model(const std::string &path, Mesh &mesh, std::string &error) {
  mesh = Mesh();
  std::string ext = lower_ext(path);
  bool ok;
  if (ext == "stl") ok = load_stl(path, mesh, error);
  else if (ext == "obj") ok = load_obj(path, mesh, error);
  else if (ext == "ply") ok = load_ply(path, mesh, error);
  else if (ext == "gltf" || ext == "glb") ok = load_gltf(path, mesh, error);
  else { error = "unsupported file type '." + ext + "' (use .stl .obj .ply .gltf .glb)"; return false; }
  if (ok && mesh.draws.empty()) mesh.draws.push_back({0, mesh.vertex_count(), -1});
  return ok;
}

Mesh make_cube_mesh() {
  Mesh m;
  static const float n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  static const float corners[6][2] = {{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
  for (int f = 0; f < 6; f++) {
    V3 N = {n[f][0], n[f][1], n[f][2]};
    V3 U = fabsf(N.y) > 0.5f ? V3{1, 0, 0} : V3{0, 1, 0};
    V3 W = cross(N, U);
    for (int i = 0; i < 6; i++) {
      float a = corners[i][0] * 0.5f, b = corners[i][1] * 0.5f;
      V3 p = {N.x * 0.5f + U.x * a + W.x * b, N.y * 0.5f + U.y * a + W.y * b, N.z * 0.5f + U.z * a + W.z * b};
      push_vertex(m, p, N);
    }
  }
  m.draws.push_back({0, m.vertex_count(), -1});
  return m;
}

void fit_mesh(Mesh &mesh, float radius) {
  const int S = Mesh::FLOATS_PER_VERTEX;
  size_t n = mesh.vertex_count();
  if (!n) return;
  V3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i < n; i++) {
    const float *p = &mesh.verts[i * S];
    lo = {fminf(lo.x, p[0]), fminf(lo.y, p[1]), fminf(lo.z, p[2])};
    hi = {fmaxf(hi.x, p[0]), fmaxf(hi.y, p[1]), fmaxf(hi.z, p[2])};
  }
  V3 c = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
  float r2 = 0;
  for (size_t i = 0; i < n; i++) {
    const float *p = &mesh.verts[i * S];
    float dx = p[0] - c.x, dy = p[1] - c.y, dz = p[2] - c.z;
    r2 = fmaxf(r2, dx * dx + dy * dy + dz * dz);
  }
  float s = r2 > 0 ? radius / sqrtf(r2) : 1;
  for (size_t i = 0; i < n; i++) {
    float *p = &mesh.verts[i * S];
    p[0] = (p[0] - c.x) * s;
    p[1] = (p[1] - c.y) * s;
    p[2] = (p[2] - c.z) * s;
  }
}
