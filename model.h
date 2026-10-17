// 3D model loading for frame-float: STL, OBJ, PLY and glTF/GLB, all turned into
// one flat triangle list ready for OpenGL.
#pragma once
#include <string>
#include <vector>

struct Mesh {
  // Interleaved, one entry per triangle corner (no index buffer):
  // position xyz, normal xyz, color rgb, texture uv.
  static const int FLOATS_PER_VERTEX = 11;
  std::vector<float> verts;

  struct Texture {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
  };
  std::vector<Texture> textures;

  // A run of vertices drawn with one texture (-1 = none).
  struct Draw {
    size_t first = 0, count = 0;
    int texture = -1;
  };
  std::vector<Draw> draws;

  // True if the file brought its own colors (vertex colors, material colors or
  // textures); then the color cycle starts with the original look.
  bool has_own_colors = false;

  size_t vertex_count() const { return verts.size() / FLOATS_PER_VERTEX; }
};

// Load a model by file extension (.stl .obj .ply .gltf .glb). On failure
// returns false and sets `error`.
bool load_model(const std::string &path, Mesh &mesh, std::string &error);

// A unit cube (edge 1, centered), for when no model file is given.
Mesh make_cube_mesh();

// Center the mesh on the origin and scale it so it fits in a sphere of
// `radius`, so any model ends up the same size in front of you.
void fit_mesh(Mesh &mesh, float radius);
