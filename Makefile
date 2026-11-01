APP_NAME    := app
BUILD_DIR   := dist
VENDOR_DIR  := .vendor/quickjs
CIVET_DIR   := .vendor/civetweb
PUBLIC_DIR  := public
GLEAM_ENTRY := build/dev/javascript/$(APP_NAME)/$(APP_NAME).mjs
BUNDLE_OUT  := $(BUILD_DIR)/bundle.mjs
TARGET_BIN  := $(BUILD_DIR)/$(APP_NAME)

ESBUILD     ?= npx esbuild

.PHONY: all clean run distclean

all: $(TARGET_BIN)

# 1. Fetch & Build QuickJS compiler and static library
$(VENDOR_DIR)/qjsc:
	@mkdir -p .vendor
	@if [ ! -d "$(VENDOR_DIR)" ]; then \
		echo "==> Cloning QuickJS..."; \
		git clone --depth 1 https://github.com/bellard/quickjs.git $(VENDOR_DIR); \
	fi
	@echo "==> Compiling QuickJS..."
	$(MAKE) -C $(VENDOR_DIR) qjsc libquickjs.a

# 2. Fetch CivetWeb pinned to stable release v1.16
$(CIVET_DIR)/src/civetweb.c:
	@mkdir -p .vendor
	@if [ ! -d "$(CIVET_DIR)" ]; then \
		echo "==> Cloning CivetWeb (v1.16)..."; \
		git clone --depth 1 --branch v1.16 https://github.com/civetweb/civetweb.git $(CIVET_DIR); \
	fi

# 3. Create public directory if missing
$(PUBLIC_DIR):
	@mkdir -p $(PUBLIC_DIR)

# 4. Build Gleam to ES module JavaScript
$(GLEAM_ENTRY): src/$(APP_NAME).gleam src/server.gleam src/server_bridge.mjs src/ffi.mjs
	gleam build --target javascript

# 5. Bundle with esbuild
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

# 6. Generate C bytecode and link with QuickJS and CivetWeb
$(TARGET_BIN): $(VENDOR_DIR)/qjsc $(CIVET_DIR)/src/civetweb.c $(PUBLIC_DIR) $(BUNDLE_OUT) src/server_qjs.c
	@echo "==> Generating QuickJS C bytecode wrapper..."
	$(VENDOR_DIR)/qjsc -m -e -o $(BUILD_DIR)/wrapper.c $(BUNDLE_OUT)
	@echo "==> Patching C wrapper to register server global..."
	@sed -i 's/js_std_add_helpers(ctx, argc, argv);/js_std_add_helpers(ctx, argc, argv); { extern void js_init_server_global(JSContext *ctx); js_init_server_global(ctx); }/' $(BUILD_DIR)/wrapper.c
	@echo "==> Compiling native binary (-Os -s)..."
	$(CC) -Os -s \
		-I$(VENDOR_DIR) \
		-I$(CIVET_DIR)/include \
		-DUSE_WEBSOCKET \
		-DNO_SSL \
		$(BUILD_DIR)/wrapper.c \
		src/server_qjs.c \
		$(CIVET_DIR)/src/civetweb.c \
		$(VENDOR_DIR)/libquickjs.a \
		-lm -lpthread -ldl -o $(TARGET_BIN)
	@echo "==> Done: $(TARGET_BIN)"
	@ls -lh $(TARGET_BIN)
	@file $(TARGET_BIN)

run: $(TARGET_BIN)
	@./$(TARGET_BIN)

clean:
	gleam clean
	rm -rf $(BUILD_DIR) showcase_data.txt

distclean: clean
	rm -rf .vendor
