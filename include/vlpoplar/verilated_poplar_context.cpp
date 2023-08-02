
#include VPROGRAM_HEADER /*defined by the Makefile*/

#include "verilated_poplar_context.h"

#include "verilated.h"

#include <chrono>
#include <fstream>
#include <iomanip>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#define VL_NUM_TILES_PER_IPU 1472

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

    cfg = parseArgs(argc, argv);
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
        Program program;
        Call callback;
        Tensor count;
        explicit TsHandles(const Program& p, const Call& c, const Tensor& t)
            : program{p}
            , callback{c}
            , count{t} {}
    };
    auto createTsProgram = [&](const std::string& name) -> TsHandles {
        auto tsSet = graph->addComputeSet(name);
        graph->addCodelets("VlTimeStamp.gp");
        std::vector<std::tuple<Type, std::size_t>> argSizes;
        std::vector<Tensor> cbTensors;
        Tensor ocount;
        for (int i = 0; i < VL_NUM_TILES_USED; i++) {
            auto vtx = graph->addVertex(tsSet, "VlTimeStamp");
            auto buffer = graph->addVariable(UNSIGNED_LONGLONG, {VL_IPU_TRACE_BUFFER_SIZE},
                                             name + "::buffer");
            auto totCount = graph->addVariable(UNSIGNED_INT, {1}, name + "::totCount");
            graph->setInitialValue(buffer, 0u);

            graph->setInitialValue(totCount, 0u);
            graph->setTileMapping(vtx, i);
            graph->setTileMapping(buffer, i);

            graph->setTileMapping(totCount, i);
            graph->connect(vtx["buffer"], buffer);
            graph->connect(vtx["totCount"], totCount);

            if (i == 0) {
                ocount = totCount;
                argSizes.push_back({UNSIGNED_INT, 1});
                cbTensors.push_back(totCount);
            }
            argSizes.push_back({UNSIGNED_LONGLONG, VL_IPU_TRACE_BUFFER_SIZE});
            cbTensors.push_back(buffer);
            // std::cout << "adding vertex " << i << std::endl;
        }
        auto func = graph->addHostFunction("cb_" + name,
                                           poplar::ArrayRef{argSizes.data(), argSizes.size()}, {});
        Call callback{func, poplar::ArrayRef{cbTensors.data(), cbTensors.size()}, {}};
        Execute program{tsSet};
        return TsHandles{program, callback, ocount};
    };

    auto tsPreExchange = createTsProgram("preExchange");
    auto tsPreWorkload = createTsProgram("preWorkload");
    auto tsPostWorkload = createTsProgram("postWorkload");

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
                tsPreExchange.program,
#endif
                exchangeCopies
#ifdef VL_INSTRUMENT
                // timestamp
                , tsPreWorkload.program
#endif
                , Execute{*workload}
#ifdef VL_INSTRUMENT
                , tsPostWorkload.program,
                If {
                    tsPostWorkload.count[0],
                    Sequence{},
                    Sequence{
                        tsPreExchange.callback,
                        tsPreWorkload.callback,
                        tsPostWorkload.callback
                    }
                }
#endif
            }
        }
#ifdef VL_INSTRUMENT
        , Sequence{
            tsPreExchange.callback,
            tsPreWorkload.callback,
            tsPostWorkload.callback
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
        TimeTraceDump(const std::string& name)
            : name{name} {
            ofs = std::ofstream{std::string{OBJ_DIR} + "/" + name + ".txt", std::ios::out};
        }
    };
    TimeTraceDump preExchange{"preExchange"};
    TimeTraceDump preWorkload{"preWorkload"};
    TimeTraceDump postWorkload{"postWorkload"};

    auto attachCallback = [&](TimeTraceDump& dump) {
        engine->connectHostFunction(
            "cb_" + dump.name, 0,
            [&](poplar::ArrayRef<const void*> ins, poplar::ArrayRef<void*> /*unused*/) -> void {
                // std::cout << "dumping " << dump.name << std::endl;
                const uint32_t count = *reinterpret_cast<const uint32_t*>(ins[0]);
                while (dump.lastCount != count) {
                    int j = dump.lastCount % VL_IPU_TRACE_BUFFER_SIZE;
                    for (int i = 0; i < VL_NUM_TILES_USED; i++) {
                        const uint64_t* bufferp = reinterpret_cast<const uint64_t*>(ins[1 + i]);
                        if (!bufferp[j]) {
                            std::cout << "got zero in " << dump.name << " tile " << i << " pos "
                                      << j << std::endl;
                        }
                        dump.ofs << bufferp[j] << "    ";
                    }
                    dump.ofs << std::endl;
                    dump.lastCount = dump.lastCount + 1;
                }
            });
    };
    attachCallback(preExchange);
    attachCallback(preWorkload);
    attachCallback(postWorkload);
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
#endif
}

