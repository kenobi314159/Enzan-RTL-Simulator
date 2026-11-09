// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unpacked arrays and structs holding reals as wholes: initialised by declarations and patterns,
// copied, and compared, which compares element by element, each real as a real (IEEE 1800 section
// 11.4.5): 0.0 equals -0.0, a NaN equals nothing, and an unknown bit elsewhere makes equality
// unknown unless a real differs. Neither Icarus Verilog nor Verilator compares these, so the case
// checks itself against the standard.

`timescale 1ns / 1ps

`define CHECK(condition, what) \
    if (!(condition)) begin \
        $display("FAIL %s", what); \
        failures++; \
    end

module tb;
    typedef struct {
        real x;
        int n;
        real y;
    } point_t;

    typedef struct {
        point_t corner[2];
        real scale;
    } box_t;

    typedef struct {
        real value;
        logic [3:0] flags;
    } tagged_t;

    real init[2] = '{1.5, -2.25};
    real a[3], b[3];
    real grid[2][2];
    real zero = 0.0;
    point_t p = '{x: 1.0, n: 2, y: 3.0};
    point_t q;
    box_t box;
    tagged_t one, other;
    int failures = 0;

    initial begin
        `CHECK(init[0] == 1.5 && init[1] == -2.25, "an array a declaration initialises")
        `CHECK(p.x == 1.0 && p.n == 2 && p.y == 3.0, "a struct a declaration initialises")

        a = '{1.0, 0.0, -2.5};
        b = a;
        `CHECK(b[2] == -2.5, "a copy of a whole array")
        `CHECK(a == b, "an array and its copy")
        b[1] = -0.0;
        `CHECK(a == b && !(a != b), "arrays differing only in 0.0 and -0.0")
        b[2] = 3.0;
        `CHECK(a != b && !(a == b), "arrays differing in a real")
        a[1] = zero / zero;
        b = a;
        `CHECK(a != b && !(a == b), "an array holding a NaN and its copy")

        grid = '{default: 2.5};
        `CHECK(grid[0][0] == 2.5 && grid[1][1] == 2.5, "a default pattern of a real")
        b = '{3{0.5}};
        `CHECK(b[0] == 0.5 && b[1] == 0.5 && b[2] == 0.5, "a replicated pattern of a real")

        p.x = 1.25;
        p.n = 7;
        p.y = p.x * 2;
        q = p;
        `CHECK(q.y == 2.5 && q == p, "a copy of a whole struct")
        q.n = 8;
        `CHECK(q != p, "structs differing in an integer")
        q.n = 7;
        q.x = 1.5;
        `CHECK(q != p, "structs differing in a real")

        box.corner[1] = p;
        box.corner[1].y = 2.0;
        box.scale = 0.5;
        `CHECK(box.corner[1].y * box.scale == 1.0, "a real in a struct in an array in a struct")
        `CHECK(box.corner[1].x == 1.25, "a struct copied into an array of a struct")

        one = '{value: 1.0, flags: 4'bx01x};
        other = '{value: 1.0, flags: 4'b0010};
        `CHECK((one == other) === 1'bx, "an unknown bit beside equal reals")
        `CHECK((one != other) === 1'bx, "an unknown bit beside equal reals, unequal")
        other.value = 2.0;
        `CHECK((one == other) === 1'b0, "an unknown bit beside reals that differ")
        `CHECK(one !== other, "case inequality with an unknown bit")

        if (failures == 0) $display("PASS real_aggregates");
        $finish;
    end
endmodule
