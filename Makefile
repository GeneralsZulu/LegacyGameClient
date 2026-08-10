# Top-level convenience targets. The CMake/Ninja build is driven by
# scripts/docker-build.sh; this Makefile only wraps the steps that turn the
# build output plus the assets/ tree into a shippable installer.
#
# Common invocations:
#   make installer           build Zulu.big, build the exe, package Zulu_Setup.exe
#   make installer-release   build the installer and upload it to GCS as a
#                            versioned, publicly-downloadable object
#   make zulu-big            just (re)pack assets/ into build/installer-tmp/Zulu.big
#   make replay-bigs         rebuild the per-release data archives (ReplayData/)
#                            that Replay Theater mounts to play back old replays
#   make zulu-exe            build the shipping Release exe
#   make zulu-exe-log        build a Release exe with DEBUG_LOGGING + DEBUG_CRASHING
#                            (no debug CRT; same DLL deps as the shipping exe)
#   make clean-installer     remove the staged tmp dir and the built setup exe

BIG          ?= big
NSIS         ?= makensis
DOCKER_BUILD ?= ./scripts/docker-build.sh
GCLOUD       ?= gcloud
GCS_BUCKET   ?= zulu-installer

ASSETS_DIR := assets
TMP_DIR    := build/installer-tmp

# scripts/docker-build.sh keys its build directory off the cmake preset:
# build/docker-<preset>. The shipping exe and the launcher use the default
# `vc6` preset; the logging exe uses `vc6-releaselog`, which switches
# RTS_DEBUG_LOGGING/RTS_DEBUG_CRASHING on. They deliberately no longer share a
# cmake cache: a `vc6` configure on top of a `vc6-releaselog` cache kept the
# DEBUG_* flags (cmake caches are sticky for variables a preset does not name),
# so `make installer` silently produced a logging-enabled "release" binary.
# Separate dirs also keep both configurations' ninja state warm, so alternating
# `make installer` and `make installer-dev` rebuilds nothing.
PRESET_DEFAULT := vc6
PRESET_LOG     := vc6-releaselog
BUILD_DIR      := build/docker-$(PRESET_DEFAULT)
BUILD_DIR_LOG  := build/docker-$(PRESET_LOG)

BIG_NAME      := Zulu.big
EXE_NAME      := generalszh_zulu.exe
EXE_LOG_NAME  := generalszh_zulu_log.exe
LAUNCHER_NAME := ZuluLauncher.exe
SOURCE_EXE      := $(BUILD_DIR)/GeneralsMD/generalszh.exe
SOURCE_EXE_LOG  := $(BUILD_DIR_LOG)/GeneralsMD/generalszh.exe
SOURCE_LAUNCHER := $(BUILD_DIR)/launcher/ZuluLauncher.exe

# Per-release data archives for old-replay playback. Reconstructed from git on
# demand rather than checked in: they are ~95% identical to one another, so
# storing them would add ~73 MB of near-duplicate binaries to the repo.
REPLAY_DATA_CSV := installer/replay_data_versions.csv
REPLAY_DATA_DIR := $(TMP_DIR)/ReplayData
REPLAY_DATA_STAMP := $(TMP_DIR)/.replay-data-built

NSI               := installer/Zulu.nsi
INSTALLER_OUT     := installer/Zulu_Setup.exe
INSTALLER_OUT_DEV := installer/Zulu_Setup_Dev.exe
LATEST_JSON       := $(TMP_DIR)/latest.json
LATEST_DEV_JSON   := $(TMP_DIR)/latest-dev.json
# Hash of the game exe a dev installer carries, stashed while it is still
# staged so the manifest can publish it. See the installer-dev recipe.
DEV_EXE_SHA_FILE  := $(TMP_DIR)/dev-exe-sha256.txt

