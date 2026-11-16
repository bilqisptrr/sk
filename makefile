# Respect an explicitly selected compiler; otherwise prefer Clang when present.
ifeq ($(origin CXX),default)
    CXX := $(if $(shell command -v clang++ 2>/dev/null),clang++,g++)
endif
COMPILER_TYPE ?= $(if $(findstring clang,$(shell $(CXX) --version 2>/dev/null)),clang,gcc)

# Nama binary & source files
TARGET := SorterHunter
SRCS   := prefix_processor.cpp hutils.cpp SorterHunter.cpp ConfigParser.cpp
HEADERS:= htypes.h hutils.h prefix_processor.h ConfigParser.h timeout.h
OBJS   := $(SRCS:.cpp=.o)

# Argumen default untuk training run PGO (sesuaikan jika program butuh argumen spesifik)
PGO_RUN_ARGS ?= 
BASE_FLAGS := -O3 -DNDEBUG -Wall -Wextra -march=native -mtune=native -flto -funroll-loops -fomit-frame-pointer

# Optional tuning: benchmark on the target machine before enabling.
# -fno-plt changes dynamic binding behavior and is only supported here on Linux.
NO_PLT ?= 0
TUNE_FLAGS ?=
EXTRA_LDFLAGS ?=
ifeq ($(NO_PLT),1)
    ifneq ($(shell uname -s),Linux)
        $(error NO_PLT=1 is only supported on Linux)
    endif
    HAS_NO_PLT := $(shell printf 'int main() { return 0; }\n' | $(CXX) -Werror -fno-plt -x c++ -c -o /dev/null - >/dev/null 2>&1 && echo yes)
    ifneq ($(HAS_NO_PLT),yes)
        $(error NO_PLT=1 requested but $(CXX) does not support -fno-plt)
    endif
    BASE_FLAGS += -fno-plt
endif
BASE_FLAGS += $(TUNE_FLAGS)

# 3. Compiler-Specific Tuning (LLVM/Clang vs GCC)
ifeq ($(COMPILER_TYPE), clang)
	# Gunakan lld jika tersedia agar LTO jauh lebih cepat & optimal
	HAS_LLD := $(shell command -v ld.lld 2> /dev/null)
	ifdef HAS_LLD
		LDFLAGS += -fuse-ld=lld
	endif

	# -mllvm tuning untuk memperluas batas inlining dan loop unrolling pada hot path
	LLVM_INLINE_THRESHOLD ?= 1000
	LLVM_UNROLL_THRESHOLD ?= 300
	LLVM_EXTRA_FLAGS ?=
	LLVM_LTO_EXTRA_FLAGS ?=
	LLVM_FLAGS := -mllvm -inline-threshold=$(LLVM_INLINE_THRESHOLD) \
	              -mllvm -unroll-threshold=$(LLVM_UNROLL_THRESHOLD) $(LLVM_EXTRA_FLAGS)
	CXXFLAGS := $(BASE_FLAGS) $(LLVM_FLAGS)
	# Driver -mllvm flags are compile-only here; lld receives LTO flags below.
	LINK_CXXFLAGS = $(filter-out $(LLVM_FLAGS),$(CXXFLAGS))
	ifdef HAS_LLD
		# -mllvm on the driver affects compilation but is not forwarded to
		# the LTO backend. Forward the same settings explicitly to lld.
		LDFLAGS += -Wl,--mllvm=-inline-threshold=$(LLVM_INLINE_THRESHOLD) \
		           -Wl,--mllvm=-unroll-threshold=$(LLVM_UNROLL_THRESHOLD) $(LLVM_LTO_EXTRA_FLAGS)
	endif
	
	# PGO Flags untuk Clang
	PGO_GEN_FLAGS := -fprofile-instr-generate
	PGO_USE_FLAGS := -fprofile-instr-use=default.profdata
else
	# GCC Specific Tuning
	GCC_FLAGS := -fipa-pta -fdevirtualize-at-ltrans -fgcse-sm -fgcse-las
	CXXFLAGS  := $(BASE_FLAGS) $(GCC_FLAGS)
	LINK_CXXFLAGS = $(CXXFLAGS)
	
	# PGO Flags untuk GCC
	PGO_GEN_FLAGS := -fprofile-generate
	PGO_USE_FLAGS := -fprofile-use -fprofile-correction
endif

LLVM_PROFDATA ?= llvm-profdata
LLVM_BOLT ?= llvm-bolt
# Conservative layout-only defaults; benchmark on the deployment CPU.
BOLT_FLAGS ?= -reorder-blocks=ext-tsp -reorder-functions=hfsort+
CSPGO_BASE_RAW := cspgo-base.profraw
CSPGO_BASE_DATA := cspgo-base.profdata
CSPGO_RAW := cspgo.profraw
CSPGO_DATA := cspgo.profdata
BOLT_DATA := bolt.fdata

