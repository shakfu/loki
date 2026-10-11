.DEFAULT_GOAL := all
.PHONY: all build configure library loki all clean test show-config check-sync

BUILD_DIR ?= build
CMAKE ?= cmake

all: build

configure:
	@mkdir -p build && $(CMAKE) -S . -B $(BUILD_DIR)

build: configure
	@$(CMAKE) --build $(BUILD_DIR) --config Release

library: configure
	@$(CMAKE) --build $(BUILD_DIR) --target libloki --config Release

loki: configure
	@$(CMAKE) --build $(BUILD_DIR) --target loki --config Release

show-config: configure
	@$(CMAKE) --build $(BUILD_DIR) --target show-config --config Release

test: build
	@$(CMAKE) -E chdir $(BUILD_DIR) ctest --output-on-failure

# Compare the generic-core files with psnd (upstream). PSND_DIR defaults to ../psnd.
check-sync:
	@scripts/check-psnd-sync.sh $(PSND_DIR)

clean:
	@$(CMAKE) -E rm -rf $(BUILD_DIR)
