# Warp Shaders capture plugin for SaltyNX (Spike 2).
# Needs devkitA64 only (no libnx): `make` builds warp-shaders.elf, `make check` verifies the
# ELF has the shape SaltyNX's loader expects.

DEVKITPRO ?= /opt/devkitpro
PREFIX    := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CC        := $(PREFIX)gcc
READELF   := $(PREFIX)readelf
STRIP     := $(PREFIX)strip

TARGET    := warp-shaders
comma     := ,
BUILD     := build
SOURCES   := source/crt0.s source/plugin.c
DEPS      := source/capture_format.h source/xxhash64.h plugin.ld

CFLAGS    := -O2 -Wall -Wextra -Werror -std=gnu11 \
             -march=armv8-a -mtune=cortex-a57 -mgeneral-regs-only -mno-outline-atomics \
             -fPIE -fno-plt -fvisibility=hidden -ffreestanding -fno-builtin -nostdlib \
             -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
             -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections
LDFLAGS   := -nostdlib -nostartfiles -pie -T plugin.ld \
             -Wl,-z,text -Wl,-z,now -Wl,-z,nopack-relative-relocs -Wl,--build-id=sha1 \
             -Wl,--gc-sections -Wl,--no-undefined-version -Wl,-Map,$(BUILD)/$(TARGET).map

.PHONY: all clean check bisect coexist

all: $(TARGET).elf

$(TARGET).elf: $(SOURCES) $(DEPS) Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD)/$(TARGET).full.elf $(SOURCES) -lgcc
	$(STRIP) --strip-debug -o $@ $(BUILD)/$(TARGET).full.elf
	@$(MAKE) --no-print-directory check

# SaltyNX resolves our imports from GLOB_DAT/ABS64/JUMP_SLOT relocations and runs the image
# base; we apply RELATIVE ones ourselves. RELR or TLS would not be handled, so refuse them.
check: $(TARGET).elf
	@if $(READELF) -d $(TARGET).elf | grep -q RELR; then echo "error: RELR relocations"; exit 1; fi
	@if $(READELF) -lW $(TARGET).elf | grep -q TLS; then echo "error: TLS segment"; exit 1; fi
	@$(READELF) -rW $(TARGET).elf | awk 'NR>3 && $$3 != "" && $$3 !~ /R_AARCH64_(RELATIVE|GLOB_DAT|ABS64|JUMP_SLOT)/ {print "error: relocation " $$3; bad=1} END {exit bad}'
	@echo "imports:"; $(READELF) -rW $(TARGET).elf | awk '$$3 ~ /GLOB_DAT|JUMP_SLOT|ABS64/ {print "  " $$5}' | sort -u
	@$(READELF) -lW $(TARGET).elf | awk '$$1=="LOAD" {f=$$7; if ($$8 ~ /^[RWE]+$$/) f=f $$8; printf "segment %s memsz %s flags %s\n", $$3, $$6, f}'

# One .elf per bisect level (see WP_LEVEL in source/plugin.c): L00 load only, L0..L4.
BISECT := $(BUILD)/bisect
bisect: $(SOURCES) $(DEPS) Makefile
	@mkdir -p $(BISECT)
	@for lv in -1 0 1 2 3 4; do \
	    name=L$$lv; [ $$lv = -1 ] && name=L00; \
	    $(CC) $(CFLAGS) -DWP_LEVEL=$$lv $(subst $(BUILD)/$(TARGET).map,$(BISECT)/$$name.map,$(LDFLAGS)) \
	        -o $(BISECT)/full-$$name.elf $(SOURCES) -lgcc || exit 1; \
	    $(STRIP) --strip-debug -o $(BISECT)/warp-shaders-$$name.elf $(BISECT)/full-$$name.elf || exit 1; \
	    rm -f $(BISECT)/full-$$name.elf; \
	    $(MAKE) --no-print-directory check TARGET=$(BISECT)/warp-shaders-$$name >/dev/null || exit 1; \
	    echo "$(BISECT)/warp-shaders-$$name.elf"; \
	done
	@# L0 variants: L0a/L0b use NX-FPS's LINKABLE import for the real loader, L0c is L0-quiet
	@# with the pointer logged at load, L0d avoids exclusive load/store in the hook, L0e stops
	@# with a breakpoint in the hook so the crash report shows its registers.
	@for v in "L0-quiet:-DWP_QUIET" "L0a:-DWP_QUIET -DWP_L0A" "L0b:-DWP_QUIET -DWP_L0B" \
	          "L0c:-DWP_QUIET" "L0d:-DWP_QUIET -DWP_PLAIN_COUNTER" "L0e:-DWP_QUIET -DWP_L0E"; do \
	    name=$${v%%:*}; flags=$${v#*:}; \
	    $(CC) $(CFLAGS) -DWP_LEVEL=0 $$flags $(subst $(BUILD)/$(TARGET).map,$(BISECT)/$$name.map,$(LDFLAGS)) \
	        -o $(BISECT)/warp-shaders-$$name.elf $(SOURCES) -lgcc || exit 1; \
	    $(STRIP) --strip-debug $(BISECT)/warp-shaders-$$name.elf || exit 1; \
	    $(MAKE) --no-print-directory check TARGET=$(BISECT)/warp-shaders-$$name >/dev/null || exit 1; \
	    echo "$(BISECT)/warp-shaders-$$name.elf"; \
	done

# Coexistence probes (source/coexist.c): C1 claims nvnBootstrapLoader with the real address and
# patches the game's relocations itself; C2 chains onto Core's built-in NX-FPS hook.
coexist: source/crt0.s source/coexist.c plugin.ld Makefile
	@mkdir -p $(BISECT)
	@for v in C1 C2; do \
	    $(CC) $(CFLAGS) -DWP_$$v -Wl,-Map,$(BISECT)/$$v.map $(filter-out -Wl$(comma)-Map$(comma)%,$(LDFLAGS)) \
	        -o $(BISECT)/warp-shaders-$$v.elf source/crt0.s source/coexist.c -lgcc || exit 1; \
	    $(STRIP) --strip-debug $(BISECT)/warp-shaders-$$v.elf || exit 1; \
	    $(MAKE) --no-print-directory check TARGET=$(BISECT)/warp-shaders-$$v >/dev/null || exit 1; \
	    echo "$(BISECT)/warp-shaders-$$v.elf"; \
	done

clean:
	rm -rf $(BUILD) $(TARGET).elf
