# Staged builds for hardware (see WP_LEVEL in source/plugin.c):
#   F1 = C1 bootstrap hook + our nvnDeviceGetProcAddress + present hook + flush thread
#   F1b = F1 plus diagnostics: brk.flag (stop on the first hook call) and diag.flag (a thread
#         that reports the hook counters, GOT slots and IPC results 30 s after boot)
#   F2 = the full plugin
# The bisect builds of the boot hang (L00-L4, L0a-L0e) are in git history (commit 86ce798).
OBJDUMP := $(PREFIX)objdump
STAGED  := $(BUILD)/staged

staged: $(SOURCES) $(DEPS) Makefile staged.mk
	@mkdir -p $(STAGED)
	@for v in "F1:2:" "F1b:2:-DWP_DIAG" "F2:4:"; do \
	    name=$${v%%:*}; rest=$${v#*:}; lv=$${rest%%:*}; extra=$${rest#*:}; \
	    $(CC) $(CFLAGS) -DWP_LEVEL=$$lv $$extra -DWP_NAME=\"$$name\" -Wl,-Map,$(STAGED)/$$name.map \
	        $(filter-out -Wl$(comma)-Map$(comma)%,$(LDFLAGS)) \
	        -o $(STAGED)/warp-shaders-$$name.elf $(SOURCES) -lgcc || exit 1; \
	    $(STRIP) --strip-debug $(STAGED)/warp-shaders-$$name.elf || exit 1; \
	    $(MAKE) --no-print-directory check TARGET=$(STAGED)/warp-shaders-$$name >/dev/null || exit 1; \
	    if $(OBJDUMP) -d $(STAGED)/warp-shaders-$$name.elf | grep -qE '(ldxr|ldaxr|stxr|stlxr|ldxp|stxp)[[:space:]]'; then \
	        echo "error: exclusive load/store in $$name"; exit 1; fi; \
	    echo "$(STAGED)/warp-shaders-$$name.elf"; \
	done
