
#include VPROGRAM_HEADER /*defined by the Makefile*/

#include "verilated_poplar_context.h"

#include "verilated.h"

#include <chrono>
#include <fstream>
#include <iomanip>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
// #include <boost/filesystem.hpp>
// #include <boost/program_options.hpp>

void VlPoplarContext::init(int argc, char* argv[]) {

    cfg = parseArgs(argc, argv);
    auto manager = poplar::DeviceManager::createDeviceManager();
    auto devices = manager.getDevices();
    // get the first available device
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
    std::ifstream fs{CODELET_LIST /*defined by the Makefile*/, std::ios::in};
    for (std::string ln; std::getline(fs, ln);) {
        std::cout << "adding codelet " << ln << std::endl;
        graph->addCodelets(ln);
    }
#endif
    // std::cout << "initializing simulation context " << std::endl;
    vprog->constructAll();
    vprog->initialize();
    vprog->exchange();
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
    auto exec = compileGraph(*graph, {initProg, prog}, flags);

    std::ofstream execOut("main.graph.bin", std::ios::binary);
    exec.serialize(execOut);
}

void VlPoplarContext::run() {
    std::ofstream profile("vlpoplar.log", std::ios::out);
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
            std::ifstream graphIn(OBJ_DIR "/main.graph.bin", std::ios::binary);
            auto exec = poplar::Executable::deserialize(graphIn);
            graphIn.close();
            poplar::OptionFlags flags{};
            engine = std::make_unique<poplar::Engine>(exec, flags);
            engine->load(*device);
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
                              bool isInit) {
#ifdef GRAPH_COMPILE
    poplar::Tensor fromTensor = getTensor(from);
    poplar::Tensor toTensor = getTensor(to);
    poplar::program::Copy cp{fromTensor, toTensor, false, from + " ==> " + to};
    if (isInit) {
        initCopies.add(cp);
    } else {
        exchangeCopies.add(cp);
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
        std::cerr << "Can not have multiple interrup conditions" << std::endl;
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
#else
    // for +args
    Verilated::commandArgs(argc, argv);
#endif
    return cfg;
}

int main(int argc, char* argv[]) {
    VlPoplarContext ctx;
    ctx.init(argc, argv);
#ifdef GRAPH_COMPILE
    ctx.build();
#else
    ctx.run();
#endif
    return EXIT_SUCCESS;
}