//
//
// Copyright (c) 2011 fpgaminer@bitcoin-mining.com
//
//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//

module t(input wire clk);
	Main dut(.clock(clk));
endmodule

module Main #(parameter LOOP_LOG2 = 0, DIFFICULTY = 15,
	EXPECTED_NONCE = 32'h5386, EXPECTED_GOLDEN_NONCE = 32'h5302)(
// `ifndef DO_TOGGLE
	input wire clock
// `endif
);
// `ifdef DO_TOGGLE
//    reg clock = 0;
//    always #1 clock = !clock;
// `endif
   wire [31:0] golden_nonce;
   wire [31:0] nonce;
   reg [31:0] counter = 0;
   wire clk = clock;
   reg [31:0] timeout;
   reg noCheck;
   initial begin
       if(!$value$plusargs("TIMEOUT=%d", timeout)) timeout = 100000000;
	   noCheck = $test$plusargs("NOCHECK");
   end
   fpgaminer_top
        #(.LOOP_LOG2(LOOP_LOG2), .DIFFICULTY(DIFFICULTY))
        miner(.clk(clk), .golden_nonce(golden_nonce), .nonce_out(nonce));

   always @ (posedge clk) begin
       counter <= counter + 1;
		// if($test$plusargs("VERBOSE")) begin
		// 	$display("+VERBOSE: @ %d %h %h", counter, golden_nonce, nonce);
		// end
       if (golden_nonce) begin
	       $display("@ %d %h %h", counter, golden_nonce, nonce);
		   if (!noCheck && (golden_nonce != EXPECTED_GOLDEN_NONCE)) begin
			$display("Invalid golden nonce %h", golden_nonce);
			$stop;
		   end
		   if (!noCheck && (nonce != EXPECTED_NONCE)) begin
			$display("Invalid nonce %h", nonce);
			$stop;
		   end
		   $display("*-* All Finished *-*");
           $finish;
       end else if (timeout == counter) begin
		   $display("@ %d timed out!", counter);
		   $stop;
	   end
   end

endmodule

module e0(
	input  [31:0] x,
	output [31:0] y
);
	assign y = {x[1:0],x[31:2]} ^ {x[12:0],x[31:13]} ^ {x[21:0],x[31:22]};
endmodule

module e1(
	input  [31:0] x,
	output [31:0] y
);
	assign y = {x[5:0],x[31:6]} ^ {x[10:0],x[31:11]} ^ {x[24:0],x[31:25]};
endmodule

module ch(
	input  [31:0] x, y, z,
	output [31:0] o
);
	assign o = z ^ (x & (y ^ z));
endmodule

module maj(
	input  [31:0] x, y, z,
	output [31:0] o
);
	assign o = (x & y) | (z & (x | y));
endmodule

module s0(
	input  [31:0] x,
	output [31:0] y
);
	assign y[31:29] = x[6:4] ^ x[17:15];
	assign y[28:0] = {x[3:0], x[31:7]} ^ {x[14:0],x[31:18]} ^ x[31:3];
endmodule

module s1(
	input  [31:0] x,
	output [31:0] y
);
	assign y[31:22] = x[16:7] ^ x[18:9];
	assign y[21:0] = {x[6:0],x[31:17]} ^ {x[8:0],x[31:19]} ^ x[31:10];
endmodule


module sha256_digester (clk, k, rx_w, rx_state, tx_w, tx_state);


	input clk;
	input [31:0] k;
	input [511:0] rx_w;
	input [255:0] rx_state;

	output reg [511:0] tx_w;
	output reg [255:0] tx_state;


	wire [31:0] e0_w, e1_w, ch_w, maj_w, s0_w, s1_w;


	e0	e0_blk	(rx_state[31:0], e0_w);
	e1	e1_blk	(rx_state[159:128], e1_w);
	ch	ch_blk	(rx_state[159:128], rx_state[191:160], rx_state[223:192], ch_w);
	maj	maj_blk	(rx_state[31:0], rx_state[63:32], rx_state[95:64], maj_w);
	s0	s0_blk	(rx_w[63:32], s0_w);
	s1	s1_blk	(rx_w[479:448], s1_w);

	wire [31:0] t1 = rx_state[255:224] + e1_w + ch_w + rx_w[31:0] + k;
	wire [31:0] t2 = e0_w + maj_w;
	wire [31:0] new_w = s1_w + rx_w[319:288] + s0_w + rx_w[31:0];


	always @ (posedge clk)
	begin
		tx_w[511:480] <= new_w;
		tx_w[479:0] <= rx_w[511:32];

		tx_state[255:224] <= rx_state[223:192];
		tx_state[223:192] <= rx_state[191:160];
		tx_state[191:160] <= rx_state[159:128];
		tx_state[159:128] <= rx_state[127:96] + t1;
		tx_state[127:96] <= rx_state[95:64];
		tx_state[95:64] <= rx_state[63:32];
		tx_state[63:32] <= rx_state[31:0];
		tx_state[31:0] <= t1 + t2;
	end

