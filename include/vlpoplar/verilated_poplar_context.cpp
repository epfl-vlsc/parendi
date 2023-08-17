
#include VPROGRAM_HEADER /*defined by the Makefile*/

#include "verilated_poplar_context.h"

#include "verilated.h"

#include <chrono>
#include <fstream>
#include <iomanip>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <poplar/CycleCount.hpp>
#ifndef VL_NUM_TILES_USED
#error "VL_NUM_TILES_USED is no defined!"
#endif
#ifndef VL_NUM_WORKERS_USED
#error "VL_NUM_WORKERS_USED is not defined!"
#endif
#ifndef VL_IPU_TRACE_BUFFER_SIZE
#error "VL_IPU_TRACE_BUFFER_SIZE is not defined!"
#endif

void VlPoplarContext::init(int argc, char* argv[]) {

    // get the first available device

    uint32_t requiredNumIpus = 0;
    constexpr uint32_t MAX_TILES_PER_IPU = 1472;
    if (VL_NUM_TILES_USED <= MAX_TILES_PER_IPU * 1) {
        requiredNumIpus = 1;
    } else if (VL_NUM_TILES_USED <= MAX_TILES_PER_IPU * 2) {
        requiredNumIpus = 2;
    } else if (VL_NUM_TILES_USED <= MAX_TILES_PER_IPU * 3) {
        requiredNumIpus = 3;
    } else if (VL_NUM_TILES_USED <= MAX_TILES_PER_IPU * 4) {
        requiredNumIpus = 4;
    } else {
        std::cerr << "Can not have more than 4 IPUs, max. number of tiles is "
                  << MAX_TILES_PER_IPU * 4 << " but requested " << VL_NUM_TILES_USED << std::endl;
        std::exit(-1);
    }

#ifdef GRAPH_RUN
    // for +args
    Verilated::commandArgs(argc, argv);
#endif
    auto manager = poplar::DeviceManager::createDeviceManager();

    auto devices = manager.getDevices(poplar::TargetType::IPU, requiredNumIpus);

    auto devIt = std::find_if(devices.begin(), devices.end(),
                              [](poplar::Device& dev) { return dev.attach(); });
    if (devIt == devices.end()) {
        std::cerr << "Failed to attache to an IPU" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    device = std::make_unique<poplar::Device>(std::move(*devIt));
    vprog = std::make_unique<VPROGRAM>(*this);
    graph = std::make_unique<poplar::Graph>(device->getTarget());
    workload = std::make_unique<poplar::ComputeSet>(graph->addComputeSet("workload"));
    initializer = std::make_unique<poplar::ComputeSet>(graph->addComputeSet("initializer"));
    condeval = std::make_unique<poplar::ComputeSet>(graph->addComputeSet("condeval"));
#ifdef GRAPH_COMPILE
#ifdef GRAPH_RUN
    std::ifstream fs{OBJ_DIR "/" CODELET_LIST /*defined by the Makefile*/, std::ios::in};
#else
    std::ifstream fs{CODELET_LIST /*defined by the Makefile*/, std::ios::in};
#endif
    for (std::string ln; std::getline(fs, ln);) {
        std::cout << "adding codelet " << ln << std::endl;
#ifdef GRAPH_RUN
        graph->addCodelets(std::string{OBJ_DIR "/"} + ln);
#else
        graph->addCodelets(ln);
#endif
    }
#endif
    // std::cout << "initializing simulation context " << std::endl;
    vprog->constructAll();
    vprog->initialize();
    vprog->exchange();
    vprog->dpiExchange();
    vprog->dpiBroadcast();
}

void VlPoplarContext::buildReEntrant() {
    using namespace poplar;
    using namespace poplar::program;

    if (!interruptCond.valid()) {
        std::cerr << "Program has no host interface/stop condition!" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    Sequence resetProg;
    auto zeroValue = graph->addConstant(UNSIGNED_INT, {1}, false, "false value");
    graph->setTileMapping(zeroValue, 0);
    resetProg.add(Copy(zeroValue, interruptCond[0]));  // need to clear the lo
    for (auto hreq : hostRequest) { resetProg.add(Copy(zeroValue, hreq[0])); }

    Sequence initProg;
    if (hasInit) {
        initProg.add(Execute(*initializer));
        initProg.add(dpiCopies);
        initProg.add(Execute{*condeval});
        initProg.add(dpiBroadcastCopies);
    }

#ifdef VL_INSTRUMENT
    struct TsHandles {
        const std::string name;
        Tensor ovf;
        Tensor buffer;
        Tensor count;
        Program program;
        std::unique_ptr<Call> m_callback;
        const std::vector<Tensor> cbTensors;
        const std::vector<std::tuple<Type, std::size_t>> argSizes;
        explicit TsHandles(const std::unique_ptr<Graph>& graph, const std::string& name,
                           const ComputeSet& cs, const Tensor& ovf, const Tensor& buffer,
                           const Tensor& count)
            : name(name)
            , ovf{ovf}
            , buffer{buffer}
            , count{count} {
            program = Sequence{Execute{cs}};
            auto hfunc = graph->addHostFunction(
                "cb_" + name,
                {{UNSIGNED_INT, 1 + VL_NUM_TILES_USED * VL_IPU_TRACE_BUFFER_SIZE * 2}}, {});
            Tensor arg = concat(count, buffer.flatten());
            m_callback = std::make_unique<Call>(hfunc, poplar::ArrayRef<Tensor>{arg},
                                                poplar::ArrayRef<Tensor>{});
        }
        Call& callback() const { return *m_callback; }
    };

    graph->addCodelets("VlTimeStamp.gp");
    auto createTsProgram = [&](const std::string& name) {
        auto tsSet = graph->addComputeSet(name);
        std::vector<std::tuple<Type, std::size_t>> argSizes;
        std::vector<Tensor> cbTensors;
        cbTensors.clear();
        argSizes.clear();
        Tensor ocount;
        Tensor countTotal;

        Tensor buffer = graph->addVariable(
            UNSIGNED_INT, {VL_NUM_TILES_USED, VL_IPU_TRACE_BUFFER_SIZE * 2}, name + "::buffer");

        for (int tid = 0; tid < VL_NUM_TILES_USED; tid++) {
            auto vtx = graph->addVertex(tsSet, "VlTimeStamp");

            auto totCount
                = graph->addVariable(UNSIGNED_INT, {1}, name + "::totCount" + std::to_string(tid));
            auto ovf
                = graph->addVariable(UNSIGNED_INT, {1}, name + "::overflow" + std::to_string(tid));
            std::vector<uint32_t> zeros;
            zeros.resize(VL_IPU_TRACE_BUFFER_SIZE * 2);
            std::fill(zeros.begin(), zeros.end(), 0ull);
            graph->setInitialValue(buffer[tid], poplar::ArrayRef{zeros});
            graph->setInitialValue(ovf, 0u);
            graph->setInitialValue(totCount, 0u);
            graph->setTileMapping(vtx, tid);
            graph->setTileMapping(buffer[tid], tid);
            graph->setTileMapping(ovf, tid);
            graph->setTileMapping(totCount, tid);
            graph->connect(vtx["buffer"], buffer[tid]);
            graph->connect(vtx["totCount"], totCount);
            graph->connect(vtx["overflow"], ovf);

            if (tid == 0) {
                ocount = ovf;
                countTotal = totCount;
            }
        }

        return std::make_unique<TsHandles>(graph, name, tsSet, ocount, buffer, countTotal);
    };

    auto tsPreExchange = createTsProgram("preExchange");
    auto tsPreWorkload = createTsProgram("preWorkload");
    auto tsPostWorkload = createTsProgram("postWorkload");
    // Tensor zeroOutBuffer
    //     = graph->addConstant(UNSIGNED_INT, {VL_NUM_TILES_USED, VL_IPU_TRACE_BUFFER_SIZE * 2},
    //     0u);
    // graph->setTileMapping(zeroOutBuffer, 0);
    // initCopies.add(Copy(zeroOutBuffer, tsPreExchange->buffer));
    // initCopies.add(Copy(zeroOutBuffer, tsPreWorkload->buffer));
    // initCopies.add(Copy(zeroOutBuffer, tsPostWorkload->buffer));
    {
        std::vector<unsigned> tileSet;
        for (int tid = 0; tid < VL_NUM_TILES_USED; tid++) { tileSet.push_back(tid); }
        std::vector<Tensor> initTs
            = cycleStamp(*graph, initCopies, tileSet, SyncType::EXTERNAL, "initTs");
        Tensor initTsAll = concat(initTs);
        auto hfunc
            = graph->addHostFunction("cb_initTs", {{UNSIGNED_INT, VL_NUM_TILES_USED * 2}}, {});
        initCopies.add(Call(hfunc, {initTsAll}, {}));
    }
#endif

    // clang-format off
    Sequence simLoop {
        dpiBroadcastCopies
        , Execute{*workload}
        , RepeatWhileFalse{
            Sequence{
                dpiCopies
                , Execute{*condeval}
            },
            interruptCond[0],
            Sequence{
#ifdef VL_INSTRUMENT
                // timestamp
                tsPreExchange->program,
#endif
                exchangeCopies
#ifdef VL_INSTRUMENT
                // timestamp
                , tsPreWorkload->program
#endif
                , Execute{*workload}
#ifdef VL_INSTRUMENT
                , tsPostWorkload->program,
                If {
                    tsPostWorkload->ovf[0],
                    Sequence{
                        tsPreExchange->callback(),
                        tsPreWorkload->callback(),
                        tsPostWorkload->callback()
                    },
                    Sequence{}
                }
#endif
            }
        }
#ifdef VL_INSTRUMENT
        , Sequence{
            tsPreExchange->callback(),
            tsPreWorkload->callback(),
            tsPostWorkload->callback()
        }
#endif
    };
    Sequence nbaProg {
        Execute{*condeval},
        dpiBroadcastCopies,
        If{
            interruptCond[0],
            Sequence{
                Execute{*workload},
                dpiCopies,
                Execute{*condeval},
                If {
                    interruptCond[0],
                    Sequence{},
                    exchangeCopies,
                }
            },
            Sequence{}
        },
        If{
            interruptCond[0],
            Sequence{},
            simLoop
        }
    };

    std::vector<Program> programs {
        resetProg,
        initProg,
        initCopies,
        nbaProg
    };
    // clang-format on
    OptionFlags flags{};
#ifdef POPLAR_INSTRUMENT
    {
#ifdef GRAPH_RUN
        std::ifstream ifs(OBJ_DIR "/" ROOT_NAME "_engine_options.json", std::ios::in);
#else
        std::ifstream ifs(ROOT_NAME "_compile_options.json", std::ios::in);
#endif
        poplar::readJSON(ifs, flags);
        ifs.close();
        std::cout << flags << std::endl;
    }
#endif
    float lastProg = 0.0;
    exec = std::make_unique<Executable>(
        compileGraph(*graph, programs, flags, [&lastProg](int step, int total) {
            float newProg = static_cast<float>(step) / static_cast<float>(total) * 100.0;
            if (newProg - lastProg >= 10.0f) {
                std::cout << "Graph compilation: " << static_cast<int>(newProg) << "%"
                          << std::endl;
                lastProg = newProg;
            }
        }));

#ifndef GRAPH_RUN
    std::ofstream execOut(ROOT_NAME ".graph.bin", std::ios::binary);
    exec->serialize(execOut);
#endif
}

void VlPoplarContext::runReEntrant() {
    std::ofstream profile(OBJ_DIR "/" ROOT_NAME "_runtime.log", std::ios::out);
    const auto simStartTime = std::chrono::high_resolution_clock::now();
    auto timed = [](auto&& lazyValue) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        lazyValue();
        const auto t1 = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double>(t1 - t0).count();
    };

    auto measure = [&timed, &profile](auto&& lazyValue, const std::string& n) {
        const auto t = timed(std::forward<decltype(lazyValue)>(lazyValue));
        profile << n << ": " << std::fixed << std::setw(15) << std::setprecision(6) << t << "s"
                << std::endl;
    };
    // build the engined and copy cached values of arguments and files to the device
    measure(
        [this]() {
#ifndef GRAPH_COMPILE
            std::ifstream graphIn(OBJ_DIR "/" ROOT_NAME ".graph.bin", std::ios::binary);
            exec = std::make_unique<poplar::Executable>(poplar::Executable::deserialize(graphIn));
            graphIn.close();
#endif
            poplar::OptionFlags flags{};
#ifdef POPLAR_INSTRUMENT
            {
                std::ifstream ifs(OBJ_DIR "/" ROOT_NAME "_engine_options.json", std::ios::in);
                poplar::readJSON(ifs, flags);
                ifs.close();
                std::cout << flags << std::endl;
            }
#endif
            engine = std::make_unique<poplar::Engine>(*exec, flags);
            engine->load(*device);
            vprog->plusArgs();
            vprog->plusArgsCopy();
            vprog->readMem();
            vprog->readMemCopy();
            engine->run(E_RESET);
        },
        "load");
    // the initializings is performed in a loop, because the there may be DPI
    // call in the init program
    int invIndex = 0;
    uint32_t interrupt = 0;
#ifdef VL_INSTRUMENT
    struct TimeTraceDump {
        uint32_t lastCount = 0;
        std::ofstream ofs;
        const std::string name;
        std::mutex m_mutex;
        TimeTraceDump(const std::string& name)
            : name{name} {
            ofs = std::ofstream{std::string{OBJ_DIR} + "/" + name + ".txt", std::ios::out};
        }
    };
    std::vector<uint64_t> initTimeStamps;

    TimeTraceDump preExchange{"preExchange"};
    TimeTraceDump preWorkload{"preWorkload"};
    TimeTraceDump postWorkload{"postWorkload"};
    TimeTraceDump initTsDump{"initTs"};

    auto attachCallback = [&](TimeTraceDump& dump) {
        engine->connectHostFunction(
            "cb_" + dump.name, 0,
            [&](poplar::ArrayRef<const void*> ins, poplar::ArrayRef<void*> /*unused*/) -> void {
                // std::cout << "dumping " << dump.name << std::endl;
                std::lock_guard<std::mutex> lk{dump.m_mutex};
                const uint32_t* argp = reinterpret_cast<const uint32_t*>(ins[0]);
                const uint32_t count = argp[0];
                const uint64_t* bufferp = reinterpret_cast<const uint64_t*>(argp + 1);

                while (dump.lastCount < count) {
                    int j = dump.lastCount % VL_IPU_TRACE_BUFFER_SIZE;
                    for (int i = 0; i < VL_NUM_TILES_USED; i++) {
                        uint64_t vl = bufferp[i * VL_IPU_TRACE_BUFFER_SIZE + j];
                        if (!vl) {
                            std::cout << "got zero in " << dump.name << " tile " << i << "pos "
                                      << j << std::endl;
                        }
                        // uint64_t its = initTimeStamps[i];

                        dump.ofs << vl << "    ";
                    }
                    dump.ofs << std::endl;
                    dump.lastCount = dump.lastCount + 1;
                }
            });
    };
    attachCallback(preExchange);
    attachCallback(preWorkload);
    attachCallback(postWorkload);
    engine->connectHostFunction(
        "cb_initTs", 0,
        [&](poplar::ArrayRef<const void*> ins, poplar::ArrayRef<void*> /*unused*/) -> void {
            std::lock_guard<std::mutex> lk{initTsDump.m_mutex};
            const uint64_t* tsp = reinterpret_cast<const uint64_t*>(ins[0]);
            for (int tid = 0; tid < VL_NUM_TILES_USED; tid++) {
                initTsDump.ofs << tsp[tid] << " ";
            }
            initTsDump.ofs << std::endl;
        });
#endif
    do {
        profile << "init " << invIndex << std::endl;
        engine->run(E_INIT);
        vprog->hostHandle();
        interrupt = getHostData<uint32_t>("interrupt");
    } while (interrupt && !Verilated::gotFinish());
    engine->run(E_INITCOPY);

    // starting the main simulation loop
    auto simLoopStart = std::chrono::high_resolution_clock::now();
    while (!Verilated::gotFinish()) {
        profile << "run " << invIndex++ << std::endl;
        measure([this]() { engine->run(E_NBA); }, "\twall");
        vprog->hostHandle();
    }

    auto simEnd = std::chrono::high_resolution_clock::now();
    profile << "sim: " << std::chrono::duration<double>(simEnd - simLoopStart).count() << "s"
            << std::endl;
    profile << "all: " << std::chrono::duration<double>(simEnd - simStartTime).count() << "s"
            << std::endl;
    // dumpCycleTrace(profile);
    profile.close();
#ifdef VL_INSTRUMENT
    preExchange.ofs.close();
    preWorkload.ofs.close();
    postWorkload.ofs.close();
    initTsDump.ofs.close();

#endif
}

void VlPoplarContext::addCopy(const std::string& from, const std::string& to, uint32_t size,
                              const std::string& kind) {
#ifdef GRAPH_COMPILE
    poplar::Tensor fromTensor = getTensor(from);
    poplar::Tensor toTensor = getTensor(to);
    poplar::program::Copy cp{fromTensor, toTensor, true, from + " ==> " + to};
    if (kind == "initialize") {
        initCopies.add(cp);
    } else if (kind == "exchange") {
        exchangeCopies.add(cp);
    } else if (kind == "dpiExchange") {
        dpiCopies.add(cp);
    } else if (kind == "dpiBroadcast") {
        dpiBroadcastCopies.add(cp);
    } else {
        std::cerr << "invalid copy operation \"" << kind << "\"\n";
        std::exit(EXIT_FAILURE);
    }
#endif
}
poplar::Tensor VlPoplarContext::addTensor(uint32_t size, const std::string& name) {
#ifdef GRAPH_COMPILE
    poplar::Tensor t = graph->addVariable(
        poplar::UNSIGNED_INT,
        {std::max(size, 2u) /*pad single-word tensors to 8 bytes to optimize on-tile copies*/},
        name);
    if (size > 1) {
        std::vector<uint32_t> zeros(size);
        graph->setInitialValue(t, poplar::ArrayRef(zeros));
    } else {
        graph->setInitialValue(t, 0u);
    }
    tensors.emplace(name, t);
    return t;
#else
    return poplar::Tensor{};
#endif
}

poplar::VertexRef VlPoplarContext::getOrAddVertex(const std::string& name,
                                                  const std::string& where) {
#ifdef GRAPH_COMPILE
    auto it = vertices.find(name);
    if (it == vertices.end()) {
        if (where == "compute") {
            hasCompute = true;
            vertices.emplace(name, graph->addVertex(*workload, name));
        } else if (where == "init") {
            hasInit = true;
            vertices.emplace(name, graph->addVertex(*initializer, name));
        } else if (where == "condeval") {
            hasCond = true;
            vertices.emplace(name, graph->addVertex(*condeval, name));
        } else {
            std::cerr << "invalid computeset \"" << where << "\"" << std::endl;
            std::exit(EXIT_FAILURE);
        }
    }
    return vertices[name];
#else
    return poplar::VertexRef{};
#endif
}
void VlPoplarContext::setTileMapping(poplar::VertexRef& vtxRef, uint32_t tileId) {
#ifdef GRAPH_COMPILE
    graph->setTileMapping(vtxRef, tileId);
#endif
}
void VlPoplarContext::setTileMapping(poplar::Tensor& tensor, uint32_t tileId) {
#ifdef GRAPH_COMPILE
    graph->setTileMapping(tensor, tileId);
#endif
}
void VlPoplarContext::connect(poplar::VertexRef& vtx, const std::string& field,
                              poplar::Tensor& tensor) {
#ifdef GRAPH_COMPILE
    graph->connect(vtx[field], tensor);
#endif
}
void VlPoplarContext::createHostRead(const std::string& handle, poplar::Tensor& tensor,
                                     uint32_t numElems) {
#ifdef GRAPH_COMPILE
    graph->createHostRead(handle, tensor);
#endif
    hbuffers.emplace(handle, std::make_unique<HostBuffer>(numElems));
}
void VlPoplarContext::createHostWrite(const std::string& handle, poplar::Tensor& tensor,
                                      uint32_t numElems) {
#ifdef GRAPH_COMPILE
    graph->createHostWrite(handle, tensor);
#endif
    hbuffers.emplace(handle, std::make_unique<HostBuffer>(numElems));
}

void VlPoplarContext::isHostRequest(poplar::Tensor& tensor, bool isInterruptCond) {
#ifdef GRAPH_COMPILE
    if (interruptCond.valid() && isInterruptCond) {
        std::cerr << "Can not have multiple interrupt conditions" << std::endl;
        std::exit(EXIT_FAILURE);
    } else if (isInterruptCond) {
        interruptCond = tensor;
    }
    hostRequest.push_back(tensor);
#endif
}

int main(int argc, char* argv[]) {
    VlPoplarContext ctx;
    ctx.init(argc, argv);
#ifdef GRAPH_COMPILE
    ctx.buildReEntrant();
#endif
#ifdef GRAPH_RUN
    ctx.runReEntrant();
#endif
    return EXIT_SUCCESS;
}