void VlPoplarContext::dumpCycleTrace(std::ostream& os) {

    VlIpuProfileTraceVec trace = vprog->profileTrace();

    os << "Cycle summary:" << std::endl;
    uint32_t numTraces = 0;
    for (const auto& desc : trace.m_desc) {
        os << "\t@" << desc.second.m_tile << ", " << desc.second.m_worker << ", " << desc.first
           << ": ";
        os << (desc.second.m_total / static_cast<uint64_t>(desc.second.m_count)) << " ("
           << desc.second.m_pred << ")  x" << desc.second.m_count << std::endl;
        numTraces = std::max(desc.second.m_count, numTraces);
    }
    os << "Cycle trace: " << std::endl;
    for (int i = 0; i < trace.m_traceSize; i++) {
        os << "T[" << numTraces - i << "]" << std::endl;
        const auto& t = trace.m_trace[i];
        for (const auto& desc : trace.m_desc) {
            os << "\t" << desc.first << ": ";
            // if (i >= desc.second.m_count) {
            //     os << "N/A";
            // } else {
            auto s = t[desc.second.m_index].first;
            auto e = t[desc.second.m_index].second;
            os << s << " " << e << " " << (e - s);
            // }
            os << std::endl;
        }
        os << "=================" << std::endl;
    }
}
void VlPoplarContext::build() {
    // build the poplar graph
    using namespace poplar;
    using namespace poplar::program;

    Sequence prog;
    Sequence resetReq;
    Sequence callbacks;
    // handle any host calls invoked by the initial blocks
    if (!interruptCond.valid()) {
        std::cerr << "No interrupt!" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    auto zeroValue = graph->addConstant(UNSIGNED_INT, {1}, false, "false value");
    graph->setTileMapping(zeroValue, 0);
    resetReq.add(Copy(zeroValue, interruptCond[0]));  // need to clear the loop conditions
    for (auto hreq : hostRequest) { resetReq.add(Copy(zeroValue, hreq[0])); }

    auto withCycleCounter = [this, &callbacks](Program&& code, const std::string& n, bool en) {
        Sequence wrapper;
        wrapper.add(code);
        if (en) {
            auto t = cycleCount(*graph, wrapper, 0, SyncType::INTERNAL);
            auto cb = graph->addHostFunction(n, {{UNSIGNED_INT, 2}}, {});
            callbacks.add(Call(cb, {t}, {}));
        }
        return wrapper;
    };
    if (hasCompute) {
        Sequence loopBody{
            withCycleCounter(Execute{*workload}, "prof.exec", cfg.counters.exec),
            withCycleCounter(Sync{SyncType::INTERNAL}, "prof.sync1", cfg.counters.sync1),
            withCycleCounter(std::move(exchangeCopies), "prof.copy", cfg.counters.copy),
            withCycleCounter(Sync{SyncType::INTERNAL}, "prof.sync2", cfg.counters.sync2)};
        Sequence preCond = withCycleCounter(Execute{*condeval}, "prof.cond", cfg.counters.cond);
        prog.add(resetReq);
        prog.add(
            withCycleCounter(RepeatWhileFalse{preCond, interruptCond[0], loopBody, "eval loop"},
                             "prof.loop", cfg.counters.loop));
        prog.add(callbacks);
    }
    Sequence initProg;
    if (hasInit) { initProg.add(Execute(*initializer)); }
    initProg.add(initCopies);
    OptionFlags flags{};
#ifdef POPLAR_INSTRUMENT
    {
        std::ifstream ifs(ROOT_NAME "_compile_options.json", std::ios::in);
        readJSON(ifs, flags);
        ifs.close();
    }
#endif
    float lastProg = 0.0;
    auto exec = compileGraph(*graph, {initProg, prog}, flags, [&lastProg](int step, int total) {
        float newProg = static_cast<float>(step) / static_cast<float>(total) * 100.0;
        if (newProg - lastProg >= 10.0f) {
            std::cout << "Graph compilation: " << static_cast<int>(newProg) << "%" << std::endl;
            lastProg = newProg;
        }
    });

    std::ofstream execOut(ROOT_NAME ".graph.bin", std::ios::binary);
    exec.serialize(execOut);
}

void VlPoplarContext::run() {
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

    measure(
        [this]() {
            std::ifstream graphIn(OBJ_DIR "/" ROOT_NAME ".graph.bin", std::ios::binary);
            auto exec = poplar::Executable::deserialize(graphIn);
            graphIn.close();
            poplar::OptionFlags flags{};
#ifdef POPLAR_INSTRUMENT
            {
                std::ifstream ifs(OBJ_DIR "/" ROOT_NAME "_engine_options.json", std::ios::in);
                poplar::readJSON(ifs, flags);
                ifs.close();
                std::cout << flags << std::endl;
            }
#endif
            engine = std::make_unique<poplar::Engine>(exec, flags);
            engine->load(*device);
            vprog->profileInit();
            vprog->plusArgs();
            vprog->plusArgsCopy();
            vprog->readMem();
            vprog->readMemCopy();
        },
        "load");
    measure([this]() { engine->run(INIT_PROGRAM); }, "init");

    vprog->hostHandle();
    int invIx = 0;

    for (const auto p : {"cond", "exec", "sync1", "copy", "sync2", "loop"}) {
        const std::string handle = std::string{"prof."} + p;
        try {
            engine->connectHostFunction(
                handle, 0,
                [handle /*copy*/, &profile](poplar::ArrayRef<const void*> v,
                                            poplar::ArrayRef<void*> /*unused*/) -> void {
                    profile << "\t" << handle << ": " << reinterpret_cast<const uint64_t*>(v[0])[0]
                            << std::endl;
                });
        } catch (poplar::stream_connection_error& e) {
            // skip
        }
    }

    auto simLoopStart = std::chrono::high_resolution_clock::now();
    while (!Verilated::gotFinish()) {
        profile << "run " << invIx++ << std::endl;
        measure([this]() { engine->run(EVAL_PROGRAM); }, "\twall");
        vprog->hostHandle();
    }
    auto simEnd = std::chrono::high_resolution_clock::now();

    profile << "sim: " << std::chrono::duration<double>(simEnd - simLoopStart).count() << "s"
            << std::endl;
    profile << "all: " << std::chrono::duration<double>(simEnd - simStartTime).count() << "s"
            << std::endl;
    profile.close();
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

RuntimeConfig parseArgs(int argc, char* argv[]) {

    namespace opts = boost::program_options;

    RuntimeConfig cfg;
#ifdef GRAPH_COMPILE
    opts::options_description opt_desc("Allowed options");
    // clang-format off
    opt_desc.add_options()
    ("help,h", "show the help message and exit")
    ("log,l",
        opts::value<boost::filesystem::path>()->value_name("<file>"),
        "redirect runtime logs to a file")
    ("instrument-execute,e",
        opts::bool_switch(&cfg.counters.exec)->default_value(false),
        "instrument the execution phase")
    ("instrument-sync1,s",
        opts::bool_switch(&cfg.counters.sync1)->default_value(false),
        "instrument the sync1 phase")
    ("instrument-copy,c",
        opts::bool_switch(&cfg.counters.copy)->default_value(false),
        "instrument the copy phase")
    ("instrument-sync2,S",
        opts::bool_switch(&cfg.counters.sync2)->default_value(false),
        "instrument the sync2 phase")
    ("instrument-condition,C",
        opts::bool_switch(&cfg.counters.cond)->default_value(false),
        "instrument the condition evaluation")
    ("instrument-loop,L",
        opts::bool_switch(&cfg.counters.loop)->default_value(false),
        "instrument the simulation loop");
    // clang-format on
    opts::variables_map vm;
    try {
        opts::store(opts::parse_command_line(argc, argv, opt_desc), vm);

    } catch (opts::error& e) {
        std::cerr << "Failed parsing arguments: " << e.what() << std::endl;
        std::exit(-2);
    }

    if (vm.count("help")) {
        std::cerr << "Usage: " << argv[0] << " [options]" << std::endl;
        std::cerr << opt_desc << std::endl;
        std::exit(-1);
    }
    opts::notify(vm);
#endif
#ifdef GRAPH_RUN
    // for +args
    Verilated::commandArgs(argc, argv);
#endif
    return cfg;
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