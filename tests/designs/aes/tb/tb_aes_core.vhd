-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- secworks' tb_aes_core.v in VHDL, around the core's own Verilog: the NIST vectors driven and the
-- same lines printed, step for step, so that its transcript is the Verilog testbench's under Icarus
-- Verilog. What that testbench reads inside the core through hierarchical names, the key memory and
-- the encipher state, VHDL cannot reach, and those lines are left out on both sides (run.sh).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

entity tb_aes_core_vhdl is
end entity;

architecture test of tb_aes_core_vhdl is
    component aes_core is
        port ( clk, reset_n, encdec, init, \next\ : in std_logic;
               ready : out std_logic;
               key : in std_logic_vector(255 downto 0);
               keylen : in std_logic;
               \block\ : in std_logic_vector(127 downto 0);
               result : out std_logic_vector(127 downto 0);
               result_valid : out std_logic );
    end component;

    constant CLK_HALF_PERIOD : time := 1 ns;
    constant CLK_PERIOD : time := 2 * CLK_HALF_PERIOD;

    constant AES_128_BIT_KEY : std_logic := '0';
    constant AES_256_BIT_KEY : std_logic := '1';
    constant AES_DECIPHER : std_logic := '0';
    constant AES_ENCIPHER : std_logic := '1';

    signal tb_clk : std_logic := '0';
    signal tb_reset_n : std_logic := '1';
    signal tb_encdec : std_logic := '0';
    signal tb_init : std_logic := '0';
    signal tb_next : std_logic := '0';
    signal tb_ready : std_logic;
    signal tb_key : std_logic_vector(255 downto 0) := (others => '0');
    signal tb_keylen : std_logic := '0';
    signal tb_block : std_logic_vector(127 downto 0) := (others => '0');
    signal tb_result : std_logic_vector(127 downto 0);
    signal tb_result_valid : std_logic;

    -- A value as Verilog's %x writes it: lower-case digits, and x for a digit it does not know.
    function hex(value : std_logic_vector) return string is
        variable text : string(1 to (value'length + 3) / 4) := to_hstring(value);
    begin
        for index in text'range loop
            case text(index) is
                when 'A' to 'F' =>
                    text(index) := character'val(character'pos(text(index)) + 32);
                when 'X' | 'U' | 'W' | '-' => text(index) := 'x';
                when 'Z' => text(index) := 'z';
                when others => null;
            end case;
        end loop;
        return text;
    end function;

    function hex(value : std_logic) return string is
    begin
        return hex(std_logic_vector'(0 => value));
    end function;

    function digit(value : natural) return character is
    begin
        return character'val(character'pos('0') + value);
    end function;

    -- A count of at most two digits as Verilog's %0d writes it, and as its %02d does.
    function decimal(value : natural) return string is
    begin
        if value < 10 then
            return (1 => digit(value));
        end if;
        return digit(value / 10) & digit(value mod 10);
    end function;

    function decimal2(value : natural) return string is
    begin
        return digit(value / 10) & digit(value mod 10);
    end function;

    procedure say(text : string) is
        variable buffered : line;
    begin
        write(buffered, text);
        writeline(output, buffered);
    end procedure;
begin
    dut: aes_core
        port map ( clk => tb_clk, reset_n => tb_reset_n, encdec => tb_encdec, init => tb_init,
                   \next\ => tb_next, ready => tb_ready, key => tb_key, keylen => tb_keylen,
                   \block\ => tb_block, result => tb_result, result_valid => tb_result_valid );

    clk_gen: process is
    begin
        wait for CLK_HALF_PERIOD;
        tb_clk <= not tb_clk;
    end process;

    aes_core_test: process is
        variable tc_ctr : natural := 0;
        variable error_ctr : natural := 0;

        procedure dump_dut_state is
        begin
            say("State of DUT");
            say("------------");
            say("Inputs and outputs:");
            say("encdec = 0x" & hex(tb_encdec) & ", init = 0x" & hex(tb_init) & ", next = 0x"
                & hex(tb_next));
            say("keylen = 0x" & hex(tb_keylen) & ", key  = 0x" & hex(tb_key) & " ");
            say("block  = 0x" & hex(tb_block));
            say("");
            say("ready        = 0x" & hex(tb_ready));
            say("result_valid = 0x" & hex(tb_result_valid) & ", result = 0x" & hex(tb_result));
            say("");
            say("");
        end procedure;

        procedure reset_dut is
        begin
            say("*** Toggle reset.");
            tb_reset_n <= '0';
            wait for 2 * CLK_PERIOD;
            tb_reset_n <= '1';
        end procedure;

        procedure wait_ready is
        begin
            while tb_ready = '0' loop
                wait for CLK_PERIOD;
            end loop;
        end procedure;

        procedure ecb_mode_single_block_test(tc_number : natural; encdec : std_logic;
                                             key : std_logic_vector(255 downto 0);
                                             key_length : std_logic;
                                             block_value : std_logic_vector(127 downto 0);
                                             expected : std_logic_vector(127 downto 0)) is
        begin
            say("*** TC " & decimal(tc_number) & " ECB mode test started.");
            tc_ctr := tc_ctr + 1;

            -- Init the cipher with the given key and length.
            tb_key <= key;
            tb_keylen <= key_length;
            tb_init <= '1';
            wait for 2 * CLK_PERIOD;
            tb_init <= '0';
            wait_ready;

            say("Key expansion done");
            say("");
            say("");

            -- Perform encipher or decipher operation on the block.
            tb_encdec <= encdec;
            tb_block <= block_value;
            tb_next <= '1';
            wait for 2 * CLK_PERIOD;
            tb_next <= '0';
            wait_ready;

            if tb_result = expected then
                say("*** TC " & decimal(tc_number) & " successful.");
                say("");
            else
                say("*** ERROR: TC " & decimal(tc_number) & " NOT successful.");
                say("Expected: 0x" & hex(expected));
                say("Got:      0x" & hex(tb_result));
                say("");
                error_ctr := error_ctr + 1;
            end if;
        end procedure;

        constant nist_aes128_key1 : std_logic_vector(255 downto 0) :=
            x"2b7e151628aed2a6abf7158809cf4f3c00000000000000000000000000000000";
        constant nist_aes128_key2 : std_logic_vector(255 downto 0) :=
            x"000102030405060708090a0b0c0d0e0f00000000000000000000000000000000";
        constant nist_aes256_key1 : std_logic_vector(255 downto 0) :=
            x"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4";
        constant nist_aes256_key2 : std_logic_vector(255 downto 0) :=
            x"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

        constant nist_plaintext0 : std_logic_vector(127 downto 0) :=
            x"6bc1bee22e409f96e93d7e117393172a";
        constant nist_plaintext1 : std_logic_vector(127 downto 0) :=
            x"ae2d8a571e03ac9c9eb76fac45af8e51";
        constant nist_plaintext2 : std_logic_vector(127 downto 0) :=
            x"30c81c46a35ce411e5fbc1191a0a52ef";
        constant nist_plaintext3 : std_logic_vector(127 downto 0) :=
            x"f69f2445df4f9b17ad2b417be66c3710";
        constant nist_plaintext4 : std_logic_vector(127 downto 0) :=
            x"00112233445566778899aabbccddeeff";

        constant nist_ecb_128_enc_expected0 : std_logic_vector(127 downto 0) :=
            x"3ad77bb40d7a3660a89ecaf32466ef97";
        constant nist_ecb_128_enc_expected1 : std_logic_vector(127 downto 0) :=
            x"f5d3d58503b9699de785895a96fdbaaf";
        constant nist_ecb_128_enc_expected2 : std_logic_vector(127 downto 0) :=
            x"43b1cd7f598ece23881b00e3ed030688";
        constant nist_ecb_128_enc_expected3 : std_logic_vector(127 downto 0) :=
            x"7b0c785e27e8ad3f8223207104725dd4";
        constant nist_ecb_128_enc_expected4 : std_logic_vector(127 downto 0) :=
            x"69c4e0d86a7b0430d8cdb78070b4c55a";

        constant nist_ecb_256_enc_expected0 : std_logic_vector(127 downto 0) :=
            x"f3eed1bdb5d2a03c064b5a7e3db181f8";
        constant nist_ecb_256_enc_expected1 : std_logic_vector(127 downto 0) :=
            x"591ccb10d410ed26dc5ba74a31362870";
        constant nist_ecb_256_enc_expected2 : std_logic_vector(127 downto 0) :=
            x"b6ed21b99ca6f4f9f153e7b1beafed1d";
        constant nist_ecb_256_enc_expected3 : std_logic_vector(127 downto 0) :=
            x"23304b7a39f9f3ff067d8d8f9e24ecc7";
        constant nist_ecb_256_enc_expected4 : std_logic_vector(127 downto 0) :=
            x"8ea2b7ca516745bfeafc49904b496089";
    begin
        say("   -= Testbench for aes core started =-");
        say("     ================================");
        say("");

        dump_dut_state;
        reset_dut;
        dump_dut_state;

        say("ECB 128 bit key tests");
        say("---------------------");
        ecb_mode_single_block_test(16#01#, AES_ENCIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_plaintext0, nist_ecb_128_enc_expected0);
        ecb_mode_single_block_test(16#02#, AES_ENCIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_plaintext1, nist_ecb_128_enc_expected1);
        ecb_mode_single_block_test(16#03#, AES_ENCIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_plaintext2, nist_ecb_128_enc_expected2);
        ecb_mode_single_block_test(16#04#, AES_ENCIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_plaintext3, nist_ecb_128_enc_expected3);

        ecb_mode_single_block_test(16#05#, AES_DECIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_ecb_128_enc_expected0, nist_plaintext0);
        ecb_mode_single_block_test(16#06#, AES_DECIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_ecb_128_enc_expected1, nist_plaintext1);
        ecb_mode_single_block_test(16#07#, AES_DECIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_ecb_128_enc_expected2, nist_plaintext2);
        ecb_mode_single_block_test(16#08#, AES_DECIPHER, nist_aes128_key1, AES_128_BIT_KEY,
                                   nist_ecb_128_enc_expected3, nist_plaintext3);

        ecb_mode_single_block_test(16#09#, AES_ENCIPHER, nist_aes128_key2, AES_128_BIT_KEY,
                                   nist_plaintext4, nist_ecb_128_enc_expected4);
        ecb_mode_single_block_test(16#0a#, AES_DECIPHER, nist_aes128_key2, AES_128_BIT_KEY,
                                   nist_ecb_128_enc_expected4, nist_plaintext4);

        say("");
        say("ECB 256 bit key tests");
        say("---------------------");
        ecb_mode_single_block_test(16#10#, AES_ENCIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_plaintext0, nist_ecb_256_enc_expected0);
        ecb_mode_single_block_test(16#11#, AES_ENCIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_plaintext1, nist_ecb_256_enc_expected1);
        ecb_mode_single_block_test(16#12#, AES_ENCIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_plaintext2, nist_ecb_256_enc_expected2);
        ecb_mode_single_block_test(16#13#, AES_ENCIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_plaintext3, nist_ecb_256_enc_expected3);

        ecb_mode_single_block_test(16#14#, AES_DECIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_ecb_256_enc_expected0, nist_plaintext0);
        ecb_mode_single_block_test(16#15#, AES_DECIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_ecb_256_enc_expected1, nist_plaintext1);
        ecb_mode_single_block_test(16#16#, AES_DECIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_ecb_256_enc_expected2, nist_plaintext2);
        ecb_mode_single_block_test(16#17#, AES_DECIPHER, nist_aes256_key1, AES_256_BIT_KEY,
                                   nist_ecb_256_enc_expected3, nist_plaintext3);

        ecb_mode_single_block_test(16#18#, AES_ENCIPHER, nist_aes256_key2, AES_256_BIT_KEY,
                                   nist_plaintext4, nist_ecb_256_enc_expected4);
        ecb_mode_single_block_test(16#19#, AES_DECIPHER, nist_aes256_key2, AES_256_BIT_KEY,
                                   nist_ecb_256_enc_expected4, nist_plaintext4);

        if error_ctr = 0 then
            say("*** All " & decimal2(tc_ctr) & " test cases completed successfully");
        else
            say("*** " & decimal2(tc_ctr) & " tests completed - " & decimal2(error_ctr)
                & " test cases did not complete successfully.");
        end if;
        say("");

        say("   -= Testbench for aes core cmpleted =-");
        say("     =================================");
        say("");
        std.env.finish;
    end process;
end architecture;
