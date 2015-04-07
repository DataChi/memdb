
# memdb

This repository contains a family of tools to track memory accesses in applications and to visualize memory access patterns in order to reveal opportunities for program optimization. 

At the moment, the tools allow you to detect sharing (true and false) in multithreaded C applications in order to debug scalability problems. To visualize data sharing in your application, you need to use the following three tools in the order that they are listed:

* memtracker -- a Pin tool to collect a detailed memory trace of your running application
* memtracker2json -- a script that converts the Pin trace to JSON format
* memvis -- a tool that analyzes the JSON trace and produces HTML and JavaScript to visualize it.

You can run them sequentially or as a pipeline. If you run them sequentially, what needs to happen is this:

 1. memtracker will spit log records to stdout. Redirect this output to a file, say log.txt. (See scripts/memtracker.sh for an example.) 
 2. Run memtracker2json.py -- supply log.txt as the input. Memtracker2json will output the JSON trace to the stdout. Save this trace to a file. (See the documentation below for an example). 
 3. Run memvis, providing the JSON trace generated above as an input. See the memvis documentation (below) to visualize the output in the browser. 

An alternative to running the tools sequentially is to pipe the output of each tool into the input of the next tool. This removes the need to save the traces. For an example, take a look at scripts/memtracker+m2j.sh.


## memtracker.so 


This tool tracks memory allocations, memory accesses and function entry/exit points for C programs and the associated libraries. It prints out a trace of allocation and access records. 

The tool has some nice features enabling it to work with real production code. 

* It does not require changes to the source code of your program. 
* It can track any memory allocation function or macro (not just malloc). You specify the prototype of your memory allocation functions in a configuration file. 
* It works for multithreaded programs. 
* It tracks source location of memory accesses (for binaries with debug information) and the names of the accessed variables (for variables allocated on the heap via the tracked allocation functions you identified). 

### HOW TO USE THE TOOL:

#### Pre-requisites to building:

The first pre-requisite for building the pintool is to have the Intel Pin toolkit installed on your machine. Download and unpack the pin toolkit from the Intel's website: 
http://software.intel.com/en-us/articles/pin-a-dynamic-binary-instrumentation-tool

**Note: the MemDB tool is optimized to run with Intel Pin version 2.13 revision 65163, correct work with newer versions of Pin is not guaranteed**

Set the PIN_ROOT environmental variable to point to the root  directory of the toolkit. 

The second prerequisite is to have libelf and libdwarf libraries installed. Those are required by the custom debug_info library used by the pintool to find the types of the allocated variables and the names of the fields within large structures. If you are using Ubuntu, you will be able to run the install.sh script from within memdb/pintools directory (see below) to have them installed automatically. Otherwise, follow the installation instruction for your particular Linux system. 

#### Building:

```
% git clone https://github.com/DataChi/memdb
% cd memdb/pintools
% ./build.sh
```

#### Running:

##### Pre-requisites:

If you want memtracker to be able to know where in your code true or false sharing occurs as well as the names and types of the shared variables, compile your code with debug symbols. Run your program (under the pintool) on the same system where you compiled your program and don't remove or move the source files. (If you are running on a different system, make sure that the source files are in the same absolute path as they were on the system where the program or the library was compiled).

**Read about configuring the tool (below) before running it!**

To run:

```
pin.sh -t $CUSTOM_PINTOOLS_HOME/obj-intel64/memtracker.so -- <your program with arguments>
```

For an example of the actual working script that launches this tool with a WiredTiger library running the LevelDB benchmark, take a look at scripts/memtracker.sh. 

##### Command-line options:

| Option name | Description |
|-------------|-------------|
|  -a [file]  | The file containing the signatures of alloc functions and macros (see below for format). Default: alloc.in. |
|  -f [file]  | The file configuring the scope of tracking (see below for format). Default: memtracker.in |
|  -p [32|64] | Application pointer size. Default: 64.|
|  -s         | Output stack addresses into the trace. Default: no. |

#### Configuring:

There are two required configuration files that memtracker accepts:

1. Configuration of memory allocation functions (see scripts/alloc.in for an example)
2. Configuration of the scope of tracking memory accesses (see scripts/memtracker.in for an example)

##### Configuration of memory allocation functions

