module t(
    input wire clk
);
   

    typedef logic [31:0] data_t;
    typedef logic [31:0] addr_t;
    typedef struct packed {
        data_t data;
        addr_t addr;
    } payload_t;
    typedef struct packed {
        logic ex;
        logic valid;
        payload_t payload;
    } req_t;

    logic [31:0] counter = '0;
    req_t r1;
    req_t r1_q;
    always_comb begin
        logic addr_is_zero;
        logic addr_is_100;
        addr_is_zero = r1.payload.addr == 0;
        addr_is_100 = r1.payload.addr == 32'h0000_0010;

        r1.ex = addr_is_zero || addr_is_100;
    end
    wire not_ex = !r1.ex;
    wire is_valid = r1.valid;

    always_comb begin
        logic counter_not_10;
        counter_not_10 = counter != 10;
        r1.valid = not_ex && counter_not_10;
    end
    always_comb begin
        r1.payload.addr = {counter[31:2], 2'b0};
    end
    always_comb begin
        r1.payload.data = is_valid ? counter : '0;
    end

// `define EXPECT(at, cond, message)
`define EXPECT(at, cond, message) \
    if (counter == at && !(cond)) begin  \
        $display(message); \
        $stop; \
    end

    always_ff @(posedge clk) counter <= counter + 32'd1;
    always_ff @(posedge clk) r1_q <= r1;
    always_ff @(posedge clk) begin
        $display("@%d: %d %d %d %d\n", counter, r1_q.valid, r1_q.ex,
            r1_q.payload.addr, r1_q.payload.data);
        for (int i = 1; i < 5; i = i + 1) begin
            `EXPECT(i, r1_q.ex == 1, "invalid ex value, should be 1");
            `EXPECT(i, r1_q.valid == 0, "invalid valid, should be 0");
        end
        for (int i = 5; i < 17; i = i + 1) begin
            if (i == 11) begin
                `EXPECT(11, r1_q.valid == 0 && r1_q.ex == 0 && r1_q.payload.data == 0, "invalid resutl");
            end else begin
                `EXPECT(i, r1_q.valid == 1, "valid should be 1");
                `EXPECT(i, r1_q.ex   == 0, "ex should b0");
                `EXPECT(i, r1_q.payload.addr == (((i - 1) >> 2) << 2), "invalid addr");
                `EXPECT(i, r1_q.payload.data == (i - 1), "invalid data");
            end
        end
        for (int i = 17; i < 20; i = i + 1) begin
            `EXPECT(i, r1_q.valid == 0 && r1_q.ex == 1 && r1_q.payload.data == 0 && r1_q.payload.addr == 16, "invalid result");
        end

        // `EXPECT(32'h11, r1_q.ex == 1, "invalid ex value, should be 1");
        // `EXPECT(1, r1_q.valid == 0, "invalid valid, should be 0");
        // `EXPECT(32'h11, r1_q.valid == 0, "ivalid valid, should be 0");
        // `EXPECT(11, r1_q.valid == 0, "invalid valid, should be 0");

        if (counter == 20) begin
            $display("*-* All Finished *-*");
            $finish;
        end
    end
endmodule
