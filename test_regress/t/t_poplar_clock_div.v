
module ClockDiv #(parameter DIVISOR = 2)
(
    input wire clockIn,
    output wire clockOut
);
    localparam CounterBits = $clog2(DIVISOR + 1);
    logic [CounterBits - 1 : 0] counter;
    logic gen = 0;
    assign clockOut = gen;
    initial counter = 0;
    always_ff @(posedge clockIn) begin
        counter <= counter + 1;
        if (counter == CounterBits'(DIVISOR - 1)) begin
            counter <= 0;
            gen <= !gen;
        end
        else begin
            counter <= counter + 1;
        end
    end
    // initial $monitor("@%d clk_div(%d) = %d", $time, DIVISOR, clockOut);
endmodule


module t(
    input wire clk /*verilator clocker*/
);

    // logic clk = 0;
    // always #1 clk = !clk;

    genvar gi;
    logic [31:0] rootTicks = 0;

    always_ff @(posedge clk) begin
        rootTicks <= rootTicks + 1;
        if (rootTicks == 200) $stop;
    end

    wire clkDiv;
    logic [31:0] ticksDiv = 0;
    ClockDiv #(2) dut (.clockIn(clk), .clockOut(clkDiv));
    always_ff @(posedge clkDiv) ticksDiv <= ticksDiv + 1;

    always_ff @(posedge clkDiv) begin
        $write("ticks = %d ticksDiv = %d\n", rootTicks, ticksDiv);
        /*
            ticks =          2 ticksDiv =          0
            ticks =          6 ticksDiv =          1
            ticks =         10 ticksDiv =          2
            ticks =         14 ticksDiv =          3
            ticks =         18 ticksDiv =          4
            ticks =         22 ticksDiv =          5
            ticks =         26 ticksDiv =          6
            ticks =         30 ticksDiv =          7
            ticks =         34 ticksDiv =          8
            ticks =         38 ticksDiv =          9
            ticks =         42 ticksDiv =         10
        */
        if (rootTicks != ticksDiv * 4 + 2) begin
            $write("Invalid result\n");
            $stop;
        end
        if (ticksDiv == 10) begin
            $write("*-* All Finished *-*\n");
            $finish;
        end
    end



endmodule