# Dev artifact lives at a fixed name in the same GCS bucket so the dev
# launcher can always pull the latest dev build from a stable URL; the
# manifest sidecar (latest-dev.json) carries the SHA256 the launcher
# uses to decide whether to prompt for an update.
DEV_OBJECT_NAME       := Zulu-Installer-Dev.exe
DEV_GCS_URI            = gs://$(GCS_BUCKET)/$(DEV_OBJECT_NAME)
DEV_PUBLIC_URL         = https://storage.googleapis.com/$(GCS_BUCKET)/$(DEV_OBJECT_NAME)
LATEST_DEV_GCS_URI     = gs://$(GCS_BUCKET)/latest-dev.json
LATEST_DEV_PUBLIC_URL  = https://storage.googleapis.com/$(GCS_BUCKET)/latest-dev.json

# Single source of truth for the release version is APPVERSION inside the
# NSI script. Keep this Make-side parser tolerant of whitespace/quoting so
# the user can edit the .nsi without having to also touch the Makefile.
APPVERSION   := $(shell sed -n 's/^!define[[:space:]]\+APPVERSION[[:space:]]\+"\([^"]*\)".*/\1/p' $(NSI))
RELEASE_NAME      = Zulu_Setup_v$(APPVERSION).exe
GCS_URI           = gs://$(GCS_BUCKET)/$(RELEASE_NAME)
PUBLIC_URL        = https://storage.googleapis.com/$(GCS_BUCKET)/$(RELEASE_NAME)
LATEST_GCS_URI    = gs://$(GCS_BUCKET)/latest.json
LATEST_PUBLIC_URL = https://storage.googleapis.com/$(GCS_BUCKET)/latest.json

# Decompose APPVERSION ("major.minor.build") for the cmake build so the
# binary's TheVersion (and therefore every replay it records) reports the
# same version as the installer. Empty trailing components default to 0
# so APPVERSION="1.1" still parses as 1.1.0. Forwarded to docker-build.sh
# as ZULU_VERSION_* env vars; cmake is force-reconfigured below so the
# new values are baked into BuildVersion.h on every Make-driven build.
ZULU_VERSION_MAJOR    := $(or $(word 1,$(subst ., ,$(APPVERSION))),0)
ZULU_VERSION_MINOR    := $(or $(word 2,$(subst ., ,$(APPVERSION))),0)
ZULU_VERSION_BUILDNUM := $(or $(word 3,$(subst ., ,$(APPVERSION))),0)

TMP_BIG      := $(TMP_DIR)/$(BIG_NAME)
TMP_EXE      := $(TMP_DIR)/$(EXE_NAME)
TMP_EXE_LOG  := $(TMP_DIR)/$(EXE_LOG_NAME)
TMP_LAUNCHER := $(TMP_DIR)/$(LAUNCHER_NAME)

ASSET_FILES := $(shell find $(ASSETS_DIR) -type f 2>/dev/null)

# Empty-archive seed bytes for `big add` to work against. The Go BIGF library
# refuses to open a missing or zero-byte file, so we hand-roll a 16-byte
# header: magic "BIGF", little-endian archive_size = 16, big-endian count = 0,
# big-endian first-data offset = 16.
EMPTY_BIG_BYTES := 'BIGF\x10\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x10'

.PHONY: installer installer-release installer-dev zulu-big replay-bigs zulu-exe zulu-exe-log zulu-launcher clean-installer

# Target-specific variables that propagate down the prereq chain so the
# docker-build-* recipes pick the right Discord webhook secret and bake
# the right build-variant tag into the binaries. Plain `make zulu-exe`
# leaves these empty: the discord fetch is skipped and the cmake
# buildvariant module defaults the tag to "dev".
installer:         DISCORD_WEBHOOK_SECRET := discord_webhook
installer:         ZULU_BUILD_VARIANT     := release
installer-release: DISCORD_WEBHOOK_SECRET := discord_webhook
installer-release: ZULU_BUILD_VARIANT     := release
installer-dev:     DISCORD_WEBHOOK_SECRET := debug_discord_webhook
installer-dev:     ZULU_BUILD_VARIANT     := dev

installer: $(INSTALLER_OUT)

zulu-big: $(TMP_BIG)

replay-bigs: $(REPLAY_DATA_STAMP)

