# Enzan RTL Simulator idea description

The goal of this project is to develop a completely new RTL Simulator tool.
Current RTL simulators are either enormously expensive (Questasim, Modelsim), or lack interactive GUI, are difficult to use and/or have missing key features like language support.
Using AI agents, I aim to create a new fresh RTL Simulator tool that will deliver performance and wide feature set as well as good looking interactive GUI and overall usability.

# Naming

I've picked the name "Enzan" for this project based on the Japanese word.
The word has two-fold meaning.
It can mean "distant peaks" describing the jagged look of a screen covered in waveforms reminiscent of mountain-like horizon.
The other meaning is "calculation" drawing connection to computational efficiency and performance of the RTL simulator.

# Language compilation

The simulator will use existing solutions for VHDL and SystemVerilog compilation.
For SystemVerilog it will utilize C++ library "slang".
For VHDL it will use C library NVC.

Another possibility for a shared language representation is the "hdlConvertor".

The simulator is to be implemented in C++ to get maximum performance and parallelism of execution. 

# General ideas

RTL code will be analyzed and transformed into internal representation which can be executed as individual parallel processes. The simulation is event-driven discrete simulation with pre-set minimum delta step.

Implementation is to be done incrementally. The goal is to get visible usable proof of concept as soon aS possible. For example, an early version might only support a subset of HDL language features, but already be able to simulate them and show in waveform. 

The GUI is to be similar to that of Modelsim. The main feature is to show interactive progressing waveform. Ability to add signals from various modules and printing log. Layout can be defined using a config file. Signals can be added to waveform both in GUI and using a script. In general the tool includes a scripting control, possibly through TCL. The waveform window includes features such as multiple named cursors, cursor locking, zoom, grouping signals, selecting parts of signals into separate signals, selecting signal value format. Default format is hexa for multi-bit values. Simple line going up or down for single bit values.

The color scheme of the GUI is dark. All used colors defined in a config file. The waveform window itself has a high contrast black background and light green foreground.

The testing should be done continuously using unit tests tied to each new implemented feature and also by running simulation on actual RTL designs. The designs will be pulled from various GitHub repositories and contain systemVerilog, VHDL and mixed-language modules. Initially it will be just simple designs, later tests should include more complex ones. The simulator must be efficient enough to handle designs spanning hundreds of thousands of FPGA LUTs.

Besides RTL modules, the simulator will support RTL test benches in VHDL and systemVerilog. Advanced HDL verification features such as constraints and coverage are to be supported in later versions. 

TCL will be used for the definition of file lists. Simulator must be able to auto-infer compile order. 

Mixed clock domain designs will be supported, but can be added in a later development iteration.

RTL modules translated into modular internal representation libraries, which can be reused by multiple executions without repeated source code compilation. (No whole design flattening?)

Later versions will provide handles to RTL hierarchy to external software through an API similar as (or same as) VPI/DPI. Such API is used for example by cocotb to drive the simulation from Python.

The source code must be ready to read and maintain. Important classes and methods must be accompanied by a description comment.

For individual design decisions it is ok to take inspiration from existing open source solutions and relevant research papers.

Installation is done using cmake with clearly recognizable dependencies.

