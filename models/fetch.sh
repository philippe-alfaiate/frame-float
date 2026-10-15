#!/bin/sh
# Download one free sample model per supported format into this folder.
# The models are not part of the repo (see their licenses below); run this
# script to get them.
set -eu
cd "$(dirname "$0")"

fetch() {  # url file
  if [ -s "$2" ]; then echo "have $2"; return; fi
  echo "fetching $2"
  curl -fL --retry 2 -o "$2.part" "$1"
  mv "$2.part" "$2"
}

# STL: 3DBenchy by Creative Tools, CC BY-ND 4.0 (https://www.3dbenchy.com)
fetch https://raw.githubusercontent.com/CreativeTools/3DBenchy/master/Single-part/3DBenchy.stl 3DBenchy.stl

# OBJ: Spot the cow by Keenan Crane, CC0
# (https://www.cs.cmu.edu/~kmcrane/Projects/ModelRepository/)
fetch https://raw.githubusercontent.com/alecjacobson/common-3d-test-models/master/data/spot.obj spot.obj

# PLY: Stanford Bunny, Stanford Computer Graphics Laboratory
# (https://graphics.stanford.edu/data/3Dscanrep/)
if [ ! -s bunny.ply ]; then
  fetch http://graphics.stanford.edu/pub/3Dscanrep/bunny.tar.gz bunny.tar.gz
  tar -xzf bunny.tar.gz bunny/reconstruction/bun_zipper.ply
  mv bunny/reconstruction/bun_zipper.ply bunny.ply
  rm -rf bunny bunny.tar.gz
fi

# glTF (binary .glb): Avocado from the Khronos glTF Sample Assets, CC0
fetch https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Avocado/glTF-Binary/Avocado.glb Avocado.glb

echo "done:"; ls -l ./*.stl ./*.obj ./*.ply ./*.glb
