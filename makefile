HAS_CLANG := $(shell command -v clang++ 2> /dev/null)
ifdef HAS_CLANG
	CXX := clang++
	COMPILER_TYPE := clang
else
	CXX := g++
	COMPILER_TYPE := gcc
endif

# Nama binary & source files
TARGET := SorterHunter
SRCS   := prefix_processor.cpp hutils.cpp SorterHunter.cpp ConfigParser.cpp
HEADERS:= htypes.h
OBJS   := $(SRCS:.cpp=.o)

# Argumen default untuk training run PGO (sesuaikan jika program butuh argumen spesifik)
PGO_RUN_ARGS ?= 
BASE_FLAGS := -O3 -DNDEBUG -Wall -Wextra -march=native -mtune=native -flto -funroll-loops -fomit-frame-pointer

# 3. Compiler-Specific Tuning (LLVM/Clang vs GCC)
ifeq ($(COMPILER_TYPE), clang)
	# Gunakan lld jika tersedia agar LTO jauh lebih cepat & optimal
	HAS_LLD := $(shell command -v ld.lld 2> /dev/null)
	ifdef HAS_LLD
		LDFLAGS += -fuse-ld=lld
	endif

	# -mllvm tuning untuk memperluas batas inlining dan loop unrolling pada hot path
	LLVM_FLAGS := -mllvm -inline-threshold=1000 \
	              -mllvm -unroll-threshold=300
	CXXFLAGS := $(BASE_FLAGS) $(LLVM_FLAGS)
	
	# PGO Flags untuk Clang
	PGO_GEN_FLAGS := -fprofile-instr-generate
	PGO_USE_FLAGS := -fprofile-instr-use=default.profdata
else
	# GCC Specific Tuning
	GCC_FLAGS := -fipa-pta -fdevirtualize-at-ltrans -fgcse-sm -fgcse-las
	CXXFLAGS  := $(BASE_FLAGS) $(GCC_FLAGS)
	
	# PGO Flags untuk GCC
	PGO_GEN_FLAGS := -fprofile-generate
	PGO_USE_FLAGS := -fprofile-use -fprofile-correction
endif

RM := rm -rf

.PHONY: all pgo pgo-gen pgo-train pgo-use clean distclean

# Default build (tanpa PGO)
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS)

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
	$(MAKE) $(TARGET) CXXFLAGS="$(CXXFLAGS) $(PGO_GEN_FLAGS)" LDFLAGS="$(LDFLAGS) $(PGO_GEN_FLAGS)"

pgo-train:
	./$(TARGET) $(PGO_RUN_ARGS)
ifeq ($(COMPILER_TYPE), clang)
	llvm-profdata merge -output=default.profdata default.profraw
endif

pgo-use:
	@$(MAKE) clean
	$(MAKE) $(TARGET) CXXFLAGS="$(CXXFLAGS) $(PGO_USE_FLAGS)" LDFLAGS="$(LDFLAGS) $(PGO_USE_FLAGS)"

# Bersihkan binary & object files
clean:
	-$(RM) $(TARGET) $(OBJS)

# Bersihkan semuanya termasuk file profil PGO
distclean: clean
	-$(RM) default.profraw default.profdata *.gcda *.gcno