You need to describe to memtracker the prototypes of the memory allocation functions used in your program. Why? Memtracker will need to know the address of the allocated variable, the size of the allocation and the number of allocated items (for calloc-like functions). It takes this information by reading the arguments and return values of the memory allocation function. To interpret these values correctly, it needs to know: is the allocated address returned from the function call or is it written to a memory location pointed to by a pointer argument and, if so, which argument? which argument specifies the size of the allocation? 

By default, memtracker looks for allocation function prototypes in the file alloc.in in the current working directory. Alternatively, you can provide your own file name with the -a option to the pintool as follows:

```
pin.sh -t $CUSTOM_PINTOOLS_HOME/obj-intel64/memtracker.so -a my_alloc_prototypes.in -- <your program with arguments>
```

Memtracker expects to find the prototypes of the allocation functions in the following format (one function per line):

```
<func_name>	   <arg_id_of_number> <arg_id_of_size> <arg_id_of_addr>
```

where:

| Token name | Description |
|------------|-------------|
| func_name> | the function name |
| arg_id_of_number | argument id of the number of allocated items or -1 if your alloc function does not use such an argument |
| arg_id_of_size | argument id of the size of the allocation |
| arg_id_of_addr | argument id of the pointer to the location where the allocated address will be stored or -1 if the address is returned by the function. |

arg_id is the index of the corresponding argument passed into the allocation function. We assume that the very first argument has id 0.

For example, consider the following valid alloc.in file:

```
# func                number   size   addr
#
malloc                  -1       0    -1
__wt_calloc              1       2     3 
```

Here we see two function signatures: the conventional malloc from libc and an application-specific allocation function __wt_calloc. Let's walk through these signatures to understand their specification. Malloc does not use the number of allocations in its arguments, so we put "-1" in the <arg_id_of_number> field (the first value after the function name). The size of the allocation is the first argument to malloc, so we put "0" into the <arg_id_of_size> field. The address of the allocation is returned by malloc, so we put "-1" into the <arg_id_of_addr>.

__wt_calloc, on the other hand, expects to receive the number of allocated items as the second argument, so we put "1" into the <arg_id_of_number> field. The size of each item is provided in the third argument so we put "2" into the <arg_id_of_size>. The allocated address is placed into a memory location pointed to by the fourth argument, so we put "3" into the <arg_id_of_addr> field. 

** What if your allocation functions are wrapped by a macro? 

Real-world code sometimes wraps memory-allocation functions into a macro. That would present a problem for memtracker when it attempts to identify the name of the variable for which the function allocates space. Memtracker parses the source file where the allocation function is called, but if it is wrapped in the macro, it will not find it there, because it will be replaced with the name of the macro. To fix this problem, we ask the user to provide the names of the macros (if any) that might be used as wrappers for memory allocation functions. Suppose that __wt_calloc function from the above example can be wrapped in one of three macros. Then, we would specify their names as follows:

```
# func                number   size   addr
#
malloc                  -1       0    -1
__wt_calloc              1       2     3 
!__wt_calloc_def         1      -1     2
!__wt_block_size_alloc  -1      -1     1 
!__bit_alloc            -1      -1     2
```

You see three additional records here corresponding to the __wt_calloc-wrapping macros. They appear directly under the signature for __wt_calloc (this is required) and are prefixed with an "!". Other than that, they have the same format as the simple function-signature record, except the fields <arg_id_of_number> and <arg_id_of_size> are not used. We only care about <arg_id_of_addr>.


##### Limiting the scope of tracking memory accesses

Tracking every memory access in the entire program is very expensive. It will produce very large traces (roughly 1GB for every second of single-threaded execution) and will significantly slow down the program (more than 10,000 times). To limit these effects, you may opt to track memory accesses only within a function of interest (and its descendants). For instance, if you determined by profiling your code that function foo() slows down when run with multiple threads and you want to see whether there is some true or false sharing that is responsible for the slowdown, you can tell the memtracker to only track memory accesses in foo() and its descendants. 

To do so, you use the -f option to the pintool and provide the file name that has the names of the functions of interest. For example, suppose you put your problematic functions in the file funcs.in. Then you would invoke the tool as follows: 

```
pin.sh -t $CUSTOM_PINTOOLS_HOME/obj-intel64/memtracker.so -f funcs.in -- <your program with arguments>
```

