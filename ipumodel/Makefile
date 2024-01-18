
CXX ?= g++
POPC ?= popc
INCLUDES := -I$(PARENDI_ROOT)/include -I$(PARENDI_ROOT)/include/vltstd -I.

LIBS := -lpoplar -lpopops -lpoputil -lboost_program_options


JOBS ?= 30


IPU_SCHED_FLAGS :=                    \
	-X-mllvm -X--Ot 			      \
	-X-mllvm -X--enable-misched       \
	-X-mllvm -X--enable-post-misched  \
	-X-mllvm -X--pre-RA-sched=default \
	-X-mllvm -X--post-RA-scheduler    \
	-X-mllvm -X--verify-misched       \


IPU_FLAGS := -O3 $(INCLUDES) -Wno-parentheses-equality \
	-X-finline-functions \
	-X-finline-hint-functions \
	-X-fno-builtin-memset \
	-X-fno-builtin-memcpy \
	-X-funroll-loops \
	$(IPU_SCHED_FLAGS) \

all: profile.txt


runner: runner.cpp
	$(CXX) -O2 $^ $(LIBS)  -o $@

funcs/%.gp: funcs/%.cpp
	$(POPC) $^ $(IPU_FLAGS) --target ipu2 -o $@

funcs/%.s: funcs/%.cpp
	$(POPC) -S $^ $(IPU_FLAGS) --target ipu2 -o $@

funcs/%: funcs/%.gp


profile.txt: runner generateProfile.py
	python3 generateProfile.py -n 60 -j $(JOBS)

../src/V3BspIpuCostModelLinReg.h: profile.txt generateHeader.py
	python3 generateHeader.py
	clang-format -i $@


clean:
	rm -f funcs/*.cpp funcs/*.gp funcs/*.s
