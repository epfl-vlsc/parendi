
<head>
    <h1 align='center'>
    <img src="docs/image/logo_flat.svg", width='320', align='middle', hover='Parendi'><br/>
    Thousand-way Parallel RTL Simulation
    </h1>
</head>

Parendi is a thousand-way parallel RTL simulator for research on the [Graphcore IPU](https://www.graphcore.ai). It can parallelize RTL simulation of large SoCs (hundreds of cores) to up to 5888 IPU cores.

Parendi is built on [Verilator](https://www.veripool.org/verilator) and uses much of its facility but has its new partitioning, optimizations, and code generation passes.
Parendi can be viewed as an extension to Verilator that on top of generating C++ (`--cc`), SystemC (`--sc`), can now generate code that is compiled with Graphcores's [Poplar](https://docs.graphcore.ai/en/latest/child-pages/poplar.html#poplar) SDK (`--poplar`).
For simple use cases, you can swap `--cc` with `--poplar` and expect it to work, but overall, there is much work left to do to complete the `--poplar` backend; see [unsupported features](#Unsupported-features) for more information.

## Who should use Parendi?
Parendi does not intend to replace Verilator today. It is primarily a proof-of-concept simulator that shows that 1000-core parallel RTL simulation is possible.
The primary audience for Parendi right is researchers working on parallel RTL simulation, like us.



## Requirements

#### Hardware
To get something running, you would need access to an IPU. Currently, [Paperspace](https://gcore.com) offers 4 hours of free IPU usage; that's more than enough to try things out, but for long-running simulations experiments or development, you may need to rent one (e.g., from [GCore](https://gcore.com)).

#### Software

We recommend Ubuntu 20.04, other operating systems may also work, but we have not tested them.

You would need the following packages (similar to Veriltor)
```bash
# Prerequisites:
sudo apt-get install git help2man perl python3 make ninja cmake autoconf g++ flex bison ccache
sudo apt-get install libgoogle-perftools-dev numactl perl-doc
sudo apt-get install g++-10 gcc-10
```



You should also download the poplar SDK and set it up from [Graphcore's website](https://www.graphcore.ai/downloads). Alternatively, the following commands should also do the work for you (poplar 3.3):

```bash
apt install wget tar
wget -O 'poplar_sdk-ubuntu_20_04-3.3.0-208993bbb7.tar.gz' 'https://downloads.graphcore.ai/direct?package=poplar-poplar_sdk_ubuntu_20_04_3.3.0_208993bbb7-3.3.0&file=poplar_sdk-ubuntu_20_04-3.3.0-208993bbb7.tar.gz'
tar -xzf poplar_sdk-ubuntu_20_04-3.3.0-208993bbb7.tar.gz
mkdir /opt/poplar
mv poplar_sdk-ubuntu_20_04-3.3.0+1403-208993bbb7/poplar-ubuntu_20_04-3.3.0+7857-b67b751185 /opt/poplar
```

Enable the SDK:
```
source /opt/poplar/enable.sh
```

## Building Parendi

Clone Parendi:

``` bash
git clone https://github.com/epfl-vlsc/parendi.git
```

Parendi uses the KaHyPar graph partitioning library as a submodule:
```bash
git submodule update --init --recursive
```


Do not use the `autoconf` and `Makefile` as you would typically build Verilator; it probably will not work. Instead, build using `cmake`:

```bash
cd parendi
mkdir -p build
cd build
unset VERILATOR_ROOT # PARENDI does not rely on it
cmake .. -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/gcc-10 -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++-10 -G Ninja
cmake --build  . --parallel 8 --config Release
```
This will build Parendi with 8 threads and should take a few minutes.
The built binary relies on having the `PARENDI_ROOT` pointing to the repository's base.
There is no install option for now, so Parendi relies on this environment variable.



## Usage

Parendi has the same commandline interface as Verilator, so you can compile code in much of the same way:
```verilog
// hello_parendi.v
module HelloParendi(input wire clk);
    logic [31:0] counter = 0;
    always_ff @(posedge clk) begin
        counter <= counter + 1;
        if (counter == 10) begin;
            $display("Hello from the IPU");
            $finish;
        end
    end
endmodule
```
Unlike Verilator, where you have a C++ test bench, Parendi requires all the code to be in Verilog.
The top-level module should always have one input, the clock, which is automatically toggled.
This is just a hack until we get around to allowing real clock generation like:
```verilog
logic clk = 0;
always #10 clk = !clk;
```
The reason for this hack is that we do not currently support timing: we would like to allow clock generation, but nothing more, we will implement this feature at some point.

To generate code, you can do the following:
```bash
${PARENDI_ROOT}/bin/verilator --poplar -O3 --build --tiles 4 hello_parendi.v
```
Here, we limited the number of tiles in the generated code to 4 (just as a showcase); the default is 1472, the total number of tiles in one IPU. You can pass up to 5888, and Parendi will target an M2000 or BOW-2000 machine with 4 IPUs.
Like Verilator, the objects are in the `obj_dir` directory. Since we passed `--build`, we don't need to call `make` on the generated objects, and we can directly run the code:

```bash
./obj_dir/VHelloParendi
```

### Running RocketChip simulations
COMING SOON


### Running a demo on Paperspace
COMING SOON


### Running 5888-core simulation on GCore
COMING SOON

### Development Docker
COMING SOON

### Deployment Docker
COMING SOON


## Unsupported features
The following features are currently unsupported:
- C++ test harness
- `--timing` is unsupported
- `UNOPTFLAT` cannot be bypassed with `--poplar`
- Unpacked structs, classes, queues, and associative arrays
- Limited support for packed structs
- DPI export is unsupported (DPI import is buggy and slow)

