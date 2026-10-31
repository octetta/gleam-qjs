APP_NAME    := app
BUILD_DIR   := dist
VENDOR_DIR  := .vendor/quickjs
GLEAM_ENTRY := build/dev/javascript/$(APP_NAME)/$(APP_NAME).mjs
BUNDLE_OUT  := $(BUILD_DIR)/bundle.mjs
TARGET_BIN  := $(BUILD_DIR)/$(APP_NAME)

ESBUILD     ?= npx esbuild

SYSTEM_QJSC := $(shell which qjsc 2>/dev/null)

.PHONY: all clean run distclean

all: $(TARGET_BIN)

# 1. Compile QuickJS locally if not found in system PATH
$(VENDOR_DIR)/qjsc:
	@mkdir -p .vendor
	@if [ ! -d "$(VENDOR_DIR)" ]; then \
		echo "==> Cloning QuickJS..."; \
		git clone --depth 1 https://github.com/bellard/quickjs.git $(VENDOR_DIR); \
	fi
	@echo "==> Compiling QuickJS (qjsc & libquickjs.a)..."
	$(MAKE) -C $(VENDOR_DIR) qjsc libquickjs.a

# 2. Build Gleam to ES module JavaScript
$(GLEAM_ENTRY): src/$(APP_NAME).gleam src/ffi.mjs
	gleam build --target javascript

# 3. Bundle with esbuild, preserving QuickJS native modules and executing main()
$(BUNDLE_OUT): $(GLEAM_ENTRY)
	@mkdir -p $(BUILD_DIR)
	$(ESBUILD) $(GLEAM_ENTRY) \
		--bundle \
		--format=esm \
		--platform=neutral \
		--external:std \
		--external:os \
		--footer:js="main();" \
		--outfile=$(BUNDLE_OUT)

# 4. Compile to native executable
$(TARGET_BIN): $(BUNDLE_OUT) $(if $(SYSTEM_QJSC),,$(VENDOR_DIR)/qjsc)
ifdef SYSTEM_QJSC
	@echo "==> Building binary with system qjsc..."
	qjsc -m -o $(TARGET_BIN) $(BUNDLE_OUT)
	strip $(TARGET_BIN)
else
	@echo "==> Generating C wrapper with vendored qjsc..."
	$(VENDOR_DIR)/qjsc -m -e -o $(BUILD_DIR)/wrapper.c $(BUNDLE_OUT)
	@echo "==> Linking native binary via gcc..."
	$(CC) -Os -s -I$(VENDOR_DIR) $(BUILD_DIR)/wrapper.c $(VENDOR_DIR)/libquickjs.a -lm -lpthread -ldl -o $(TARGET_BIN)
endif
	@echo "==> Native binary ready: $(TARGET_BIN)"
	@ls -lh $(TARGET_BIN)

run: $(TARGET_BIN)
	@./$(TARGET_BIN)

clean:
	gleam clean
	rm -rf $(BUILD_DIR) sample_output.txt

distclean: clean
	rm -rf .vendor