By default, memtracker looks for the scope-limiting functions in the file memtracker.in (located in the working directory) even if you don't use the -f option. 

For an example of a valid configuration file, take a look at scripts/memtracker.in.


### UNDERSTANDING MEMTRACKER TRACES

You don't have to understand memtracker traces if you use memtracker2json and memvis to visualize them. This information is intended for those who want to do some else with the traces. 

Here is an excerpt from a trace that memtracker collects:

```
read: 0 0x00007fff802a1bc0 8 calloc <unknown>
function-end: 0 calloc
alloc: 0 0x0000000001ccef10 __wt_calloc 3776 1 /cs/systems/home/fedorova/Work/WiredTiger/wt-dev/build_posix/../src/conn/conn_api.c:1216 conn
function-end: 0 __wt_calloc
read: 0 0x00007f771eefcf38 8 wiredtiger_open /cs/systems/home/fedorova/Work/WiredTiger/wt-dev/build_posix/../src/include/mutex.i:172
write: 0 0x0000000001ccef10 8 wiredtiger_open /cs/systems/home/fedorova/Work/WiredTiger/wt-dev/build_posix/../src/conn/conn_api.c:1217 /cs/systems/home/fedorova/Work/WiredTiger/wt-dev/build_posix/../src/conn/conn_api.c:1216 conn
```

In this example we see five different record types:

1. Allocation record
2. Function delimiter record
3. Memory access record without call-site information 
4. Memory access record with call-site information, but without data-source information
5. Memory access record with call-site information and with data-source information

Let's go into detail of what these records show.

**Allocation record**: This record type is prefixed with "alloc:" and has the following fields:

* thread id
* allocated address
* size of the allocation
* number of items
* source file and line from which the allocation was made
* the name of the variable for which we allocated space.

**Function delimiter record**: Prefixed with "function-begin:" or "function-end:". The fields are:

* thread id
* function name

**Memory access record without call-site information**: Prefixed with "read:" or "write:". The fields are:

* thread id
* memory address 
* size of the accessed data
* function from which the access is made
* <unknown> to indicate that we could not obtain the source file/line information

An example of such a record is the first one in the above trace excerpt.

**Memory access record with call-site information, but without data-source information**: This record has the same format as the type 3 above, but the last field contains the source file/line of the access instead of <unknown>. The fifth record in the above excerpt is an example of such a record. 

**Memory access record with call-site information and with data-source information:** This record is the same as the type 4 above, but we have two additional fields at the end:

* the source code location of the dynamic memory allocation corresponding to this access
* the name of the variable to which this access is made. 


## memtracker2json.py

This script converts the raw trace generated by memtracker to JSON format. JSON format is needed to analyze the memory access pattern and visualize them using our visualization tools. 

Here are the options for running memtracker2json.py:

| Option name | Description |
|-------------|-------------|
|  -h, --help | show this help message and exit |
|  --infile INFILE | Name of the trace file generated by the memtracker pintool.|
|  --keepdots | Do not skip records from .text and .plt when generating trace |


The most common way is to run it like this:

```
./memtracker2json.py --infile <memtracker-raw-trace> > <output-JSON-trace>
```

Alternatively, the script can take the data from stdin:

```
cat memtracker.log | memtracker2json.py > trace.json
```

This is useful if you want to run the memtracker and memtracker2json concurrently. For an example, see scripts/memtracker+m2j.sh.


## memvis

Memviz takes the trace generated by memtracker2json and produces the HTML, JavaScript and data to visualize it. 

### Download and build:

```
% git clone https://github.com/znik/memvis.git
% make
```

### Generate the visualization:

```
MEMVIS_HOME=/path/to/memvis

cat memtracker-json-trace.json | $MEMVIS_HOME/analyzer -p "Trace description" -d $MEMVIS_HOME/server/data
```

### View the visualization in the browser:

#### Option 1: Launch a server locally:

```
% cd $MEMVIS_HOME/server
% ./run_server.sh
```

Then open the browser and type in the name of the machine where you launched the above command and port 8080, for instance: "localhost:8080" if running locally.

#### Option 2: Copy data to an existing web server directory

You can also copy the entire $MEMVIS_HOME/server directory (including the $MEMVIS_HOME/server/data) to a directory of an existing web server and access it via the browser. 