RM := rm -rf

.PHONY: all test test-clang-pgo pgo pgo-gen pgo-train pgo-use cspgo bolt cspgo-bolt bolt-optimize clean distclean

# Default build (tanpa PGO)
all: $(TARGET)

test: $(TARGET)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_LDFLAGS) -I. tests/layer_metrics.cpp hutils.cpp -o tests/layer_metrics
	./tests/layer_metrics
	python3 tests/timeout_test.py ./$(TARGET)
	python3 tests/webrtc_configs_test.py ./$(TARGET)

test-clang-pgo:
	python3 tests/clang_pgo_test.py "$(CXX)" "$(LLVM_PROFDATA)"

$(TARGET): $(OBJS)
	$(CXX) $(LINK_CXXFLAGS) $(LDFLAGS) $(EXTRA_LDFLAGS) -o $@ $(OBJS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

pgo:
	@echo "[1/4] Membersihkan build lama..."
	@$(MAKE) distclean
	@echo "[2/4] Compiling dengan PGO instrumentation ($(COMPILER_TYPE))..."
	@$(MAKE) pgo-gen
	@echo "[3/4] Menjalankan workload untuk mengumpulkan profil..."
	@$(MAKE) pgo-train
	@echo "[4/4] Recompiling dengan hasil optimasi PGO..."
	@$(MAKE) pgo-use
	@echo "Selesai! Binary $(TARGET) sudah teroptimasi penuh dengan PGO."

pgo-gen:
	@$(MAKE) clean
	$(MAKE) $(TARGET) CXXFLAGS="$(CXXFLAGS) $(PGO_GEN_FLAGS)" LDFLAGS="$(LDFLAGS) $(PGO_GEN_FLAGS)"

pgo-train:
ifeq ($(COMPILER_TYPE), clang)
	LLVM_PROFILE_FILE="$(CURDIR)/default.profraw" ./$(TARGET) $(PGO_RUN_ARGS)
	$(LLVM_PROFDATA) merge -output=default.profdata default.profraw
else
	./$(TARGET) $(PGO_RUN_ARGS)
endif

pgo-use:
ifeq ($(COMPILER_TYPE), clang)
	@set -e; \
	if [ -f default.profraw ] && { [ ! -f default.profdata ] || [ default.profraw -nt default.profdata ]; }; then \
		command -v $(LLVM_PROFDATA) >/dev/null || { echo "Error: $(LLVM_PROFDATA) not found; install the LLVM tools matching Clang or set LLVM_PROFDATA." >&2; exit 1; }; \
		echo "Merging default.profraw into default.profdata..."; \
		$(LLVM_PROFDATA) merge -o default.profdata.tmp default.profraw || { rm -f default.profdata.tmp; exit 1; }; \
		mv -f default.profdata.tmp default.profdata; \
	elif [ ! -s default.profdata ]; then \
		echo "Error: no Clang PGO profile found. Run make pgo-train (or run the instrumented binary normally) first." >&2; \
		exit 1; \
	fi
endif
	@$(MAKE) clean
	$(MAKE) $(TARGET) CXXFLAGS="$(CXXFLAGS) $(PGO_USE_FLAGS)" LDFLAGS="$(LDFLAGS) $(PGO_USE_FLAGS)"

# Context-sensitive IR-PGO requires two distinct instrumented builds. The
# first profile guides inlining in the CS-instrumented build; the final profile
# merges both stages. All training runs must terminate normally to flush data.
cspgo:
ifeq ($(COMPILER_TYPE),clang)
	@test -n "$(strip $(PGO_RUN_ARGS))" || { echo "Set PGO_RUN_ARGS, e.g. 'sample_config.txt --timeout 30'" >&2; exit 1; }
	@command -v $(LLVM_PROFDATA) >/dev/null || { echo "Missing $(LLVM_PROFDATA) (use the version matching Clang)" >&2; exit 1; }
	@$(MAKE) clean
	$(RM) $(CSPGO_BASE_RAW) $(CSPGO_BASE_DATA) $(CSPGO_RAW) $(CSPGO_DATA)
	$(MAKE) $(TARGET) CXXFLAGS="$(CXXFLAGS) -fprofile-generate" LDFLAGS="$(LDFLAGS) -fprofile-generate"
	LLVM_PROFILE_FILE="$(CURDIR)/$(CSPGO_BASE_RAW)" ./$(TARGET) $(PGO_RUN_ARGS)
	$(LLVM_PROFDATA) merge -o $(CSPGO_BASE_DATA) $(CSPGO_BASE_RAW)
	@$(MAKE) clean
	$(MAKE) $(TARGET) CXXFLAGS="$(CXXFLAGS) -fprofile-use=$(CSPGO_BASE_DATA) -fcs-profile-generate" LDFLAGS="$(LDFLAGS) -fprofile-use=$(CSPGO_BASE_DATA) -fcs-profile-generate"
	LLVM_PROFILE_FILE="$(CURDIR)/$(CSPGO_RAW)" ./$(TARGET) $(PGO_RUN_ARGS)
	$(LLVM_PROFDATA) merge -o $(CSPGO_DATA) $(CSPGO_BASE_DATA) $(CSPGO_RAW)
	@$(MAKE) clean
	$(MAKE) $(TARGET) CXXFLAGS="$(CXXFLAGS) -fprofile-use=$(CSPGO_DATA)" LDFLAGS="$(LDFLAGS) -fprofile-use=$(CSPGO_DATA)"
else
	@echo "cspgo requires Clang and matching llvm-profdata" >&2; exit 1
endif

# BOLT instrumentation avoids requiring perf/LBR. Keep the original binary and
# write a separate .bolt output, so a failed optimization cannot replace it.
bolt:
ifeq ($(COMPILER_TYPE),clang)
	@test -n "$(strip $(PGO_RUN_ARGS))" || { echo "Set PGO_RUN_ARGS, e.g. 'sample_config.txt --timeout 30'" >&2; exit 1; }
	@command -v $(LLVM_BOLT) >/dev/null || { echo "Missing $(LLVM_BOLT) and its instrumentation runtime" >&2; exit 1; }
	@test "$$(uname -s)" = Linux || { echo "BOLT workflow requires Linux ELF" >&2; exit 1; }
	@$(MAKE) clean
	$(MAKE) $(TARGET) EXTRA_LDFLAGS="$(EXTRA_LDFLAGS) -Wl,--emit-relocs"
	$(MAKE) bolt-optimize
else
	@echo "bolt requires Clang and llvm-bolt" >&2; exit 1
endif

cspgo-bolt:
ifeq ($(COMPILER_TYPE),clang)
	@command -v $(LLVM_BOLT) >/dev/null || { echo "Missing $(LLVM_BOLT) and its instrumentation runtime" >&2; exit 1; }
	@test "$$(uname -s)" = Linux || { echo "BOLT workflow requires Linux ELF" >&2; exit 1; }
	$(MAKE) cspgo EXTRA_LDFLAGS="$(EXTRA_LDFLAGS) -Wl,--emit-relocs"
	$(MAKE) bolt-optimize
else
	@echo "cspgo-bolt requires Clang, llvm-profdata, and llvm-bolt" >&2; exit 1
endif

bolt-optimize:
	@test -n "$(strip $(PGO_RUN_ARGS))" || { echo "Set PGO_RUN_ARGS for BOLT training" >&2; exit 1; }
	@command -v $(LLVM_BOLT) >/dev/null || { echo "Missing $(LLVM_BOLT)" >&2; exit 1; }
	@test -x ./$(TARGET) || { echo "Missing $(TARGET); build with -Wl,--emit-relocs first" >&2; exit 1; }
	$(RM) $(TARGET).bolt.inst $(TARGET).bolt $(BOLT_DATA)
	$(LLVM_BOLT) ./$(TARGET) -instrument -instrumentation-file="$(CURDIR)/$(BOLT_DATA)" -o $(TARGET).bolt.inst
	./$(TARGET).bolt.inst $(PGO_RUN_ARGS)
	@test -s $(BOLT_DATA) || { echo "BOLT profile is empty or missing; training must exit normally" >&2; exit 1; }
	$(LLVM_BOLT) ./$(TARGET) -data=$(BOLT_DATA) $(BOLT_FLAGS) -o $(TARGET).bolt
	@echo "BOLT output: ./$(TARGET).bolt (original: ./$(TARGET))"

# Bersihkan binary & object files
clean:
	-$(RM) $(TARGET) $(OBJS) tests/layer_metrics $(TARGET).bolt.inst $(TARGET).bolt

# Bersihkan semuanya termasuk file profil PGO
distclean: clean
	-$(RM) default.profraw default.profdata *.gcda *.gcno $(CSPGO_BASE_RAW) $(CSPGO_BASE_DATA) $(CSPGO_RAW) $(CSPGO_DATA) $(BOLT_DATA)
