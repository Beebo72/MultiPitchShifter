CC := gcc
CXX := g++

RUBBERBAND_SRC := import/rubberband-src
SIGNALSMITH_SRC := import/signalsmith-stretch-src
BUNGEE_SRC := import/bungee-src
SOUNDTOUCH_SRC := import/soundtouch-src

BUNGEE_OBJECTS := BungeeAssert.o BungeeFourier.o BungeeGrain.o BungeeGrains.o BungeeInput.o BungeeInstrumentation.o BungeeOutput.o BungeePartials.o BungeeStretcher.o BungeeStretch.o BungeeSynthesis.o BungeeTiming.o BungeeVersion.o BungeeWindow.o BungeePffft.o BungeeFftpack.o
SOUNDTOUCH_OBJECTS := SoundTouchAAFilter.o SoundTouchBPMDetect.o SoundTouchCpuDetect.o SoundTouchFIFOSampleBuffer.o SoundTouchFIRFilter.o SoundTouchInterpolateCubic.o SoundTouchInterpolateLinear.o SoundTouchInterpolateShannon.o SoundTouchMMX.o SoundTouchPeakFinder.o SoundTouchRateTransposer.o SoundTouchCore.o SoundTouchSSE.o SoundTouchTDStretch.o

CFLAGS := -std=c11 -O2 -Wall -Wextra -DUSE_EMBEDDED_RUBBERBAND -DUSE_EMBEDDED_SIGNALSMITH -DUSE_EMBEDDED_BUNGEE -DUSE_EMBEDDED_SOUNDTOUCH -I. -I$(RUBBERBAND_SRC) -I$(SIGNALSMITH_SRC) -I$(BUNGEE_SRC) -I$(SOUNDTOUCH_SRC)/include
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -I. -I$(RUBBERBAND_SRC) -I$(SIGNALSMITH_SRC) -I$(BUNGEE_SRC) -I$(BUNGEE_SRC)/submodules/eigen -I$(SOUNDTOUCH_SRC)/include -DBUNGEE_SELF_TEST=0 -DBUNGEE_VERSION=\"embedded\" -Deigen_assert=BUNGEE_ASSERT1 -DEIGEN_DONT_PARALLELIZE=1 -DSOUNDTOUCH_FLOAT_SAMPLES=1 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS=1 -fwrapv
LDFLAGS := -static -lm -static-libstdc++ -static-libgcc

.PHONY: all clean rebuild prototype audiofx windows

all: multipitch audiofx
windows: multipitch.exe

rebuild: clean all

prototype: uptempo_prototype.exe

multipitch: multipitch.o RubberBandSingle.o SignalsmithBridge.o BungeeBridge.o SoundTouchBridge.o $(BUNGEE_OBJECTS) $(SOUNDTOUCH_OBJECTS)
	$(CXX) -O2 -o $@ $^ $(LDFLAGS)

multipitch.exe: multipitch.o RubberBandSingle.o SignalsmithBridge.o BungeeBridge.o SoundTouchBridge.o $(BUNGEE_OBJECTS) $(SOUNDTOUCH_OBJECTS)
	$(CXX) -O2 -o $@ $^ $(LDFLAGS)

multipitch.o: multipitch.c
	$(CC) $(CFLAGS) -c $< -o $@

audiofx: audiofx.o RubberBandSingle.o
	$(CXX) -O2 -o $@ $^ $(LDFLAGS)

audiofx.o: audiofx.c
	$(CC) $(CFLAGS) -c $< -o $@

RubberBandSingle.o: $(RUBBERBAND_SRC)/single/RubberBandSingle.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SignalsmithBridge.o: signalsmith_bridge.cpp signalsmith_bridge.h
	$(CXX) $(CXXFLAGS) -c signalsmith_bridge.cpp -o $@

BungeeBridge.o: bungee_bridge.cpp bungee_bridge.h
	$(CXX) $(CXXFLAGS) -c bungee_bridge.cpp -o $@

SoundTouchBridge.o: soundtouch_bridge.cpp soundtouch_bridge.h
	$(CXX) $(CXXFLAGS) -c soundtouch_bridge.cpp -o $@

BungeeAssert.o: $(BUNGEE_SRC)/src/Assert.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeFourier.o: $(BUNGEE_SRC)/src/Fourier.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeGrain.o: $(BUNGEE_SRC)/src/Grain.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeGrains.o: $(BUNGEE_SRC)/src/Grains.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeInput.o: $(BUNGEE_SRC)/src/Input.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeInstrumentation.o: $(BUNGEE_SRC)/src/Instrumentation.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeOutput.o: $(BUNGEE_SRC)/src/Output.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeePartials.o: $(BUNGEE_SRC)/src/Partials.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeStretcher.o: $(BUNGEE_SRC)/src/Stretcher.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeStretch.o: $(BUNGEE_SRC)/src/Stretch.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeSynthesis.o: $(BUNGEE_SRC)/src/Synthesis.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeTiming.o: $(BUNGEE_SRC)/src/Timing.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeVersion.o: $(BUNGEE_SRC)/src/version.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeeWindow.o: $(BUNGEE_SRC)/src/Window.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

BungeePffft.o: $(BUNGEE_SRC)/submodules/pffft/pffft.c
	$(CC) -O2 -Wall -Wextra -I$(BUNGEE_SRC)/submodules/pffft -ffast-math -fno-finite-math-only -c $< -o $@

BungeeFftpack.o: $(BUNGEE_SRC)/submodules/pffft/fftpack.c
	$(CC) -O2 -Wall -Wextra -I$(BUNGEE_SRC)/submodules/pffft -ffast-math -fno-finite-math-only -c $< -o $@

SoundTouchAAFilter.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/AAFilter.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchBPMDetect.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/BPMDetect.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchCpuDetect.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/cpu_detect_x86.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchFIFOSampleBuffer.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/FIFOSampleBuffer.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchFIRFilter.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/FIRFilter.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchInterpolateCubic.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/InterpolateCubic.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchInterpolateLinear.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/InterpolateLinear.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchInterpolateShannon.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/InterpolateShannon.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchMMX.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/mmx_optimized.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchPeakFinder.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/PeakFinder.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchRateTransposer.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/RateTransposer.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchCore.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/SoundTouch.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchSSE.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/sse_optimized.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

SoundTouchTDStretch.o: $(SOUNDTOUCH_SRC)/source/SoundTouch/TDStretch.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

uptempo_prototype.exe: uptempo_prototype.cpp RubberBandSingle.o
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I$(RUBBERBAND_SRC) -mwindows uptempo_prototype.cpp RubberBandSingle.o -o $@ -lgdi32 -lmsimg32 -lcomdlg32 -lole32 -lshell32 -luuid -lwinmm

clean:
	rm -f multipitch.o audiofx.o RubberBandSingle.o SignalsmithBridge.o BungeeBridge.o SoundTouchBridge.o $(BUNGEE_OBJECTS) $(SOUNDTOUCH_OBJECTS)
