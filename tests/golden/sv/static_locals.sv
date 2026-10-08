// SPDX-License-Identifier: GPL-3.0-or-later
//
// A static variable of a function or task is one variable every call shares (IEEE 1800 section
// 13.3.1): its initialiser runs once, at time zero, and each call reads what the call before it
// left. A function declared without `automatic` is static, as in Verilog-2001, and so is one in
// a package. Callers sharing one are in static_function_callers.sv.

package serials;
    function integer next_serial();
        static integer last = 100;
        last = last + 1;
        return last;
    endfunction
endpackage

module tb;
    function integer next_id();
        static integer count = 0;
        count = count + 1;
        return count;
    endfunction

    task bump(output integer result);
        static integer seen = 10;
        seen = seen + 1;
        result = seen;
    endtask

    integer first, second, sum, task_first, task_second, serial_first, serial_second;
    int failures = 0;

    initial begin
        first = next_id();
        second = next_id();
        sum = next_id() + next_id();
        bump(task_first);
        bump(task_second);
        serial_first = serials::next_serial();
        serial_second = serials::next_serial();
        $display("ids %0d %0d %0d, task %0d %0d, serials %0d %0d", first, second, sum,
                 task_first, task_second, serial_first, serial_second);
        if (first != 1 || second != 2 || sum != 7) begin
            $display("FAIL: a function's static variable did not keep its value between calls");
            failures = failures + 1;
        end
        if (task_first != 11 || task_second != 12) begin
            $display("FAIL: a task's static variable did not keep its value between calls");
            failures = failures + 1;
        end
        if (serial_first != 101 || serial_second != 102) begin
            $display("FAIL: a package function's static variable did not keep its value");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL static_locals: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS static_locals");
        $finish;
    end
endmodule