zulu-exe: $(TMP_EXE)

zulu-exe-log: $(TMP_EXE_LOG)

zulu-launcher: $(TMP_LAUNCHER)

# Stage the BIG by packing every file in assets/ in one pass, mapping each
# relative path to a backslash-separated archive path (Data/INI/Foo.ini ->
# Data\INI\Foo.ini). Rebuilt from scratch each time so removed assets do not
# linger inside the archive. The old per-file `big add` loop rewrote the
# archive once per file, which took ~12 minutes once the community patch INI
# tree (2000+ files) moved into assets/ (and its expanded command line
# overflowed ARG_MAX).
$(TMP_BIG): $(ASSET_FILES) | $(TMP_DIR)
	@rm -f $@
	@python3 scripts/pack_big.py $(ASSETS_DIR) $@

# Stage every historical Zulu.big next to a copy of the version map, so an
# installed client can mount the data a given replay was recorded against
# (see installer/replay_data_versions.csv for why that is necessary). Each
# archive is rebuilt from the assets tree its release shipped, straight out
# of git history, so this depends only on the map and the packer -- not on
# the current working tree.
$(REPLAY_DATA_STAMP): $(REPLAY_DATA_CSV) scripts/build_replay_bigs.py scripts/pack_big.py | $(TMP_DIR)
	@rm -rf "$(REPLAY_DATA_DIR)"
	@python3 scripts/build_replay_bigs.py "$(REPLAY_DATA_CSV)" "$(REPLAY_DATA_DIR)"
	@cp "$(REPLAY_DATA_CSV)" "$(REPLAY_DATA_DIR)/versions.csv"
	@touch $@

# Drive the docker build for the Zero Hour exe and copy the result into the
# tmp dir under its installer name. We always invoke docker-build.sh so the
# build system itself decides whether anything is stale; pinning to a single
# Make timestamp would lie about the underlying source tree.
#
# --cmake forces a configure pass so the ZULU_VERSION_* env vars below
# are picked up into BuildVersion.h. Without --cmake, ninja keeps the
# previous configure's BuildVersion.h and the version doesn't update.
#
# DISCORD_WEBHOOK_SECRET, when set by a parent target (installer /
# installer-release / installer-dev), names the GCP Secret Manager secret
# to fetch and bake into the binary as ZULU_DISCORD_WEBHOOK_URL. Empty
# (the default for plain `make zulu-exe`) skips the fetch entirely so dev
# builds keep working without gcloud credentials. The secret must be
# non-empty when requested; an empty payload aborts the build so a release
# never silently ships with the feature disabled.
.PHONY: docker-build-z_generals
docker-build-z_generals:
	@if [ -n "$(DISCORD_WEBHOOK_SECRET)" ] && [ -z "$$ZULU_DISCORD_WEBHOOK_URL" ]; then \
		echo "[discord] fetching secret '$(DISCORD_WEBHOOK_SECRET)' from GCP Secret Manager..."; \
		ZULU_DISCORD_WEBHOOK_URL=$$($(GCLOUD) secrets versions access latest --secret=$(DISCORD_WEBHOOK_SECRET)) \
			|| { echo "ERROR: gcloud failed to read secret '$(DISCORD_WEBHOOK_SECRET)' (see error above)."; exit 1; }; \
		if [ -z "$$ZULU_DISCORD_WEBHOOK_URL" ]; then \
			echo "ERROR: secret '$(DISCORD_WEBHOOK_SECRET)' is empty."; \
			exit 1; \
		fi; \
		export ZULU_DISCORD_WEBHOOK_URL; \
		echo "[discord] webhook url loaded ($${#ZULU_DISCORD_WEBHOOK_URL} bytes)"; \
	fi; \
	ZULU_VERSION_MAJOR=$(ZULU_VERSION_MAJOR) \
	ZULU_VERSION_MINOR=$(ZULU_VERSION_MINOR) \
	ZULU_VERSION_BUILDNUM=$(ZULU_VERSION_BUILDNUM) \
	ZULU_BUILD_VARIANT=$(ZULU_BUILD_VARIANT) \
	$(DOCKER_BUILD) --cmake --target z_generals

