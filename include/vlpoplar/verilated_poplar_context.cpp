
#include VPROGRAM_HEADER /*defined by the Makefile*/

#include "verilated_poplar_context.h"

#include "verilated.h"

#include <fstream>
// #include <boost/filesystem.hpp>
// #include <boost/program_options.hpp>

void VlPoplarContext::init(int argc, char* argv[]) {

    // cfg = parseArgs(argc, argv);

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

    graph = std::make_unique<poplar::Graph>(device->getTarget());
    workload = std::make_unique<poplar::ComputeSet>(graph->addComputeSet("workload"));
    initializer = std::make_unique<poplar::ComputeSet>(graph->addComputeSet("initializer"));
    vprog = std::make_unique<VPROGRAM>(*this);
    std::ifstream fs{CODELET_LIST /*defined by the Makefile*/, std::ios::in};

    for (std::string ln; std::getline(fs, ln);) { graph->addCodelets(ln); }
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
    // handle any host calls invoked by the initial blocks
    if (hostRequest.size() != 1) {
        std::cerr << "Can only handle exactly one host request for now, you have "
                  << hostRequest.size() << std::endl;
        std::exit(EXIT_FAILURE);
    }
    auto zeroValue = graph->addConstant(UNSIGNED_INT, {1}, false, "false value");
    graph->setTileMapping(zeroValue, 0);
    for (auto hr : hostRequest) {
        prog.add(Copy(zeroValue, hr[0]));  // need to clear the loop conditions
    }
    Tensor breakCond = hostRequest.front()[0];
    Sequence loopBody{Execute{*workload}, Sync{SyncType::INTERNAL}, exchangeCopies,
                      Sync{SyncType::INTERNAL}};
    prog.add(RepeatWhileFalse{Sequence{}, breakCond, loopBody, "eval loop"});
    OptionFlags flags{};
    auto exec = compileGraph(*graph, {Sequence{Execute(*initializer), initCopies}, prog}, flags);

    engine = std::make_unique<Engine>(exec, flags);
    engine->load(*device);
}

void VlPoplarContext::run() {
    auto contextp = std::make_unique<VerilatedContext>();
    engine->run(INIT_PROGRAM);
    vprog->hostHandle();
    while (!contextp->gotFinish()) {
        engine->run(EVAL_PROGRAM);
        vprog->hostHandle();
    }
}

void VlPoplarContext::addCopy(const std::string& from, const std::string& to, uint32_t size,
                              bool isInit) {
    poplar::Tensor fromTensor = getTensor(from);
    poplar::Tensor toTensor = getTensor(to);
    poplar::program::Copy cp{fromTensor, toTensor, false, from + " ==> " + to};
    if (isInit) {
        initCopies.add(cp);
    } else {
        exchangeCopies.add(cp);
    }
}
poplar::Tensor VlPoplarContext::addTensor(uint32_t size, const std::string& name) {
    poplar::Tensor t = graph->addVariable(poplar::UNSIGNED_INT, {size}, name);
    tensors.emplace(name, t);
    return t;
}

poplar::VertexRef VlPoplarContext::getOrAddVertex(const std::string& name, bool isInit) {
    auto it = vertices.find(name);
    if (it == vertices.end()) {
        vertices.emplace(name, graph->addVertex(isInit ? *initializer : *workload, name));
    }
    return vertices[name];
}
void VlPoplarContext::setTileMapping(poplar::VertexRef& vtxRef, uint32_t tileId) {
    graph->setTileMapping(vtxRef, tileId);
}
void VlPoplarContext::setTileMapping(poplar::Tensor& tensor, uint32_t tileId) {
    graph->setTileMapping(tensor, tileId);
}
void VlPoplarContext::connect(poplar::VertexRef& vtx, const std::string& field,
                              poplar::Tensor& tensor) {
    graph->connect(vtx[field], tensor);
}
void VlPoplarContext::createHostRead(const std::string& handle, poplar::Tensor& tensor) {
    graph->createHostRead(handle, tensor);
    hbuffers.emplace(handle, std::make_unique<HostBuffer>(tensor.numElements()));
}
void VlPoplarContext::createHostWrite(const std::string& handle, poplar::Tensor& tensor) {
    graph->createHostWrite(handle, tensor);
    hbuffers.emplace(handle, std::make_unique<HostBuffer>(tensor.numElements()));
}

void VlPoplarContext::isHostRequest(poplar::Tensor& tensor) { hostRequest.push_back(tensor); }

int main(int argc, char* argv[]) {
    VlPoplarContext ctx;
    ctx.init(argc, argv);
    ctx.build();
    ctx.run();
    return EXIT_SUCCESS;
}