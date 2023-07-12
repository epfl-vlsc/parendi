module t(
    input wire clk
);
    // logic clk = 0;
    // always #2 clk = !clk;

    localparam int unsigned W = 512;
    typedef logic [W - 1 : 0] data_t;
    typedef enum {
        E_ADD,
        E_SUB,
        E_SHL,
        E_SHR,
        E_SHRS,
        E_ONES,
        E_NONE
    } op_t;

    data_t rs1, rs2, res_add, res_sub, res_shl, res_shr, res_shrs, res_mul;
    op_t op = E_NONE;
    logic [$clog2(W) - 1 : 0] shAmount;

    assign shAmount = rs2[$clog2(W) - 1 : 0];
    assign res_add = rs1 + rs2;
    assign res_sub = rs1 - rs2;
    assign res_shl = rs1 << shAmount;
    assign res_shr = rs1 >> shAmount;
    assign res_shrs = $signed(rs1) >>> $signed(shAmount);
    data_t res_d = 512'h7789990008, res_q = 512'h7aaaff, res_qq = 512'hffff89789768;
    logic[31:0] ones;
    always_comb begin
        ones = 0;
        for (int i = 0; i < W; i = i + 32) ones ^= 32'(rs1[i +: 32]);
    end
    always_comb begin
        res_d = res_q;
        unique case (op)
            E_ADD: res_d = res_add;
            E_SUB: res_d = res_sub;
            E_SHL: res_d = res_shl;
            E_SHR: res_d = res_shr;
            E_SHRS: res_d = res_shrs;
            E_ONES: res_d = W'(ones);
            default: res_d = '0;
        endcase
    end
    logic [31:0] counter = 0;
    logic arst = 0;


    always_ff @(posedge clk) begin
        counter <= counter + 1;
        if (counter > 2 && counter < 5) arst <= 1;
        else arst <= 0;
    end
    localparam logic[31:0] magic1 = 32'hffffc7da;
    localparam logic[31:0] magic2 = 32'habab1212;
    `define EXPECT(cycle, v) \
        if (counter == cycle && res_qq != v) begin \
            $display("Invalid result"); \
            $stop; \
        end

    always_ff @(posedge clk or posedge arst) begin

        if (arst) begin
            rs1 <= '0;
            rs2 <= '0;
            op  <= E_NONE;
            res_q <= '0;
            res_qq <= '0;
        end else begin
            res_q <= res_d;
            res_qq <= res_q;
            for (int i = 0; i < W; i = i + 32) begin
                rs1[i +: 32] <= magic1 + 32'(i) + counter;
                rs2[i +: 32] <= magic2 - 32'(i) + counter;
            end
            if (counter == 11) op <= E_ADD;
            if (counter == 12) op <= E_SUB;
            // if (counter == 13) op <= E_MUL;
            if (counter == 14) op <= E_SHL;
            if (counter == 15) op <= E_ONES;
        end
    end
    always_ff @(posedge clk) begin
        $display("@%d res: %h ", counter, res_qq);
        `EXPECT(0, 512'hffff89789768);
        `EXPECT(1, 512'h7aaaff);
        `EXPECT(14, 512'habaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada03abaada02);
        `EXPECT(15, 512'h5454b9885454b9485454b9085454b8c85454b8885454b8485454b8085454b7c85454b7885454b7485454b7085454b6c85454b6885454b6485454b6085454b5c8);
        // `EXPECT(16, 512'h1d9b8ed2c072aaebd8b2d5fa665c13fe696e68f7e1e9d8e6cfce67cb331c19a50bd2f27459f2f6391d7c28f3566e8ea304ca2b48288f02e2c1bd1972d05472f9);
        `EXPECT(17, 512'hffffc9a8ffffc988ffffc968ffffc948ffffc928ffffc908ffffc8e8ffffc8c8ffffc8a8ffffc888ffffc868ffffc848ffffc828ffffc808ffffc7e800000000);
        `EXPECT(18, 512'h0e00);
        if (counter == 18) begin
            $display("*-* All Finished *-*");
            $finish;
        end
    end

endmodule
