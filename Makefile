VR_LIB ?= /opt/steamvr/bin/linuxarm64
PREFIX ?= /usr
DESTDIR ?=
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

# Installs the overlay. It looks for actions.json next to its real path, so the
# binary and the input files live together in lib/frame-float, and bin/ links to it.
install: frame-float-overlay
	install -Dm755 frame-float-overlay $(DESTDIR)$(PREFIX)/lib/frame-float/frame-float-overlay
	install -Dm644 -t $(DESTDIR)$(PREFIX)/lib/frame-float actions.json bindings_frame_controller.json bindings_frame_hmd.json
	install -d $(DESTDIR)$(PREFIX)/bin
	ln -sf ../lib/frame-float/frame-float-overlay $(DESTDIR)$(PREFIX)/bin/frame-float
	install -Dm644 frame-float.desktop $(DESTDIR)$(PREFIX)/share/applications/frame-float.desktop

clean:
	rm -f frame-float frame-float-overlay model.o

.PHONY: all install clean
