VR_LIB := /opt/steamvr/bin/linuxarm64
WARN := -Wall -Wextra -Wno-missing-field-initializers

all: vr-cube vr-cube-overlay

# OpenXR scene app (takes over the headset)
vr-cube: main.c
	$(CC) -O2 $(WARN) -o $@ $< -L$(VR_LIB) -lopenxr_loader -Wl,-rpath,$(VR_LIB) -lEGL -lOpenGL -lm

# SteamVR overlay (floats over desktop mode / passthrough)
vr-cube-overlay: overlay.cpp
	$(CXX) -O2 $(WARN) -o $@ $< -L$(VR_LIB) -lopenvr_api -Wl,-rpath,$(VR_LIB) -lEGL -lOpenGL -lm

clean:
	rm -f vr-cube vr-cube-overlay

.PHONY: all clean
