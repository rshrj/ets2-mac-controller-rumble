GAME_APP := $(HOME)/Library/Application Support/Steam/steamapps/common/Euro Truck Simulator 2/Euro Truck Simulator 2.app
PLUGIN_DIR := $(GAME_APP)/Contents/MacOS/plugins
PLUGIN := ets2_steam_input_fix.dylib

SDK_INCLUDES := -Iscs_sdk/include -Iscs_sdk/include/common
CXXFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -std=c++17

all: $(PLUGIN)

# The macOS game binary is x86_64 (it runs on Rosetta on Apple Silicon), so the
# plugin must be x86_64 too.
$(PLUGIN): src/steam_input_fix.cpp
	clang++ -arch x86_64 -mmacosx-version-min=11.0 $(CXXFLAGS) $(SDK_INCLUDES) -dynamiclib \
		-install_name $(PLUGIN) $< -o $@

# Writing into the game's app bundle needs the App Management permission for
# your terminal (System Settings > Privacy & Security > App Management). Without
# it, copy the dylib into the plugins folder with Finder instead.
install: $(PLUGIN)
	mkdir -p "$(PLUGIN_DIR)"
	cp $(PLUGIN) "$(PLUGIN_DIR)/"
	@echo "Installed to $(PLUGIN_DIR)"

uninstall:
	rm -f "$(PLUGIN_DIR)/$(PLUGIN)"
	@echo "Removed $(PLUGIN)"

clean:
	rm -f $(PLUGIN)

.PHONY: all install uninstall clean
