module t(
    input wire clk
);


    logic rst_load_q = '0;
    logic [31:0] npc_d, npc_q = '0;
    logic if_ready;
    logic [31:0] if_cache_addr_d, if_cache_addr_q;

    localparam logic [31:0] BootAddr = 32'h0000_1000;
    // V3Split does not split the following always block, but V3SplitComb should
    always_comb begin
        automatic logic [31:0] fetch_addr;

        if (rst_load_q) begin
            npc_d = BootAddr;
            fetch_addr = BootAddr;
        end else begin
            fetch_addr = npc_q;
            npc_d = npc_q;
        end

        if (if_ready) begin
            npc_d = {fetch_addr[31:2], 2'b0} + 'h4;
        end
        if_cache_addr_d = fetch_addr;
    end

    logic [31:0] counter = 0;
    always_ff @(posedge clk) begin
        npc_q <= npc_d;
        if_cache_addr_q <= if_cache_addr_d;
        counter <= counter + 1;
    end

    always_ff @(posedge clk) begin
        rst_load_q <= 0;
        if_ready <= 0;
        if (counter == 32'd3) begin
            rst_load_q <= 1;
        end
        if (counter == 32'd5) begin
            if (npc_q != BootAddr || if_cache_addr_q != BootAddr)
                $stop;
        end
        if (counter == 32'd5) begin
            if_ready <= 1;
        end
        if (counter == 32'd7) begin
            if(npc_q != BootAddr + 32'd4 || if_cache_addr_q != BootAddr)
                $stop;
        end
        $display("@%d %h %h", counter, npc_q, if_cache_addr_q);
        if (counter == 32'd7) begin
            $display("*-* All Finished *-*");
            $finish;
        end
    end


endmodule