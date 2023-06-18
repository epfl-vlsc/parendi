
module t (
    input wire clk
);


    reg [73: 0] multiWord = 74'h2bcf02356897801abfe;
    reg [31:0] other = 32'hfefefefe;
    reg [31: 0] cnt = 32'h0;

    always @(posedge clk) cnt <= cnt + 1;

    always @(posedge clk) begin
        other <=  multiWord[15 +: 32];
        if (cnt < 4) begin
        end else if (cnt < 8) begin
            multiWord[15 +: 32] <= cnt ^ 32'h897abc12;
        end
    end
`define EXPECT(cond, stmt) \
    if (cond && !(stmt)) begin \
        $display("invalid result!"); \
        $stop; \
    end

    always@(posedge clk) begin
        $display("@%d multi = 0x%x other = 0x%x", cnt, multiWord, other);
        `EXPECT(cnt <= 4, (multiWord == 74'h2bcf02356897801abfe));
        `EXPECT(cnt == 5, multiWord == 74'h2bcf02344bd5e0b2bfe);
        `EXPECT(cnt == 6, multiWord == 74'h2bcf02344bd5e0babfe);
        `EXPECT(cnt == 7, multiWord == 74'h2bcf02344bd5e0a2bfe);
        `EXPECT(cnt >= 8, multiWord == 74'h2bcf02344bd5e0aabfe);
        // EXPECT(cnt == 8, )
        if (cnt == 12) begin
            $display("*-* All Finished *-*");
            $finish;
        end
    end


endmodule