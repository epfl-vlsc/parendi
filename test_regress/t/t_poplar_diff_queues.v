
module Main #(parameter LEVELS = 3)
(
    input wire clock
);
  wire clk = clock;
  reg [15:0] counter;
  wire rst = (counter < 10);
  wire enq_ready;
  wire deq_valid;
  wire [15:0] deq_bits;
  QueueCascade #(LEVELS) dut(
    .clock(clk),
    .reset(rst),
    .io_enq_ready(enq_ready),
    .io_enq_valid(counter[0] == 1'b1),
    .io_enq_bits(counter),
    .io_deq_ready(counter[0] == 1'b0),
    .io_deq_valid(deq_valid),
    .io_deq_bits(deq_bits)
  );


  always @(posedge clk) begin
    counter <= counter + 1;
    $display("@%d got %d", counter, deq_bits);
    if (counter == 1000) begin
      $display("*-* All Finished *-*");
      $finish;
    end
    if (deq_valid && counter[0] == 1'b0) begin
      if (deq_bits[0] != 1'b1) begin
        $stop;
      end
    end

  end


endmodule

module Queue(
  input         clock,
  input         reset,
  output        io_enq_ready,
  input         io_enq_valid,
  input  [15:0] io_enq_bits,
  input         io_deq_ready,
  output        io_deq_valid,
  output [15:0] io_deq_bits
);
  reg [15:0] ram [0:31]; // @[Decoupled.scala 259:44]
  wire  ram_io_deq_bits_MPORT_en; // @[Decoupled.scala 259:44]
  wire [4:0] ram_io_deq_bits_MPORT_addr; // @[Decoupled.scala 259:44]
  wire [15:0] ram_io_deq_bits_MPORT_data; // @[Decoupled.scala 259:44]
  wire [15:0] ram_MPORT_data; // @[Decoupled.scala 259:44]
  wire [4:0] ram_MPORT_addr; // @[Decoupled.scala 259:44]
  wire  ram_MPORT_mask; // @[Decoupled.scala 259:44]
  wire  ram_MPORT_en; // @[Decoupled.scala 259:44]
  reg  ram_io_deq_bits_MPORT_en_pipe_0;
  reg [4:0] ram_io_deq_bits_MPORT_addr_pipe_0;
  reg [4:0] value; // @[Counter.scala 62:40]
  reg [4:0] value_1; // @[Counter.scala 62:40]
  reg  maybe_full; // @[Decoupled.scala 262:27]
  wire  ptr_match = value == value_1; // @[Decoupled.scala 263:33]
  wire  empty = ptr_match & ~maybe_full; // @[Decoupled.scala 264:25]
  wire  full = ptr_match & maybe_full; // @[Decoupled.scala 265:24]
  wire  do_enq = io_enq_ready & io_enq_valid; // @[Decoupled.scala 50:35]
  wire  do_deq = io_deq_ready & io_deq_valid; // @[Decoupled.scala 50:35]
  wire [4:0] _value_T_1 = value + 5'h1; // @[Counter.scala 78:24]
  wire [4:0] _value_T_3 = value_1 + 5'h1; // @[Counter.scala 78:24]
  wire [5:0] _deq_ptr_next_T_1 = 6'h20 - 6'h1; // @[Decoupled.scala 292:57]
  wire [5:0] _GEN_15 = {{1'd0}, value_1}; // @[Decoupled.scala 292:42]
  assign ram_io_deq_bits_MPORT_en = ram_io_deq_bits_MPORT_en_pipe_0;
  assign ram_io_deq_bits_MPORT_addr = ram_io_deq_bits_MPORT_addr_pipe_0;
  assign ram_io_deq_bits_MPORT_data = ram[ram_io_deq_bits_MPORT_addr]; // @[Decoupled.scala 259:44]
  assign ram_MPORT_data = io_enq_bits;
  assign ram_MPORT_addr = value;
  assign ram_MPORT_mask = 1'h1;
  assign ram_MPORT_en = io_enq_ready & io_enq_valid;
  assign io_enq_ready = ~full; // @[Decoupled.scala 289:19]
  assign io_deq_valid = ~empty; // @[Decoupled.scala 288:19]
  assign io_deq_bits = ram_io_deq_bits_MPORT_data; // @[Decoupled.scala 294:17]
  always @(posedge clock) begin
    if (ram_MPORT_en & ram_MPORT_mask) begin
      ram[ram_MPORT_addr] <= ram_MPORT_data; // @[Decoupled.scala 259:44]
    end
    ram_io_deq_bits_MPORT_en_pipe_0 <= 1'h1;
    if (1'h1) begin
      if (do_deq) begin
        if (_GEN_15 == _deq_ptr_next_T_1) begin // @[Decoupled.scala 292:27]
          ram_io_deq_bits_MPORT_addr_pipe_0 <= 5'h0;
        end else begin
          ram_io_deq_bits_MPORT_addr_pipe_0 <= _value_T_3;
        end
      end else begin
        ram_io_deq_bits_MPORT_addr_pipe_0 <= value_1;
      end
    end
    if (reset) begin // @[Counter.scala 62:40]
      value <= 5'h0; // @[Counter.scala 62:40]
    end else if (do_enq) begin // @[Decoupled.scala 272:16]
      value <= _value_T_1; // @[Counter.scala 78:15]
    end
    if (reset) begin // @[Counter.scala 62:40]
      value_1 <= 5'h0; // @[Counter.scala 62:40]
    end else if (do_deq) begin // @[Decoupled.scala 276:16]
      value_1 <= _value_T_3; // @[Counter.scala 78:15]
    end
    if (reset) begin // @[Decoupled.scala 262:27]
      maybe_full <= 1'h0; // @[Decoupled.scala 262:27]
    end else if (do_enq != do_deq) begin // @[Decoupled.scala 279:27]
      maybe_full <= do_enq; // @[Decoupled.scala 280:16]
    end
  end
endmodule

module QueueCascade#(parameter LEVELS = 64)(
    input         clock,
    input         reset,
    output        io_enq_ready,
    input         io_enq_valid,
    input  [15:0] io_enq_bits,
    input         io_deq_ready,
    output        io_deq_valid,
    output [15:0] io_deq_bits
);

    genvar i;

    generate

        for (i = 0; i < LEVELS; i = i + 1) begin : QUEUES
            wire [15:0] enq_bits;
            wire enq_valid;
            wire enq_ready;
            wire [15:0] deq_bits;
            wire deq_valid;
            wire deq_ready;

            Queue qut(
                .clock(clock),
                .reset(reset),
                .io_enq_ready(enq_ready),
                .io_enq_valid(enq_valid),
                .io_enq_bits(enq_bits),
                .io_deq_ready(deq_ready),
                .io_deq_valid(deq_valid),
                .io_deq_bits(deq_bits)
            );
            if (i == 0) begin
                assign enq_bits = io_enq_bits;
                assign enq_valid = io_enq_valid;
                assign io_enq_ready = enq_ready;
            end else begin
                assign enq_bits = QUEUES[i - 1].deq_bits;
                assign enq_valid = QUEUES[i - 1].deq_valid;
                assign QUEUES[i - 1].deq_ready = enq_ready;
            end

        end
    endgenerate

    assign QUEUES[LEVELS - 1].deq_ready = io_deq_ready;
    assign io_deq_bits = QUEUES[LEVELS - 1].deq_bits;
    assign io_deq_valid = QUEUES[LEVELS - 1].deq_valid;

endmodule