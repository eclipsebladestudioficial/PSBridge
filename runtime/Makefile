BUILD_DIR = ../build
RUNTIME_BUILD = $(BUILD_DIR)/runtime
MAIN_OBJ = $(RUNTIME_BUILD)/main.o
CORE_OBJS = $(addprefix $(RUNTIME_BUILD)/core/,imports.o guest.o vfpu_entry.o)
API_OBJS = $(addprefix $(RUNTIME_BUILD)/runtime/,audio.o controller.o ge.o ge3d.o graphics.o video.o io.o memory.o threadman.o lwmutex.o sync.o utility.o vfpu.o)
VFPU_OBJS = $(addprefix $(RUNTIME_BUILD)/runtime/vfpu/,state.o memory.o operations.o)
RUNTIME_OBJS = $(MAIN_OBJ) $(CORE_OBJS) $(API_OBJS) $(VFPU_OBJS)

EE_BIN = $(BUILD_DIR)/PSBridge-debug.elf
STRIPPED_BIN = $(BUILD_DIR)/PSBridge-stripped.elf
EE_OBJS = $(RUNTIME_OBJS) $(BUILD_DIR)/game.o
EE_LIBS = -L$(GSKIT)/lib -lgskit -ldmakit -ldebug -lm -lc -lpad
EE_INCS += -I$(CURDIR)/include -I$(GSKIT)/include
EE_LINKFILE = $(CURDIR)/linker/guest.ld
EE_ASFLAGS += -I$(abspath ..)
EE_CFLAGS += -MMD -MP

.PHONY: all clean

all: $(EE_BIN) $(STRIPPED_BIN)

-include $(RUNTIME_OBJS:.o=.d)

$(EE_BIN): linker/guest.ld

$(STRIPPED_BIN): $(EE_BIN)
	cp $< $@
	$(EE_STRIP) --strip-all $@

$(BUILD_DIR)/game.s: ../jogo.elf $(wildcard ../tools/*.py)
	cd .. && python3 tools/recompile.py jogo.elf build/game.s

$(BUILD_DIR)/game.o: $(BUILD_DIR)/game.s
	cd .. && $(EE_AS) $(EE_ASFLAGS) -call_nonpic -msingle-float build/game.s -o build/game.o

$(RUNTIME_BUILD)/main.o: main.c
	$(DIR_GUARD)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(RUNTIME_BUILD)/core/%.o: core/%.c
	$(DIR_GUARD)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(RUNTIME_BUILD)/core/guest.o: core/guest.S
	$(DIR_GUARD)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(RUNTIME_BUILD)/core/vfpu_entry.o: core/vfpu_entry.S
	$(DIR_GUARD)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(RUNTIME_BUILD)/runtime/%.o: runtime/%.c
	$(DIR_GUARD)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(RUNTIME_BUILD)/runtime/vfpu/%.o: runtime/vfpu/%.c
	$(DIR_GUARD)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

clean:
	rm -f $(EE_BIN) $(STRIPPED_BIN) $(BUILD_DIR)/psbridge.elf $(RUNTIME_OBJS) $(RUNTIME_BUILD)/runtime/vfpu/*.o $(RUNTIME_OBJS:.o=.d) $(BUILD_DIR)/game.o $(BUILD_DIR)/game.s $(BUILD_DIR)/game.bin $(BUILD_DIR)/game.map

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
