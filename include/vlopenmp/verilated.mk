# The Apple compilers do not implement OMP!
CXX = /opt/homebrew/bin/g++-13
INCLUDES := -I$(PARENDI_ROOT)/include/vlopenmp -I$(PARENDI_ROOT)/include -I$(PARENDI_ROOT)/include/vltstd -I/opt/homebrew/include/ -I.
LIBS :=

HOST_DEFINES =  \
  -DOPENMP \
	-DVPROGRAM="$(VMAIN_ROOT)" \
	-DVPROGRAM_HEADER="\"$(VMAIN_ROOT).h\"" \
	-DROOT_NAME="\"$(VMAIN_ROOT)\"" \
	-DCODELET_LIST="\"$(VMAIN_ROOT).list\"" \
	-DOBJ_DIR="\"$(OBJ_DIR)\"" \
	-DVL_VERILATED_INCLUDE="\"vlopenmp/verilated_openmp.h\""

HOST_FLAGS = --std=c++17 -O3 -fopenmp $(INCLUDES) $(HOST_DEFINES) -Wno-parentheses-equality

VERILATOR_CPP =  \
	$(PARENDI_ROOT)/include/verilated.cpp \
	$(PARENDI_ROOT)/include/verilated_threads.cpp \
	$(PARENDI_ROOT)/include/vlopenmp/verilated_openmp_context.cpp

HOST_SOURCES += $(USER_CPP)
HOST_SOURCES += $(CODELETS)
HOST_OBJS = $(HOST_SOURCES:cpp=o)


all: $(VMAIN)

$(HOST_OBJS):%.o: %.cpp
	$(CXX) $^ -c $(HOST_FLAGS) $(LIBS) -o $@

$(VMAIN): $(HOST_OBJS) $(VERILATOR_CPP)
	$(CXX) $(HOST_FLAGS) $(HOST_OBJS) $(VERILATOR_CPP) $(LIBS) -o $@

clean:
	rm -rf *.o *.gp *.s report *.graph.bin $(VMAIN) $(VMAIN).dSYM
