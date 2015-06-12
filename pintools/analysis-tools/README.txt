OVERVIEW:

This tool runs a memory trace obtained by the memtracker through a very simple cache simulator and outputs the following information:

1. Which source lines and variables trigger creation of cache lines that are used only once before they are evicted. 
2. Which source lines and variables trigger creation of cache lines where less than half of the bytes are used before the cache line is evicted. 

The first type of waste is reported at the end in the ZERO REUSE MAP. The second -- in the LOW UTILIZATION MAP. 

At the end of the simulation, the tool prints both of the maps in full and in the summarized format. A full map contain a record for every evicted cache line, which shows the source location, the variable name, type (if known) and allocation site and the address, which resulted in the creation of this "waste-generating" cache line. 

A summarized map contains the same information as above, but grouped by source line, and sorted in the order of decreasing waste occurrences, so the programmer knows where the waste is occurring. 

USAGE:

# Grab the source from git
% make
% ./wa -f /path/to/memtracker/trace > output_file.txt
