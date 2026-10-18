GAME_APP := $(HOME)/Library/Application Support/Steam/steamapps/common/Euro Truck Simulator 2/Euro Truck Simulator 2.app
PLUGIN_DIR := $(GAME_APP)/Contents/MacOS/plugins
HELPER := $(CURDIR)/rumble_helper

SDK := ../scs_sdk/include
SDK_INCLUDES := -I$(SDK) -I$(SDK)/common -I$(SDK)/eurotrucks2 -I$(SDK)/amtrucks
SDL_FLAGS := $(shell pkg-config --cflags --libs sdl3)
CFLAGS := -O2 -Wall -Wextra

all: ets2_rumble_plugin.dylib rumble_helper replay fake_telemetry

# The game is x86_64 (Rosetta), so the plugin must be too. The helper is native.
# The plugin starts the helper by absolute path, so build it in place.
ets2_rumble_plugin.dylib: plugin.cpp protocol.h
	clang++ -arch x86_64 -std=c++17 $(CFLAGS) -Wno-unused-parameter -dynamiclib \
		-mmacosx-version-min=11.0 $(SDK_INCLUDES) -DHELPER_PATH='"$(HELPER)"' \
		-install_name ets2_rumble_plugin.dylib plugin.cpp -o $@

rumble_helper: rumble_helper.c effects.c effects.h protocol.h recording.h
	cc $(CFLAGS) rumble_helper.c effects.c $(SDL_FLAGS) -o $@

replay: replay.c effects.c effects.h protocol.h recording.h
	cc $(CFLAGS) replay.c effects.c -o $@

fake_telemetry: fake_telemetry.c protocol.h
	cc $(CFLAGS) fake_telemetry.c -o $@

# Feel the effects without the game.
demo: rumble_helper fake_telemetry
	./rumble_helper & sleep 1; ./fake_telemetry; wait

# Find the weakest felt level of each motor; writes low_min/high_min to rumble.conf.
calibrate: rumble_helper
	./rumble_helper --calibrate

# Needs the App Management permission for the terminal; otherwise copy
# ets2_rumble_plugin.dylib into $(PLUGIN_DIR) with Finder.
install: ets2_rumble_plugin.dylib rumble_helper
	mkdir -p "$(PLUGIN_DIR)"
	cp ets2_rumble_plugin.dylib "$(PLUGIN_DIR)/"
	@echo "Installed to $(PLUGIN_DIR)"

uninstall:
	rm -f "$(PLUGIN_DIR)/ets2_rumble_plugin.dylib"
	@echo "Removed plugin"

clean:
	rm -f ets2_rumble_plugin.dylib rumble_helper replay fake_telemetry rumble_helper.log

.PHONY: all demo calibrate install uninstall clean
