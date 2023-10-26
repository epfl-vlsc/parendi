#include <iostream>
#include <random>

#include <boost/program_options.hpp>
#include <poplar/Device.hpp>
#include <poplar/DeviceManager.hpp>
#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>

using namespace poplar;
using namespace poplar::program;
struct Runner {
    std::mt19937 m_gen;
    Graph m_graph;
    ComputeSet m_cs;
    Target m_target;
    Device m_dev;
    std::vector<std::string> m_handles;
    Runner()
        : m_gen{7182931} {
        auto manager = DeviceManager::createDeviceManager();
        auto devices = manager.getDevices(poplar::TargetType::IPU, 1);
        auto it = std::find_if(devices.begin(), devices.end(),
                               [](Device& device) { return device.attach(); });

        if (it == devices.end()) {
            throw std::runtime_error("Failed attaching to a device");  // EXIT_FAILURE
        }

        m_dev = std::move(*it);
        m_target = m_dev.getTarget();
        m_graph = Graph{m_target};
        m_cs = m_graph.addComputeSet("workload");
    }

    void addCodelets(const std::vector<std::string>& paths) { m_graph.addCodelets(paths); }

    void initialize(const FieldRef& t, int n) {
        std::vector<uint32_t> vs(n);
        for (int i = 0; i < n; i++) { vs[i] = m_gen(); }
        m_graph.setInitialValue(t, vs);
    }
    void addVertex(const std::string& vtxName, int N, int tileId) {

        const uint32_t tensorSize = N * 32;  // only works with up to 1024-bit words
        Tensor in1 = m_graph.addVariable(UNSIGNED_INT, {tensorSize}, "in1");
        Tensor in2 = m_graph.addVariable(UNSIGNED_INT, {tensorSize}, "in2");
        Tensor out = m_graph.addVariable(UNSIGNED_INT, {tensorSize}, "out");
        Tensor cycles = m_graph.addVariable(UNSIGNED_INT, {2}, "cycles");
        m_handles.push_back(vtxName + ".cycles");
        m_graph.createHostRead(m_handles.back(), cycles);
        auto vtx = m_graph.addVertex(m_cs, vtxName);
        m_graph.setTileMapping(vtx, tileId);
        m_graph.setTileMapping(in1, tileId);
        m_graph.setTileMapping(in2, tileId);
        m_graph.setTileMapping(out, tileId);
        m_graph.setTileMapping(cycles, tileId);
        m_graph.connect(vtx["in1"], in1);
        m_graph.connect(vtx["in2"], in2);
        m_graph.connect(vtx["out"], out);
        m_graph.connect(vtx["cycles"], cycles);
        // initialize(vtx["in1"], tensorSize);
        // initialize(vtx["in2"], tensorSize);
    }
    void run() {
        Sequence program{Execute{m_cs}};
        OptionFlags flags{};
        Engine engine{m_graph, program, flags};
        engine.load(m_dev);
        engine.run(0);
        std::vector<uint64_t> cyclesReadback(m_handles.size());
        for (int i = 0; i < m_handles.size(); i++) {
            engine.readTensor(m_handles[i], &cyclesReadback[i], &cyclesReadback[i] + 1);
            std::cout << m_handles[i] << ": " << cyclesReadback[i] << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    using namespace boost::program_options;
    options_description desc("Options");
    // clang-format off
    std::vector<std::string> vertexNames, fileNames;
    int repeats = 32;
    desc.add_options()
        ("help,h", "Show help message.")
        ("files,f", value<std::vector<std::string>>(&fileNames)->multitoken()->required(), "codelet file list")
        ("vertex,v", value<std::vector<std::string>>(&vertexNames)->multitoken()->required(), "vertex names")
        ("repeats,r", value<int>(&repeats)->default_value(32), "repeat count (must be power of 2)")
        ;
    // clang-format on
    variables_map vm;
    store(parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
        std::cout << desc << std::endl;
        throw std::runtime_error("Show help");
    }
    notify(vm);
    if (repeats & (repeats - 1) != 0) {
        throw std::invalid_argument("-repeats, -r should be power of 2");
    }

    Runner runner{};

    runner.addCodelets(fileNames);
    for (int tid = 0; tid < vertexNames.size(); tid++) {
        runner.addVertex(vertexNames[tid], repeats, tid);
    }
    runner.run();

}
