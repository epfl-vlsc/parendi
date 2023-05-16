

module ArrayMultiplier #(
    WIDTH = 8
) (
    input wire clock,
    input wire [WIDTH - 1 : 0] X,
    input wire [WIDTH - 1 : 0] Y,
    input wire start,
    output wire [2 * WIDTH - 1 : 0] P,
    output logic done
);


  logic [2 * WIDTH - 1 : 0] xreg[0 : 2 * WIDTH - 2];
  logic [2 * WIDTH - 1 : 0] busy_stage;
  logic [2 * WIDTH - 1 : 0] yreg[0 : 2 * WIDTH - 2];
  logic [2 * WIDTH - 1 : 0] preg[0 : 2 * WIDTH - 1];

  // integer i;

  always_ff @(posedge clock) begin
    // $display("start: %b", start);
    // $display("busy: %b", busy_stage);
    // $write("xreg: ");
    // for (int i = 2 * WIDTH - 1; i >= 0; i = i - 1) begin
    //   $write("%d", xreg[i]);
    // end
    // $write("\nyreg: ");
    // for (int i = 2 * WIDTH - 1; i >= 0; i = i - 1) begin
    //   $write("%d", yreg[i]);
    // end
    // $write("\npreg: ");
    // for (int i = 2 * WIDTH - 1; i >= 0; i = i - 1) begin
    //   $write("%d", preg[i]);
    // end
    // $write("\n");
    busy_stage <= (start << (2 * WIDTH - 1)) | (busy_stage >> 1);
    xreg[0] <= X;
    yreg[0] <= Y;
    for (int i = 1; i < 2 * WIDTH - 1; i = i + 1) begin
      xreg[i] <= xreg[i-1];
      yreg[i] <= yreg[i-1];
    end
    preg[0] <= X[0] ? Y : 0;
    for (int i = 1; i < 2 * WIDTH; i = i + 1) begin
      preg[i] <= (xreg[i-1][i] ? (yreg[i-1] << i) : 0) + preg[i-1];
    end
    done <= busy_stage[0];
    // if (done) $write("======================\n");
  end
  assign P = preg[2*WIDTH-1];
endmodule


module t (
`ifndef DO_TOGGLE
    input wire clk
`endif
);
`ifdef DO_TOGGLE
  logic clock;
  always #10 clock = ~clock;
`else
  wire clock = clk;
`endif
  localparam W = 8;

  logic  [W - 1 : 0] x_val;
  logic  [W - 1 : 0] y_val;
  logic  [2 * W - 1 : 0] p_val, preg;
  wire   [2 * W - 1 : 0] exp = x_val * y_val;
  logic [31 : 0] testIndex;
  logic [31 : 0] TEST_SIZE;
  logic [31 : 0] cycles = 0;
  wire done;
  typedef enum logic[2:0] {IDLE = 0, GEN = 1, WAIT = 2, CHECK = 3, FINISH = 4 } state_t;
  state_t pstate, nstate;
  initial begin
    testIndex = 0;
    pstate = IDLE;
    if (!$value$plusargs("TEST_SIZE=%d", TEST_SIZE)) TEST_SIZE = 50;
  end
  ArrayMultiplier #(
      .WIDTH(W)
  ) dut (
      .clock(clock),
      .X(x_val),
      .Y(y_val),
      .P(p_val),
      .start(pstate == GEN),
      .done(done)
  );
  always_comb begin
    case (pstate)
      IDLE:
        nstate = GEN;
      GEN:
        nstate = WAIT;
      WAIT:
        if (done) nstate = CHECK; else nstate = WAIT;
      CHECK:
        if (testIndex == TEST_SIZE) nstate = FINISH; else nstate = GEN;
      default:
        nstate = FINISH;
    endcase
  end
  always_ff @(posedge clock) cycles <= cycles + 1;
  always_ff @(posedge clock) begin
    if (pstate == FINISH) begin
      $display("Validated %d random stimuli in %d cycles", TEST_SIZE, cycles);
      $display("*-* All Finished *-*");
      $finish;
    end
    if (done) preg <= p_val;
    pstate <= nstate;
    if (pstate == GEN) begin
      x_val <= $urandom();
      y_val <= $urandom();
    end else if (pstate == CHECK) begin
      if (preg != exp) begin
        $display("test[%d]: expected %d * %d = %d but got %d", testIndex, x_val, y_val, exp, preg);
        $stop;
      end else begin
        testIndex <= testIndex + 1;
      end
    end
  end


endmodule