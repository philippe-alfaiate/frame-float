VR_LIB := /opt/steamvr/bin/linuxarm64
WARN := -Wall -Wextra -Wno-missing-field-initializers

all: frame-float frame-float-overlay

# OpenXR scene app (takes over the headset)
frame-float: main.c
	$(CC) -O2 $(WARN) -o $@ $< -L$(VR_LIB) -lopenxr_loader -Wl,-rpath,$(VR_LIB) -lEGL -lOpenGL -lm

# SteamVR overlay (floats over desktop mode / passthrough)
frame-float-overlay: overlay.cpp model.h model.o
	$(CXX) -O2 $(WARN) -o $@ overlay.cpp model.o -L$(VR_LIB) -lopenvr_api -Wl,-rpath,$(VR_LIB) -lEGL -lOpenGL -lm

# Model loaders (STL/OBJ/PLY, glTF via cgltf, textures via stb_image)
model.o: model.cpp model.h
	$(CXX) -O2 $(WARN) -c -o $@ $<

clean:
	rm -f frame-float frame-float-overlay model.o

.PHONY: all clean
