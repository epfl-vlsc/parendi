
CXX ?= g++
POPC ?= popc
INCLUDES := -I$(VERIPOPLAR_ROOT)/include -I$(VERIPOPLAR_ROOT)/include/vltstd -I.

LIBS := -lpoplar -lpopops -lpoputil -lboost_program_options



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


runner: runner.cpp
	$(CXX) -O2 $^ $(LIBS)  -o $@

funcs/%.gp: funcs/%.cpp
	$(POPC) $^ $(IPU_FLAGS) --target ipu2 -o $@

funcs/%.s: funcs/%.cpp
	$(POPC) -S $^ $(IPU_FLAGS) --target ipu2 -o $@

funcs/%: funcs/%.gp

clean:
	rm -f funcs/*.cpp funcs/*.gp funcs/*.s