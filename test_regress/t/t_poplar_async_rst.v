module FFAsyncRst #(parameter W = 32)
(
    input wire clk,
    input wire arst,
    input wire en,
    input wire [W - 1 : 0] din,
    output wire [W - 1 : 0] dout
);

    logic [W - 1 : 0] ff;
    always @(posedge clk or posedge arst) begin
        // $write("hi %b %b %d %b\n", clk, arst, din, en);
        if (arst) ff <= 0;
        else if (en) ff <= din;
    end
    assign dout = ff;
endmodule

// module t(
//     input wire clk
// );
//     // logic clk = 0;
//     // always #2 clk = !clk;
//     wire arst;
//     localparam W = 5;
//     logic [W - 1 : 0] dout;
//     wire [W - 1 : 0] din;
//     wire en;
//     logic [W + 1 : 0] rnd = 0;

//     assign en = (rnd[W] == 1);
//     assign arst = rnd[W + 1];
//     assign din = rnd[W - 1 : 0];
//     logic [W - 1 : 0] expected = 0;
//     logic [31 : 0] counter = 0;
//     FFAsyncRst #(W) dut (.clk(clk), .arst(arst), .en(en), .din(din), .dout(dout));
//     always_ff @(posedge clk) begin
//         int r = $urandom();
//         rnd <= r[W + 1 : 0];
//         counter <= counter + 1;
//         if (rnd[W + 1]) expected <= 0;
//         else if (rnd[W]) expected <= din;
//         else expected <= dout;
//         $display("@%d: din = %b \t dout = %b \t arst = %b \t en = %b", counter, din, dout, rnd[W + 1], rnd[W]);
//         if (counter > 5 && dout != expected) begin
//             $write("Expected %d but got %d\n", expected, dout);
//             $stop;
//         end
//         if (counter > 10000) begin
//             $write("*-* All Finished *-*\n");
//             $finish;
//         end

//     end

// endmodule

module t(input wire clk);

    // logic clk = 0;
    // always #2 clk = !clk;

    wire arst;
    wire [31 : 0] dout;
    wire [31 : 0] din;
    wire en;
    logic m_arst [0 : 255];
    logic [31 : 0] m_din [0 : 255];
    logic [31 : 0] m_dout [0 : 255];
    logic m_en [0 : 255];
    logic [7:0] cycle = 0;

    assign din = m_din[cycle];
    assign en = m_en[cycle];
    assign arst = m_arst[cycle];

    FFAsyncRst #(32) dut (.clk(clk), .arst(arst), .en(en), .din(din), .dout(dout));
`define AssertPrint(v, e) \
    if (v != e) begin \
        $display("@%4d: expected '%s' to be %d but got %d", cycle, `"v`", e, v); \
        $stop; \
    end

    always @(posedge clk) begin
        cycle <= cycle + 1;
        if (cycle > 0) begin
            $display("@%d: din = %d \t dout = %d \t arst = %b \t en = %b", cycle, din, dout, arst, en);
            if (m_arst[cycle] == 1 || m_arst[cycle - 1] == 1) begin `AssertPrint(dout, 0); end
            else if  (m_en[cycle - 1]) begin `AssertPrint(dout, m_din[cycle - 1]); end
        end
        // $display("%d %b %b", din, m_arst[cycle], m_en[cycle]);
        if (cycle == 50) begin
            $display("*-* All Finished *-*");
            $finish;
        end
    end


`define TICK() \
        m_arst[lcycle + 1] = m_arst[lcycle]; \
        m_en[lcycle + 1] = m_en[lcycle]; \
        m_din[lcycle + 1] = m_din[lcycle];\
        lcycle = lcycle + 1

`define POKE(sig, value) m_``sig[lcycle] = value;

    initial begin
        // $dumpfile("arst.vcd");
        // $dumpon();
        logic [7:0] lcycle = 0;
        for (int i = 0; i < 3; i = i + 1) begin
            `POKE(arst, 0);
            `POKE(en, 0);
            `TICK();
        end
        `POKE(en, 1);
        `POKE(din, 7);
        `TICK();

        `POKE(en, 1);
        `POKE(din, 4);
        `TICK();

        `POKE(arst, 1);
        `POKE(din, 3);
        `TICK();

        `POKE(arst, 1);
        `POKE(din, 4);
        `TICK();

        `POKE(arst, 1);
        `POKE(din, 2);
        `POKE(en, 0);
        `TICK();

        `POKE(arst, 0);
        `POKE(din, 5);
        `POKE(en, 0);
        `TICK();

        `POKE(din, 6);
        `POKE(en, 0);
        `TICK();

        `POKE(din, 1);
        `POKE(en, 1);
        `TICK();

        `POKE(din, 2);
        `TICK();

        `TICK();
        `TICK();
        `POKE(arst, 1);
        `TICK();
        `POKE(arst, 0);
        `POKE(en, 1);
        `POKE(din, 120);
        `TICK();
        `POKE(din, 123);
        `TICK();
        `TICK();

        // $dumpfile("arst.vcd");
        // $dumpvars();
    end


endmodule