$(TMP_EXE): docker-build-z_generals | $(TMP_DIR)
	cp "$(SOURCE_EXE)" "$@"

# Logging variant: same Release build (no debug CRT, same DLL deps as the
# shipping exe), but with DEBUG_LOGGING + DEBUG_CRASHING compiled in. Writes
# DebugLogFile.txt next to the running .exe at runtime. Built via the
# vc6-releaselog cmake preset, which gets its own build dir ($(BUILD_DIR_LOG))
# so it neither leaks its DEBUG_* flags into the shipping build nor invalidates
# that build's ninja state.
#
# Mirrors docker-build-z_generals's discord-secret fetch so installer-dev
# (which depends on this recipe to get the log-enabled exe) can bake in
# the debug discord webhook the same way installer bakes in the prod one.
.PHONY: docker-build-z_generals-log
docker-build-z_generals-log:
	@if [ -n "$(DISCORD_WEBHOOK_SECRET)" ] && [ -z "$$ZULU_DISCORD_WEBHOOK_URL" ]; then \
		echo "[discord] fetching secret '$(DISCORD_WEBHOOK_SECRET)' from GCP Secret Manager..."; \
		ZULU_DISCORD_WEBHOOK_URL=$$($(GCLOUD) secrets versions access latest --secret=$(DISCORD_WEBHOOK_SECRET)) \
			|| { echo "ERROR: gcloud failed to read secret '$(DISCORD_WEBHOOK_SECRET)' (see error above)."; exit 1; }; \
		if [ -z "$$ZULU_DISCORD_WEBHOOK_URL" ]; then \
			echo "ERROR: secret '$(DISCORD_WEBHOOK_SECRET)' is empty."; \
			exit 1; \
		fi; \
		export ZULU_DISCORD_WEBHOOK_URL; \
		echo "[discord] webhook url loaded ($${#ZULU_DISCORD_WEBHOOK_URL} bytes)"; \
	fi; \
	PRESET=$(PRESET_LOG) \
	ZULU_VERSION_MAJOR=$(ZULU_VERSION_MAJOR) \
	ZULU_VERSION_MINOR=$(ZULU_VERSION_MINOR) \
	ZULU_VERSION_BUILDNUM=$(ZULU_VERSION_BUILDNUM) \
	ZULU_BUILD_VARIANT=$(ZULU_BUILD_VARIANT) \
	$(DOCKER_BUILD) --cmake --target z_generals

$(TMP_EXE_LOG): docker-build-z_generals-log | $(TMP_DIR)
	cp "$(SOURCE_EXE_LOG)" "$@"
	@echo
	@echo "Logging exe ready: $@"
	@echo "Ship this to the rejoiner VM; DebugLogFile.txt will land next to the .exe."

# Launcher build, mirroring the game-exe rule. Same ZULU_VERSION_* env
# vars so the launcher's embedded VS_VERSION_INFO matches the game's.
.PHONY: docker-build-z_launcher
docker-build-z_launcher:
	ZULU_VERSION_MAJOR=$(ZULU_VERSION_MAJOR) \
	ZULU_VERSION_MINOR=$(ZULU_VERSION_MINOR) \
	ZULU_VERSION_BUILDNUM=$(ZULU_VERSION_BUILDNUM) \
	ZULU_BUILD_VARIANT=$(ZULU_BUILD_VARIANT) \
	$(DOCKER_BUILD) --cmake --target z_launcher

$(TMP_LAUNCHER): docker-build-z_launcher | $(TMP_DIR)
	cp "$(SOURCE_LAUNCHER)" "$@"