endmodule

// Perform a SHA-256 transformation on the given 512-bit data, and 256-bit
// initial state,
// Outputs one 256-bit hash every LOOP cycle(s).
//
// The LOOP parameter determines both the size and speed of this module.
// A value of 1 implies a fully unrolled SHA-256 calculation spanning 64 round
// modules and calculating a full SHA-256 hash every clock cycle. A value of
// 2 implies a half-unrolled loop, with 32 round modules and calculating
// a full hash in 2 clock cycles. And so forth.
module sha256_transform #(parameter LOOP = 6'd4) (clk, feedback, cnt, rx_state, rx_input, tx_hash);
	input clk;
	input feedback;
	input [5:0] cnt;
	input [255:0] rx_state;
	input [511:0] rx_input;
	output reg [255:0] tx_hash;

	// Constants defined by the SHA-2 standard.
	// reg [31:0] ks

	localparam Ks = {
		32'h428a2f98, 32'h71374491, 32'hb5c0fbcf, 32'he9b5dba5,
		32'h3956c25b, 32'h59f111f1, 32'h923f82a4, 32'hab1c5ed5,
		32'hd807aa98, 32'h12835b01, 32'h243185be, 32'h550c7dc3,
		32'h72be5d74, 32'h80deb1fe, 32'h9bdc06a7, 32'hc19bf174,
		32'he49b69c1, 32'hefbe4786, 32'h0fc19dc6, 32'h240ca1cc,
		32'h2de92c6f, 32'h4a7484aa, 32'h5cb0a9dc, 32'h76f988da,
		32'h983e5152, 32'ha831c66d, 32'hb00327c8, 32'hbf597fc7,
		32'hc6e00bf3, 32'hd5a79147, 32'h06ca6351, 32'h14292967,
		32'h27b70a85, 32'h2e1b2138, 32'h4d2c6dfc, 32'h53380d13,
		32'h650a7354, 32'h766a0abb, 32'h81c2c92e, 32'h92722c85,
		32'ha2bfe8a1, 32'ha81a664b, 32'hc24b8b70, 32'hc76c51a3,
		32'hd192e819, 32'hd6990624, 32'hf40e3585, 32'h106aa070,
		32'h19a4c116, 32'h1e376c08, 32'h2748774c, 32'h34b0bcb5,
		32'h391c0cb3, 32'h4ed8aa4a, 32'h5b9cca4f, 32'h682e6ff3,
		32'h748f82ee, 32'h78a5636f, 32'h84c87814, 32'h8cc70208,
		32'h90befffa, 32'ha4506ceb, 32'hbef9a3f7, 32'hc67178f2};


	genvar i;

	generate

		for (i = 0; i < (64/LOOP); i = i + 1) begin : HASHERS
			wire [511:0] W;
			wire [255:0] state;
			// wire [31:0] k;
			if(i == 0) begin
				// sha256_constants C(
				// 	.index(63 - cnt),
				// 	.k(k)
				// );
				sha256_digester U (
					.clk(clk),
					.k(Ks[32*(63-cnt) +: 32]),
					.rx_w(feedback ? W : rx_input),
					.rx_state(feedback ? state : rx_state),
					.tx_w(W),
					.tx_state(state)
				);
			end else begin
				// sha256_constants C(
				// 	.index(63-LOOP*i-cnt),
				// 	.k(k)
				// );
				sha256_digester U (
					.clk(clk),
					.k(Ks[32*(63-LOOP*i-cnt) +: 32]),
					.rx_w(feedback ? W : HASHERS[i-1].W),
					.rx_state(feedback ? state : HASHERS[i-1].state),
					.tx_w(W),
					.tx_state(state)
				);
			end
		end

	endgenerate

	always @ (posedge clk)
	begin
		if (!feedback)
		begin
			tx_hash[31:0] <= rx_state[31:0] + HASHERS[64/LOOP-6'd1].state[31:0];
			tx_hash[63:32] <= rx_state[63:32] + HASHERS[64/LOOP-6'd1].state[63:32];
			tx_hash[95:64] <= rx_state[95:64] + HASHERS[64/LOOP-6'd1].state[95:64];
			tx_hash[127:96] <= rx_state[127:96] + HASHERS[64/LOOP-6'd1].state[127:96];
			tx_hash[159:128] <= rx_state[159:128] + HASHERS[64/LOOP-6'd1].state[159:128];
			tx_hash[191:160] <= rx_state[191:160] + HASHERS[64/LOOP-6'd1].state[191:160];
			tx_hash[223:192] <= rx_state[223:192] + HASHERS[64/LOOP-6'd1].state[223:192];
			tx_hash[255:224] <= rx_state[255:224] + HASHERS[64/LOOP-6'd1].state[255:224];
		end
	end


endmodule


module fpgaminer_top (clk, golden_nonce, nonce_out);

  input clk;
  output reg [31:0] golden_nonce = 0;
  output wire [31:0] nonce_out;
  assign nonce_out = nonce;
  reg [31:0] nonce = 32'h00000000;
  // The LOOP_LOG2 parameter determines how unrolled the SHA-256
  // calculations are. For example, a setting of 0 will completely
  // unroll the calculations, resulting in 128 rounds and a large, but
  // fast design.
  //
  // A setting of 1 will result in 64 rounds, with half the size and
  // half the speed. 2 will be 32 rounds, with 1/4th the size and speed.
  // And so on.
  //
  // Valid range: [0, 5]
  parameter LOOP_LOG2 = 0;
  // The DIFFICULT parameter determines how many trailing hash bits must
  // be zero before declaring success.
  //
  // Valid range: [1, 256]
  parameter DIFFICULTY = 4;

  // No need to adjust these parameters
  localparam [5:0] LOOP = (6'd1 << LOOP_LOG2);
  // The nonce will always be larger at the time we discover a valid
  // hash. This is its offset from the nonce that gave rise to the valid
  // hash (except when LOOP_LOG2 == 0 or 1, where the offset is 131 or
  // 66 respectively).
  localparam [31:0] GOLDEN_NONCE_OFFSET = (32'd1 << (7 - LOOP_LOG2)) + 32'd1;



  ////
  reg [255:0] state = 0;
  reg [511:0] data = 0;


  //// PLL
  wire hash_clk;
  assign hash_clk = clk;

  //// Hashers
  wire [255:0] hash, hash2;
  reg [5:0] cnt = 6'd0;
  reg feedback = 1'b0;

  sha256_transform #(.LOOP(LOOP)) uut (
    .clk(hash_clk),
    .feedback(feedback),
    .cnt(cnt),
    .rx_state(state),
    .rx_input(data),
    .tx_hash(hash)
  );
  sha256_transform #(.LOOP(LOOP)) uut2 (
    .clk(hash_clk),
    .feedback(feedback),
    .cnt(cnt),
    .rx_state(256'h5be0cd191f83d9ab9b05688c510e527fa54ff53a3c6ef372bb67ae856a09e667),
    .rx_input({256'h0000010000000000000000000000000000000000000000000000000080000000, hash}),
    .tx_hash(hash2)
  );

  //// Control Unit

  reg is_golden_ticket = 1'b0;
  reg feedback_d1 = 1'b1;
  wire [5:0] cnt_next;
  wire [31:0] nonce_next;
  wire feedback_next;

  assign cnt_next =  LOOP == 1 ? 6'd0 : ((cnt + 6'd1) & (LOOP-1));
  // On the first count (cnt==0), load data from previous stage (no feedback)
  // on 1..LOOP-1, take feedback from current stage
  // This reduces the throughput by a factor of (LOOP), but also reduces the design size by the same amount
  assign feedback_next = LOOP == 1 ? 1'b0 : (cnt_next != 0);
  assign nonce_next = feedback_next ? nonce : (nonce + 32'd1);
  reg [31:0] cycles = 0;
  always @ (posedge hash_clk)
  begin
	cycles <= cycles + 1;
	if ($test$plusargs("VERBOSE"))begin
		$display("+VERBOSE: @ %d 0x%x 0x%x %d %d %d", cycles, hash2, hash, feedback, cnt, nonce);
	end
    cnt <= cnt_next;
    feedback <= feedback_next;
    feedback_d1 <= feedback;

    // Give new data to the hasher
    state <= 255'd0;
    data <= {384'h000002800000000000000000000000000000000000000000000000000000000000000000000000000000000080000000, nonce_next, 96'd0};
    nonce <= nonce_next;

    // Check to see if the last hash generated is valid.
    // *** This last condition is here only because prior to this point,
    // *** hash2 is undefined and we don't support x as a value
    is_golden_ticket <= hash2[255:255-DIFFICULTY+1] == 0 && !feedback_d1 && (nonce > 32'h81);
    if(is_golden_ticket)
    begin
      // TODO: Find a more compact calculation for this
      if (LOOP == 1)
        golden_nonce <= nonce - 32'd131;
      else if (LOOP == 2)
        golden_nonce <= nonce - 32'd66;
      else
        golden_nonce <= nonce - GOLDEN_NONCE_OFFSET;
    end
  end
endmodule