# Hand the staged paths to NSIS via /D overrides (paths are relative to the
# .nsi file's directory, i.e. installer/). After packaging, drop the staged
# big/exe/launcher so they don't sit around taking disk; keep $(TMP_DIR)
# itself in case something else stashes things there.
$(INSTALLER_OUT): $(TMP_BIG) $(TMP_EXE) $(TMP_LAUNCHER) $(REPLAY_DATA_STAMP) $(NSI)
	$(NSIS) \
		-DBIG_SOURCE="../$(TMP_BIG)" \
		-DEXE_SOURCE="../$(TMP_EXE)" \
		-DLAUNCHER_SOURCE="../$(TMP_LAUNCHER)" \
		-DREPLAYDATA_SOURCE="../$(REPLAY_DATA_DIR)" \
		$(NSI)
	@rm -f "$(TMP_BIG)" "$(TMP_EXE)" "$(TMP_LAUNCHER)"

$(TMP_DIR):
	@mkdir -p $@

clean-installer:
	rm -rf "$(TMP_DIR)" "$(INSTALLER_OUT)"

# Build a dev-tagged installer and publish it to GCS under a stable name
# (Zulu-Installer-Dev.exe + latest-dev.json) so dev launchers always know
# where to look. Uses the vc6-releaselog preset so dev builds ship with
# DEBUG_LOGGING + DEBUG_CRASHING on by default - that's the whole reason
# you'd cut a dev build.
#
# We rename the NSIS output to $(INSTALLER_OUT_DEV) so a later `make
# installer` doesn't see a fresh installer/Zulu_Setup.exe on disk and
# skip the build.
installer-dev: $(TMP_BIG) $(TMP_EXE_LOG) $(TMP_LAUNCHER) $(REPLAY_DATA_STAMP) $(NSI) | $(TMP_DIR)
	@test -n "$(APPVERSION)" || { \
		echo "ERROR: could not parse APPVERSION from $(NSI)"; exit 1; }
	$(NSIS) \
		-DBIG_SOURCE="../$(TMP_BIG)" \
		-DEXE_SOURCE="../$(TMP_EXE_LOG)" \
		-DLAUNCHER_SOURCE="../$(TMP_LAUNCHER)" \
		-DREPLAYDATA_SOURCE="../$(REPLAY_DATA_DIR)" \
		$(NSI)
	@# The dev update gate compares the game exe on disk against the game exe
	@# this installer carries, so the manifest needs that hash -- take it while
	@# the staged exe still exists (it is dropped once the manifest is written).
	@sha256sum "$(TMP_EXE_LOG)" | cut -d' ' -f1 > "$(DEV_EXE_SHA_FILE)"
	@rm -f "$(TMP_BIG)" "$(TMP_EXE_LOG)" "$(TMP_LAUNCHER)"
	mv "$(INSTALLER_OUT)" "$(INSTALLER_OUT_DEV)"
	$(GCLOUD) storage cp "$(INSTALLER_OUT_DEV)" "$(DEV_GCS_URI)"
	@$(GCLOUD) storage objects update "$(DEV_GCS_URI)" \
		--cache-control="no-cache, max-age=0" \
		|| echo "[note] could not set cache-control on $(DEV_OBJECT_NAME)."
	@$(GCLOUD) storage objects update "$(DEV_GCS_URI)" \
		--add-acl-grant=entity=AllUsers,role=READER \
		|| echo "[note] per-object ACL grant failed for $(DEV_OBJECT_NAME); rely on bucket-level allUsers grant."
	@# "sha256" is the installer's own hash; "exe_sha256" is the hash of the
	@# game exe inside it. The launcher's dev gate compares exe_sha256 against
	@# the installed generalszh_zulu.exe. Comparing against "sha256" instead is
	@# what made every dev launch think an update was waiting: it hashed the
	@# game exe and compared it to the hash of a completely different file.
	@SIZE=$$(stat -c%s "$(INSTALLER_OUT_DEV)"); \
	SHA=$$(sha256sum "$(INSTALLER_OUT_DEV)" | cut -d' ' -f1); \
	EXESHA=$$(cat "$(DEV_EXE_SHA_FILE)"); \
	printf '%s\n' \
	    '{' \
	    '  "version": "$(APPVERSION)",' \
	    '  "url": "$(DEV_PUBLIC_URL)",' \
	    "  \"size\": $$SIZE," \
	    "  \"sha256\": \"$$SHA\"," \
	    "  \"exe_sha256\": \"$$EXESHA\"" \
	    '}' > "$(LATEST_DEV_JSON)"
	$(GCLOUD) storage cp "$(LATEST_DEV_JSON)" "$(LATEST_DEV_GCS_URI)"
	@$(GCLOUD) storage objects update "$(LATEST_DEV_GCS_URI)" \
		--content-type=application/json \
		--cache-control="no-cache, max-age=0" \
		|| echo "[note] could not set metadata on latest-dev.json."
	@$(GCLOUD) storage objects update "$(LATEST_DEV_GCS_URI)" \
		--add-acl-grant=entity=AllUsers,role=READER \
		|| echo "[note] per-object ACL grant failed for latest-dev.json; rely on bucket-level allUsers grant."
	@rm -f "$(LATEST_DEV_JSON)" "$(DEV_EXE_SHA_FILE)"
	@echo
	@echo "Uploaded dev installer: $(DEV_GCS_URI)"
	@echo "Public URL:             $(DEV_PUBLIC_URL)"
	@echo "Update manifest:        $(LATEST_DEV_GCS_URI)"
	@echo "Manifest URL:           $(LATEST_DEV_PUBLIC_URL)"

# Build the installer (via the regular pipeline) and publish it to GCS under
# a version-stamped object name so each release has a stable shareable URL.
# The ACL grant marks the uploaded object world-readable; if the bucket has
# uniform bucket-level access enabled the per-object grant will fail, in
# which case allUsers must be granted Storage Object Viewer at the bucket
# level for the public URL to work.
installer-release: $(INSTALLER_OUT) | $(TMP_DIR)
	@test -n "$(APPVERSION)" || { \
		echo "ERROR: could not parse APPVERSION from $(NSI)"; exit 1; }
	$(GCLOUD) storage cp "$(INSTALLER_OUT)" "$(GCS_URI)"
	@$(GCLOUD) storage objects update "$(GCS_URI)" \
		--add-acl-grant=entity=AllUsers,role=READER \
		|| echo "[note] per-object ACL grant failed; if the bucket uses uniform bucket-level access, ensure allUsers is granted Storage Object Viewer at the bucket level."
	@# Manifest the launcher reads on every cold start. Cache-Control is
	@# overridden because GCS's default for public objects (max-age=3600)
	@# would mask freshly published releases for up to an hour. The launcher
	@# additionally sets INTERNET_FLAG_RELOAD client-side as belt-and-braces.
	@SIZE=$$(stat -c%s "$(INSTALLER_OUT)"); \
	SHA=$$(sha256sum "$(INSTALLER_OUT)" | cut -d' ' -f1); \
	printf '%s\n' \
	    '{' \
	    '  "version": "$(APPVERSION)",' \
	    '  "url": "$(PUBLIC_URL)",' \
	    "  \"size\": $$SIZE," \
	    "  \"sha256\": \"$$SHA\"" \
	    '}' > "$(LATEST_JSON)"
	$(GCLOUD) storage cp "$(LATEST_JSON)" "$(LATEST_GCS_URI)"
	@$(GCLOUD) storage objects update "$(LATEST_GCS_URI)" \
		--content-type=application/json \
		--cache-control="no-cache, max-age=0" \
		|| echo "[note] could not set metadata on latest.json (uniform bucket-level access?)."
	@$(GCLOUD) storage objects update "$(LATEST_GCS_URI)" \
		--add-acl-grant=entity=AllUsers,role=READER \
		|| echo "[note] per-object ACL grant failed for latest.json; rely on bucket-level allUsers grant."
	@rm -f "$(LATEST_JSON)"
	@echo
	@echo "Uploaded installer:  $(GCS_URI)"
	@echo "Public installer URL: $(PUBLIC_URL)"
	@echo "Update manifest:     $(LATEST_GCS_URI)"
	@echo "Manifest URL:        $(LATEST_PUBLIC_URL)"
