/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2013 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */

#include <sys/types.h>
#include <assert.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string.h>
#include <string>
#include <sys/time.h>
#include <stdio.h>
#include <sstream> 
#include <map>
#include <utility>
#include <unistd.h>
#include <sys/syscall.h>
#include "pin.H"

#include "varinfo.hpp"

/* ===================================================================== */
/* Global Variables */
 /* ===================================================================== */
using namespace std;

PIN_LOCK lock;
bool LOUD = false;
bool go = false;
bool selectiveInstrumentation = false;

// Definitions having to do with process and thread stacks.

void get_process_stack(pid_t pid);
void get_and_refresh_thread_stacks(pid_t pid, pid_t tid);

class Stack
{
public:
    size_t start;
    size_t end;
    pid_t tid;
    
    Stack(size_t start, size_t end, pid_t tid)
	{
	    this->start = start;
	    this->end = end;
	    this->tid = tid;
	}

    Stack()
	{
	    this->start = 0;
	    this->end = 0;
	    this->tid = 0;
	}

    bool contains(size_t addr)
	{
	    if(this->start <= addr &&
	       this->end >= addr)
		return true;
	    else
		return false;
	}

    bool operator==(const Stack& rhs) 
	{ 
	    return (this->start == rhs.start && this->end == rhs.end 
		&& this->tid == rhs.tid);
	}

    bool operator!=(const Stack& rhs) 
	{ 
	    return !(this->start == rhs.start && this->end == rhs.end 
		&& this->tid == rhs.tid);
	}

};

ostream& operator<<(std::ostream &strm, const Stack &s) 
{
    return strm << (hex) << s.start <<"-" << s.end << (dec) << " [" << s.tid << "]";
}

Stack processStack(0, 0, 0);

Stack **threadStacks = NULL;
size_t threadStacksSize = 0;

// End stack-related definitions

/* In this map we keep the paths to Image build locations
 * for images we care about. Build locations are needed to
 * parse the types and names of fields within large structures, 
 * because binaries contain paths to source files relative to
 * the build location. 
 *
map<string, string> ImagePathsMap;
*/
typedef enum{
    FUNC_BEGIN,
    FUNC_END
} func_event_t;

char *funcEventNames[2] = {"function-begin:", "function-end:"};

char readStr[] = "read:";
char writeStr[] = "write:";

typedef enum{
    TRACKED,
    ALLOC
} file_mode_t;

#define BITS_PER_BYTE 8
#define KILOBYTE 1024


/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobTrackedFuncsFile(KNOB_MODE_WRITEONCE, "pintool",
    "f", "memtracker.in", "Specify the name of the file with procedures "
			   "where you want to track memory accesses. The file "
			   "must contain a single '*' if you want to track "
                           "all procedures.");

KNOB<string> KnobAllocFuncsFile(KNOB_MODE_WRITEONCE, "pintool",
				"a", "alloc.in", "Specify the  name of the file "
				" with procedures performing "
				"memory allocations");

KNOB<int> KnobAppPtrSize(KNOB_MODE_WRITEONCE, "pintool",
				"p", "64", "application pointer size in bits (default is 64)");

KNOB<bool> KnobTrackStackAccesses(KNOB_MODE_WRITEONCE, "pintool",
				  "s", "false", "Include stack memory accesses into the "
				  "trace. Default is false. ");




/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
   
INT32 Usage()
{
    cerr << "This tool produces a trace of calls to a function." << endl;
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

/* ==================================================================== 
 * A few helper classes to keep a record of memory allocations
 */

class MemoryRange
{
public:
    ADDRINT base;
    ADDRINT size;
						
    MemoryRange(ADDRINT _base, ADDRINT _size):
	base(_base), size(_size) {};

    bool operator<( const MemoryRange& other) const
	{
	    if( base+size <= other.base )
	    {
		return true;
	    }
	    else
		return false;
	};

    bool contains(ADDRINT address) const
	{
	    if(address >= base && address <= base+size)
		return true;
	    else 
		return false;
	}
};

class AllocRecord
{
public:
    string  sourceFile;
    int sourceLine;
    string  varName;
    string varType;
    VarInfo *vi;
    size_t base;
    size_t item_size;
    size_t item_number;

    AllocRecord(string file, int line, 
		string varname, string vartype, VarInfo *v,
		size_t base_addr, size_t size, size_t number):
	sourceFile(file), sourceLine(line), varName(varname), 
	varType(vartype), vi(v), base(base_addr), item_size(size), item_number(number) {};
};

map<MemoryRange, AllocRecord> allocmap;

vector<string> TrackedFuncsList;
vector<string> AllocFuncsList;

/* This struct describes the prototype of an alloc function 
 * Fields "number", "size" and "retaddr" tell us which argument (first argument is indexed '0')
 * gives us the number of elements to allocate, the allocation size and the
 * return address pointer. If we always allocate one item of the given size
 * (as in malloc), the field "number" should be "-1". If the function returns the address
 * of the allocation as opposed to passing it to the application in the in-out
 * pointer, then field "retaddr" should be set to -1.
 */
typedef struct func_proto
{
    string name;
    int number;
    int size;
    int retaddr;
    vector<struct func_proto*> *otherFuncProto;
} FuncProto;

vector<FuncProto *> funcProto;

/* This data structure contains the information about this allocation
 * that we must remember between the time we enter the allocation function
 * and return from it. 
 * Since multiple threads might be making calls to alloc at the same time, 
 * we keep this structure per-thread. 
 */

typedef struct thread_alloc_data
{
    bool dontTrack;
    ADDRINT calledFromAddr;
    ADDRINT size;
    int number;
    ADDRINT addr;
    ADDRINT retptr;
} ThreadAllocData;


typedef struct func_record
{
    string name;
    int breakID;
    int retaddr;
    bool noSourceInfo;
    VarInfo *vi;
    vector<FuncProto*> *otherFuncProto;
    vector<ThreadAllocData*> *thrAllocData; 
} FuncRecord;

vector<FuncRecord*> funcRecords;
unsigned int largestUnusedThreadID = 0;

/* A vector of per-thread flags specifying
 * if the thread is currently in an alloc function. 
 * This is used to track recursive allocations. 
 */
vector<bool> inAlloc;

/* 
 * A vector with per-thread flags specifying whether the thread is
 * executing inside one of the functions that we want to track. 
 * Having an array or a bitmap would be faster, but this is by far
 * not the bottleneck in this heavy-weight tracing tool, so we opt
 * for easier programmability and just use a vector. 
 *
 * While it would be logical to use a vector<bool>, we don't because
 * the implementation of vector<bool> is less thread-safe than the standard vector
 * implementation. 
 */

typedef enum {
    NO,
    YES
} intracked_flag_t;

vector<intracked_flag_t> inTracked;


/* ===================================================================== */
/* Helper routines                                                       */
/* ===================================================================== */

FuncRecord* findFuncRecord(vector<FuncRecord*> *frlist, string name)
{
    assert(lock._owner != 0);

    for(FuncRecord* fr: *frlist)
    {
	if(fr->name.compare(name) == 0)
	    return fr;
    }

    return NULL;
}

FuncRecord* allocateAndAdd(vector<FuncRecord*> *frlist, 
			   FuncProto* fp, VarInfo *vi)
{
    assert(lock._owner != 0);

    FuncRecord *fr = new FuncRecord();
    assert(fr != NULL);
    
    frlist->push_back(fr);
    
    fr->name = fp->name; 
    fr->vi = vi;
    fr->retaddr = fp->retaddr;
    fr->otherFuncProto = fp->otherFuncProto;
    fr->thrAllocData = new vector<ThreadAllocData*>();

    for(uint i = 0; i < largestUnusedThreadID; i++)
    {
	ThreadAllocData *tr = new ThreadAllocData();
	memset(tr, 0, sizeof(ThreadAllocData));
	fr->thrAllocData->push_back(tr);
    }

    return fr;
}

VOID trim(string& s)
{
    
    /* Trim whitespace from the beginning */
    bool reachedBeginning = false;
    while(!reachedBeginning)
    {
	const char *cstring = s.c_str();
	
	if(isspace(cstring[0]))
	    s.erase(0, 1);
	else
	    reachedBeginning = true;
    }

    /* Trim whitespace from the end */
    bool reachedEnd = false;
    while(!reachedEnd)
    {
	const char *cstring = s.c_str();
	int len = s.length();

	if(isspace(cstring[len-1]))
	    s.erase(len-1, 1);
	else
	    reachedEnd = true;
    }
}


/* 
 * We are given a part of the source line that begins with the name of the variable.
 * Let's trim the substring to get rid of any characters that are not part of the 
 * variable name. Characters that can be a part of the variable name are letters, 
 * numbers and the underscore. We also allow "-" and ">" in case we have a complex
 * variable that's dereferencing a pointer, and "[" and "]" in case we have an array.
 */
VOID trimVarName(string& var)
{
    unsigned int pos = 0;

    while(pos < var.length())
    {
	const char *cstring = var.c_str();
	
	if(!isalnum(cstring[pos]) && cstring[pos] != '_' 
	   && cstring[pos] != '-' && cstring[pos] != '>'
	   && cstring[pos] != '[' && cstring[pos] != ']')
	    var.erase(pos, 1);
	else
	    pos++;
    }
}

/*
 * Parse the prototypes of the allocation functions we are tracking.
 * Each function is on its own line. 
 * The first word on the line is the name of the function. 
 * 
 * The second token is an integer specifying number of the argument that
 * tells us how many items we are allocating (as in calloc) or "-1" if 
 * the function does not take such an argument. 
 *
 * The third token is an integer specifying which argument (0th, 1st, 2nd) 
 * tells us the size of the allocated item. This value can never be "-1".
 *
 * The fourth token token is an integer specifying which argument (if any) contains
 * the return address of the allocated memory chunk.
 * This token can either be "-1" or a positive integer. If it is "-1" then the address
 * of the allocated memory chunk (a.k.a. return pointer) is returned by the
 * alloc function (as in malloc). Otherwise, the integer tells us which
 * of the alloc function argument (0th, 1st, 2nd, etc.) contains the pointer to the 
 * allocated address.
 */

VOID parseAllocFuncsProto(vector<string> funcs)
{

    for(string funcDef: funcs)
    {
	bool subDef = false;

	if(funcDef.empty())
	    continue;

	/* Trim whitespace */
	trim(funcDef);

	/* If a line starts with a "!", it's a sub-definition, 
	 * meaning that this is an alternative definition for
	 * a function defined in the previous line. This 
	 * can happen if the function is wrapped in a macro.
	 * In that case, the instrumentation will fire when
	 * we execute the actual function, but the source code
	 * location will guide us to where we are calling the
	 * macro, so we need to know the macro's prototype in
	 * order to correctly parse the name of the allocated
	 * variable. 
	 */
	if(funcDef.find("!") == 0)
	{
	    subDef = true;
	    funcDef = funcDef.substr(1);
	}

	istringstream str(funcDef);
	int iter = 0;
	
	FuncProto *fp = new FuncProto();
	fp->otherFuncProto = new vector<FuncProto*>();

	while(!str.eof())
	{
	    string word;
	    str >> word;

	    switch(iter)
	    {
	    case 0:
		fp->name = word;
		break;
	    case 1:
		fp->number = atoi(word.c_str());
		break;
	    case 2:
		fp->size = atoi(word.c_str());
		break;
	    case 3:
		fp->retaddr = atoi(word.c_str());
		break;
	    case 4:
		cerr << "Invalid format in alloc.in file. Expecting the function "
		    "name and three numbers for prototype (see help message)." 
		     << endl;
		Usage();
		exit(-1);
	    }
	    iter++;
	}
	if(iter != 4)
	{
	    cerr << "Invalid format in alloc.in file. Expecting the function "
		"name and three numbers for prototype (see help message)." 
		 << endl;
	    Usage();
	    exit(-1);
	}
	else
	{
	    if(!subDef)
		funcProto.push_back(fp);
	    else
	    {
		FuncProto *last = funcProto.back();
		if(last == NULL)
		{
		    cerr << "Format error in alloc.in file. "
			"Sub-definition (line starting with \"!\") provided, "
			"but not preceeded by a regular function definition "
			"line (no \"!\" in the beginning)." << endl;
		    exit(-1);
		}
		last->otherFuncProto->push_back(fp);
		cout << last->name << " has alternative function "
		    "prototype under name " << fp->name << endl;
	    }
	}	
    }
}
	   
const char * StripPath(const char * path)
{
    const char * file = strrchr(path,'/');
    if (file)
        return file+1;
    else
        return path;
}

#define BILLION 1000000000

bool fileError(ifstream &sourceFile, string file, int line)
{
    if(!sourceFile)
    {
	cerr << "Error parsing file " << file << endl;
	if(sourceFile.eof())
	{
	    cerr << "Reached end of file before reaching line "
		 << line << endl;
	    cout << "Reached end of file before reaching line "
		 << line << endl;
	}
	else
	    cerr << "Unknown I/O error " << endl;
	return true;
    }
    else
	return false;
}

/* 
 * We need to find the function name and make
 * sure that it's followed either by a newline or
 * space or "(".
 */
bool functionFound(string line, string func, size_t *pos)
{
    if((*pos = line.find(func)) != string::npos
       &&
       (*pos + func.length() == line.length() ||
	line.c_str()[*pos+func.length()] == '(' ||
	isspace(line.c_str()[*pos+func.length()])))
	return true;
    else
    {
	*pos = string::npos;
	return false;
    }
}

bool
validCharInName(char c)
{
    if(c == '_')
	return true;

    return isalnum(c);
}
    
  

/*
 * We are given a line and the position where the function
 * name begins. We have to backtrack to find the name of the
 * variable to which we assign the return value of that function.
 * We return an empty string if no such name is found. 
 *
 * If the function invocation is broken across multiple lines and 
 * we have not found the name of the variable on the same line
 * as the function name, we we given one of the previous lines
 * in the source file, were we search for the variable. The
 * argument pos, in that case, points to the end of the line.
 * 
 * The argument previouslyFoundEquals tells us whether we found the
 * equals sign on one of the lines we previously searched. "Equals"
 * is important, because is it a delimiter between the return variable
 * name and the function name. 
 */
string
findReturnVar(string line, size_t pos, bool *previouslyFoundEquals)
{
    const char *cbuf;
    bool foundVarEnd = false;
    bool foundEquals = *previouslyFoundEquals;
    size_t posAtVarEnd = 0;


    if(pos == 0)
	return "";

    assert(pos < line.length());

    cbuf = line.c_str();

    while(pos > 0)
    {
	if(cbuf[pos] == '=')
	{
	    *previouslyFoundEquals = foundEquals  = true;
	}
	else if(foundEquals && !foundVarEnd && !isspace(cbuf[pos]))
	{
	    foundVarEnd = true;
	    posAtVarEnd = pos;
	}
	else if(foundEquals && foundVarEnd && 
		!validCharInName(cbuf[pos]))
	{
	    pos++;
	    return line.substr(pos, posAtVarEnd-pos+1);
	}
	pos--;
    }
    return "";
}
    


/*
 * This routine will parse the file and return to us the
 * name of the variable, for which we are allocating space. 
 * This variable might be passed to the function as a pointer,
 * which holds the allocated address or it can be assigned the
 * return value of the function. The value of "arg" will tell us
 * which situation we are dealing with. If its the former, arg
 * will tell us which argument is the allocated variable. If it's
 * the latter, arg will equal to "-1". 
 *
 * We pass the full path to the file name and the line within
 * that file (this is the location from where the alloc function
 * was called), the name of the alloc function, and "arg" (described
 * above)
 */
string findAllocVarName(string file, int line, string func, int arg,
			vector<FuncProto*>otherFuncProto)
{
    ifstream sourceFile;
    string lineString, var;
    size_t pos;
    int filepos = line;


#define MAXLINES 5
    string lineBuffer[MAXLINES];
    unsigned int bufferPos = -1;

    sourceFile.open(file.c_str());
    if(!sourceFile.is_open())
    {
	cerr << "Failed to open file " << file << endl;
	cerr << "Cannot parse the name of the allocated variable. " << endl;
	
	var = "";
	goto done;
    }

    /* Scroll through the file to the
     * source line of interest. Remember the MAXLINES last lines
     * read in a buffer. We might need to scroll back a bit
     * later as we look for a variable name.
     */
    while(filepos-- > 0)
    {
	getline(sourceFile, lineString);
	if(fileError(sourceFile, file, line))
	{
	    var = "";
	    goto done;
	}
	lineBuffer[++bufferPos%MAXLINES].assign(lineString);
    }


    /* 
     * Ok, so we are now at the source line where, according to 
     * debugging symbols, we have the alloc function. 
     * Let's attempt to find the alloc function name on that line.
     * We may not suceed for two reasons. 
     *
     * First, the alloc function could be called via a macro with a
     * different name (and also possibly with a different prototype) 
     * than the function itself. 
     * In that case, we will be pointed to
     * the source file/line where we call the macro. So let's search
     * through macro wrappers for that function to find whether
     * one of the macros' names is present on this line. 
     *
     * The second reason we may fail is if the function invocation is
     * spread across multiple lines, as in:
     *
     * myval =
     *         malloc(size);
     *
     * So we are also going to read ahead a MAXLINES lines in the 
     * file to see if we can find our function further down in the
     * file.  
     */
    while(!functionFound(lineString, func, &pos))
    {
	bool found = false;
	/* Let's go through alternative function
	 * prototypes and find it.
	 */
	for(FuncProto *fp: otherFuncProto)
	{
	    if(functionFound(lineString, fp->name, &pos))
	    {
		arg = fp->retaddr;
		found = true;
		break;
	    }
	}
	if(found)
	    break;
	else
	{
	    /* Read the next line and see to we can
	     * find the function there */
	    getline(sourceFile, lineString);
	    if(fileError(sourceFile, file, line))
	    {
		var = "";
		goto done;
	    }
	    lineBuffer[++bufferPos%MAXLINES].assign(lineString);
	}
    }


    if(pos == string::npos)
    {
	cerr << "Cannot find func name " << func << 
	    " on line " << line << " in file " << file << endl;
	var = "";
	goto done;
    }
    
    /* If the allocated variable is an argument to the function 
     * let's skip to it, otherwise, if it is assigned the return
     * value of alloc, let's back-off to find it. 
     */
    if(arg == -1) /* Variable is the return value. Back off */
    {
	/* We kept MAXLINES last lines that we read in a line
	 * buffer in case the name of the variable holding the
	 * return pointer, which is what we are after, and the
	 * function are not on the same line, as in:
	 * 
	 * my_var = 
	 *         malloc(size);
	 *
	 * If the programmer wrote some crazy code that the
	 * variable name and the function name are separated
	 * by more than MAXLINES lines, then we won't be able
	 * to find the variable name. In this case we proceed
	 * running the tool, we just won't report the variable
	 * name for that allocation.
	 */
	bool foundEquals = false;

	for(int i = 0; i < MAXLINES; i++)
	{
	    string curline = lineBuffer[(bufferPos-i) % MAXLINES];

	    if(curline.length() == 0)
		continue;

	    /* After searching for a function name above, pos is
	     * set at the start of that function. If we are backing
	     * up in our buffer in search of the variable, we 
	     * set the position to the end of the line, because we
	     * will be stepping back along the line in search 
	     * of the variable name.
	     */
	    if(i > 0)
		pos = curline.length() - 1;

	    var = findReturnVar(curline, pos, &foundEquals);
	    if(var.length() > 0)
	    {
		break;
		trimVarName(var);
	    }
	}
	
    }
    else
    {
	assert(arg >= 0);

	/* We are skipping lines until we find an opening
	 * bracket after the function name, because 
	 * the function invocation may be spread across
	 * several lines. 
	 */
	while( (pos = lineString.find("(", pos)) == string::npos)
	{
	    getline(sourceFile, lineString);
	    pos = 0;
	    if(fileError(sourceFile, file, line))
	    {
		var = "";
		goto done;
	    }
	}
	
	/* Ok, now from the opening bracket, we need to skip
	 * to the arg-th argument, assuming the first argument is
	 * arg 0. So skip as many commas as necessary
	 */
	int commasToSkip = arg;
	while(commasToSkip-- > 0)
	{
	    /* Here again we may have to skip lines, because
	     * the function invocation may be spread across
	     * multiple lines. 
	     */
	    while( (pos = lineString.find(",", pos+1)) == string::npos)
	    {
		getline(sourceFile, lineString);
		pos = 0;
		if(fileError(sourceFile, file, line))
		{
		    var = "";
		    goto done;
		}
	    }
	}
	/* We've skipped to the last comma, now let's skip
	 * over it.
	 */
	pos++;

	/* Ok, we've skipped all the commas. Now let's skip 
	 * over the whitespace, which may span multiple lines,
	 * to get to our variable.
	 */
	size_t endpos = lineString.length();

	/* We've reached the end of line after skipping all commas.
	 * Our variable must be on the next line.  
	 */
	if(pos == endpos)
	{
	    getline(sourceFile, lineString);
	    if(fileError(sourceFile, file, line))
	    {
		var = "";
		goto done;
	    }

	    pos = 0;
	    endpos = lineString.length();
	}

	while(pos <= endpos && isspace(lineString.c_str()[pos]))
	{
	    /* We've reached the end of line */
	    if(pos == endpos)
	    {
		getline(sourceFile, lineString);
		if(fileError(sourceFile, file, line))
		{
		    var = "";
		    goto done;
		}

		pos = 0;
		endpos = lineString.length();
	    }
	    else
		pos++;
	}

	/* Let's trim away all foreign characters from the line
	 * so all we are left with is the name of the variable 
	 */
	var = lineString.substr(pos);
	trimVarName(var);
    }

done:
    sourceFile.close();
    return var;
}



/* ===================================================================== */
/* Analysis routines                                                     */
/* ===================================================================== */

/* 
 * Callbacks on alloc functions are not working properly before main() is called.
 * We sometimes get a callback for function exit without having received the callback
 * on function enter. So we don't track anything before main() is called. 
 */
VOID callBeforeMain()
{
    if(LOUD)
	cout << "MAIN CALLED ++++++++++++++++++++++++++++++++++++++++++" << endl;

    get_process_stack(getpid());

    go = true;
}

VOID callBeforeAlloc(FuncRecord *fr, THREADID tid, ADDRINT addr, ADDRINT number, 
		     ADDRINT size, ADDRINT retptr)
{
    /* We were called before the application has begun running main.
     * This can happen for malloc calls. Don't do anything. 
     */
    if(!go)
	return;

    assert(fr->thrAllocData->size() > tid);

    /* We don't track recursive alloc functions. We
     * only care to track the top-level allocation function
     * because it lets us figure out the name of the allocated variable.
     */
    if(inAlloc[tid])
    {
	(*fr->thrAllocData)[tid]->dontTrack = true;
	return;
    }

    inAlloc[tid] = true;

    (*fr->thrAllocData)[tid]->calledFromAddr = addr;
    (*fr->thrAllocData)[tid]->size = size;
    (*fr->thrAllocData)[tid]->number = number;
    (*fr->thrAllocData)[tid]->retptr = retptr;

}

VOID callAfterAlloc(FuncRecord *fr, THREADID tid, ADDRINT addr)
{

    if(!go)
	return;


    assert(fr->thrAllocData->size() > tid);
    
    /* Not tracking recursive calls to alloc functions, i.e., 
     * a call to any alloc function within any other alloc func. 
     */
    if(	(*fr->thrAllocData)[tid]->dontTrack)
    {
	(*fr->thrAllocData)[tid]->dontTrack = false;
	return;
    }

    assert((*fr->thrAllocData)[tid]->calledFromAddr != 0);

    if((*fr->thrAllocData)[tid]->retptr == 0)
    {
	/* The address of the allocated chunk is the 
	   return value of the function */
	(*fr->thrAllocData)[tid]->addr = addr;
    }
    else
    {
	/* The address of the allocated chunk is in the location pointed to 
	 * by the retptr argument we received as the argument to the function
	 */
	PIN_SafeCopy( &((*fr->thrAllocData)[tid]->addr), 
		      (const void*)(*fr->thrAllocData)[tid]->retptr, 
		      KnobAppPtrSize/BITS_PER_BYTE);
    }
    
    PIN_GetLock(&lock, PIN_ThreadId() + 1);
    {
	INT32 column = 0, line = 0;
	string filename; 
	string varname, vartype;

	/* Let's get the source file and line */
	PIN_LockClient();
	PIN_GetSourceLocation((*fr->thrAllocData)[tid]->calledFromAddr, 
			      &column, &line, &filename);
	
	PIN_UnlockClient();

	if(filename.length() > 0 && line > 0)
	{
	    varname = 
		findAllocVarName(filename, line, fr->name, fr->retaddr,
				 *(fr->otherFuncProto));

	    /* Let's find the variable type */
	    if(varname.length() > 0 && fr->vi)
		vartype = fr->vi->type(filename, line, varname);
	}

	/* Let's remember this allocation record */
	{
	  ADDRINT base = (*fr->thrAllocData)[tid]->addr;
	  size_t size = (*fr->thrAllocData)[tid]->size * 
	    (*fr->thrAllocData)[tid]->number;
	  size_t item_size = (*fr->thrAllocData)[tid]->size;
	  size_t item_number = 	(*fr->thrAllocData)[tid]->number;
	  MemoryRange *mr = new MemoryRange(base, size);
	  AllocRecord *ar = new AllocRecord(filename, line, varname, vartype, fr->vi,
					    base, item_size, item_number);

	  map<MemoryRange, AllocRecord>::iterator it =
	    allocmap.find(*mr);
	  
	  if(it != allocmap.end())
	  {
	      /* If we found an allocation in the same range as the
	       * new one, chances are someone has freed that allocation.
	       * We don't support tracking of "free" calls yet, so let's
	       * output an "implicit" free record.
	       */
	      cout << "implicit-free: " 
		   << " 0x" << hex << setfill('0') << setw(16) << it->first.base << endl;
	      allocmap.erase(it);
	  }

	  allocmap.insert(make_pair(*mr, *ar));
	
	}
	
	cout << "alloc: " << tid 
	     << " 0x" << hex << setfill('0') << setw(16) << (*fr->thrAllocData)[tid]->addr 
	     << dec << " " << fr->name  
	     << " " << (*fr->thrAllocData)[tid]->size << " " 
	     << (*fr->thrAllocData)[tid]->number 
	     << " " << filename 
	     << ":" << line
	     << " " << varname 
	     << " " << vartype
	     << endl; 
	cout.flush();
    }
    PIN_ReleaseLock(&lock);

    /* Since we are exiting the function, let's reset the
     * "called from" address, to indicate that we are no longer
     * in the middle of the function call. 
     */
    (*fr->thrAllocData)[tid]->calledFromAddr = 0;
    inAlloc[tid] = false;
}

VOID callBeforeAfterFunction(VOID *rtnAddr, func_event_t eventType)
{

    /* Don't track until we hit main() */
    if(!go)
	return;

    PIN_GetLock(&lock, PIN_ThreadId() + 1);

    {
	string name = RTN_FindNameByAddress((ADDRINT)rtnAddr);
	THREADID tid = PIN_ThreadId();
	bool funcNeedsTracking = false;

        /*
	 * If we are entering a new function, 
	 * find out if we need to begin tracking. 
	 * If we are exiting a function, find out if we need to turn
	 * tracking off. 
	 */
	for (string trackedFname: TrackedFuncsList)
	{
	    if(trackedFname.compare(name) == 0)
		funcNeedsTracking = true;
	}
	
	if(funcNeedsTracking && eventType == FUNC_BEGIN) 
	    inTracked[tid] = YES;

	if(inTracked[tid] == YES)
	    cout << (char*)funcEventNames[eventType] << " " << PIN_ThreadId() << " " 
		 << name << endl;	 

	if(funcNeedsTracking && eventType == FUNC_END)
	    inTracked[tid] = NO;
    }

    cout.flush();
    PIN_ReleaseLock(&lock);
}

VOID recordMemoryAccess(ADDRINT addr, UINT32 size, ADDRINT codeAddr, 
		       VOID *rtnAddr, VOID *accessType)
{

    /* Don't track until we hit main() */
    if(!go)
	return;

    if(inTracked[PIN_ThreadId()] == NO)
	return;

    if(!KnobTrackStackAccesses)
    {
	if(processStack.contains((size_t)addr))
	    return;

	if(!threadStacks[PIN_ThreadId()])
	{
	    cerr << "Warning: null stack for thread " << PIN_ThreadId() << endl;
	}
	else if(threadStacks[PIN_ThreadId()]->contains((size_t)addr))
	    return;

    }
    
    PIN_GetLock(&lock, PIN_ThreadId()+1);
    {
	string filename;
	INT32 column = 0, line = 0;
	string source = "<unknown>";

	string name = RTN_FindNameByAddress((ADDRINT)rtnAddr);
    
	PIN_LockClient();
	PIN_GetSourceLocation(codeAddr, &column, &line, &filename);
	PIN_UnlockClient();
   
	if(filename.length() > 0)
	{
	    source = filename + ":" + to_string(line);
	}

	/* Let's retrieve the allocation information for this access */
	MemoryRange mr(addr, size);
	map<MemoryRange, AllocRecord>::iterator it = allocmap.find(mr);

	if(it != allocmap.end())
	{
	    /* We found the allocation record corresponding to that memory access.
	     * If it is a part of a larger structure, let's find out the field name.
	     * Need to retrieve the offset into the data structure. If this allocation
	     * contains multiple items (e.g., calloc-type), need to take the modulo
	     * of the item size.
	     */

	    if(!it->first.contains(addr))
	    {
		cout << "WARNING!!! " << hex << addr <<"+" << size 
		     << " is not contained in (" << 
		    it->first.base << ", " << (it->first.base + it->first.size) << ")"
		     << dec << endl;

		cerr << "WARNING!!! " << hex << addr <<"+" << size 
		     << " is not contained in (" << 
		    it->first.base << ", " << (it->first.base + it->first.size) << ")"
		     << dec << endl;

	    }

	    string field = "";
	    size_t offset = (addr - it->first.base) % it->second.item_size;
	    
	    if(offset >= 0 && it->second.vi)
		field = it->second.vi->fieldname(it->second.sourceFile, 
						 it->second.sourceLine, 
						 it->second.varName, offset);

	    if(field.length() == 0)
		cout << "Could not determine field for the following access type. "
		     << "Allocation base was " << hex << it->second.base 
		     << " Size " << dec << it->second.item_size << ", number " 
		     << it->second.item_number << ". Offset provided was " << offset << endl;
	    
	    cout << (char*)accessType << " " << PIN_ThreadId() << " 0x" << hex << setw(16) 
		 << setfill('0') << addr << dec << " " << size << " " 
		 << name << " " << source << " " << it->second.sourceFile
		 << ":" << it->second.sourceLine << " " << it->second.varName;

	    if(field.length() > 0)
		cout << "->" << field;
 
	    cout << " " << it->second.varType << endl;
	}
	else
	{
	    cout << (char*)accessType << " " << PIN_ThreadId() << " 0x" << hex << setw(16) 
		 << setfill('0') << addr << dec << " " << size << " " 
		 << name << " " << source << endl;
	}
    }
    cout.flush();
    PIN_ReleaseLock(&lock);
}



/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */
   
VOID instrumentRoutine(RTN rtn, VOID * unused)
{
    RTN_Open(rtn);
	    

    /* Instrument entry and exit to the routine to report
     * function-begin and function-end events.
     */
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAfterFunction,
		   IARG_PTR, RTN_Address(rtn), 
		   IARG_UINT32, FUNC_BEGIN, 
		   IARG_END);
    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)callBeforeAfterFunction,
		   IARG_PTR, RTN_Address(rtn),
		   IARG_UINT32, FUNC_END,  
		   IARG_END);

    RTN_Close(rtn);
}

// Is called for every instruction and instruments reads and writes
VOID Instruction(INS ins, VOID *v)
{
    /* Instruments memory accesses using a predicated call, i.e.
     * the instrumentation is called iff the instruction will actually be executed.
     *
     * On the IA-32 and Intel(R) 64 architectures conditional moves and REP 
     * prefixed instructions appear as predicated instructions in Pin.
     */
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    ADDRINT insAddr = INS_Address(ins);
    RTN rtn = RTN_FindByAddress(insAddr);

    // Iterate over each memory operand of the instruction.
    for (UINT32 memOp = 0; memOp < memOperands; memOp++)
    {
        if (INS_MemoryOperandIsRead(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)recordMemoryAccess,
		IARG_MEMORYOP_EA, memOp, 
		IARG_MEMORYREAD_SIZE, 
		IARG_INST_PTR,
		IARG_PTR, RTN_Address(rtn),
		IARG_PTR, readStr, 
                IARG_END);

        }
        // Note that in some architectures a single memory operand can be 
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)recordMemoryAccess,
		IARG_MEMORYOP_EA, memOp, 
		IARG_MEMORYWRITE_SIZE, 
		IARG_INST_PTR,
		IARG_PTR, RTN_Address(rtn),
		IARG_PTR, writeStr, 
                IARG_END);

        }
    }
}


VOID Image(IMG img, VOID *v)
{
    cerr << "Loading image " << IMG_Name(img) << endl;

    /* Find main. We won't do anything before main starts. */
    RTN rtn = RTN_FindByName(img, "main");
    if(RTN_Valid(rtn))
    {
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeMain, IARG_END);
	RTN_Close(rtn);
    }

    /* Go over all the allocation routines we are instrumenting and insert the
     * instrumentation.
     */
    bool varInfoAllocated = false;
    VarInfo *vi = NULL;
    for(FuncProto *fp: funcProto)
    {

	RTN rtn = RTN_FindByName(img, fp->name.c_str());

	if (RTN_Valid(rtn))
	{
	    /* If we found a routine we care about, allocate the
	     * VarInfo structure for that image. Init it if we can.
	     * If not, make it NULL.
	     */
	    if(!varInfoAllocated)
	    {
		varInfoAllocated = true;

		vi = new VarInfo();

		if (!vi->init(IMG_Name(img)))
		{
		    cerr<<"Failed to initialize VarInfo for image " << IMG_Name(img) << endl;
		    delete vi;
		    vi = NULL;
		}
	    }

	    FuncRecord *fr;
	    cout << "Procedure " << fp->name << " located." << endl;

	    PIN_GetLock(&lock, PIN_ThreadId() + 1);
	    if((fr = findFuncRecord(&funcRecords, fp->name)) == NULL)
	    {
		fr = allocateAndAdd(&funcRecords, fp, vi);
	    }


	    assert(fr != NULL);
	    
	    PIN_ReleaseLock(&lock);

	    RTN_Open(rtn);

	    // Instrument 
	    if(fp->number >= 0 && fp->size >= 0  && fp->retaddr >= 0)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->number, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->retaddr, IARG_END);
	    }
	    else if(fp->number == -1 && fp->size >= 0 && fp->retaddr >= 0)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_ADDRINT, 1, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->retaddr, IARG_END);
	    }
	    else if(fp->number == -1 && fp->size >= 0 && fp->retaddr == -1)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_ADDRINT, 1, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_UINT32, 0, IARG_END);
	    }
	    else if(fp->number >= 0 && fp->size >= 0  && fp->retaddr == -1)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->number, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_UINT32, 0, IARG_END);
	    }
	    else {
		cerr << "I did not understand this function prototype: " << endl
		     << fp->name << ": number " << fp->number << ", size " << fp->size
		     << ", retaddr " << fp->retaddr << endl;
		Usage();
		exit(-1);
	    }


	    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)callAfterAlloc,
			   IARG_PTR, fr, IARG_THREAD_ID, IARG_FUNCRET_EXITPOINT_VALUE, 
			   IARG_END);

	    RTN_Close(rtn);

	}
    }   
}


/* ===================================================================== */
/* Parse the list of functions we want to instrument.                    */
/* Return false if we can't open the file or it is empty.                */
/* Return true otherwise.                                                */
/* ===================================================================== */

bool parseFunctionList(const char *fname, vector<string> &list, file_mode_t mode)
{
    bool selective = true;
    ifstream f;
    
    f.open(fname);
    
    if(f.fail())
    {
	cerr << "Failed to open required file " << fname << endl;
	exit(-1);
    }

    cout << "Routines specified for instrumentation:" << endl;
    while(!f.eof())
    {
	string name;
	getline(f, name);
	cout << name << endl;
	
	/* Lines beginning with a # are to be ignored */
	if(name.find("#") == 0)
	    continue;

	/* Trim the string to get rid of empty ones */
	trim(name);

	if(name.length() == 0)
	    continue;

        /* See if we have a line with only * on it. 
	 * If so, we are tracking all functions.
	 */
	if(name.compare("*") == 0)
	{
	    if(mode == TRACKED)
	    {
		selective = false;
		continue;
	    }
	    else if(mode == ALLOC)
	    {
		cerr << "Found a line with '*' and nothing else on it. "
		    "This makes no sense in the allocation function "
		    "config file. Please read the documentation on the "
		    "correct format." << endl;
		exit(-1);
	    }
	}
	
	list.push_back(name);
    }

    f.close();
    
    if(list.size() == 0)
    {
	if(mode == TRACKED && selective)
	{
	    cerr << "No function names in file " << fname << 
		" and no line with '*'. " 
		"Please specify what you want to track. " << endl;
	    exit(-1);
	}
	else
	    return false;
    }
    else
    {
	if(mode == TRACKED && !selective)
	{
	    cerr << "There appear to be names of function to track "
		"in file " << fname << " as well as a line with '*'. "
		"Don't understand if you want to track everything or just "
		"selected functions. " << endl;
	    exit(-1);
	}
	else
	    return true;
    }
}


/* ===================================================================== */

/* 
 * Go over all func records and allocate thread-local storage for that
 * data. That storage is only allocated, never freed. If a thread exits,
 * that thread's slot is simply unused. Pin thread IDs begin from zero and 
 * monotonically increase. The IDs are not reused, so each new thread needs
 * to allocate space for itself in those function records that have already
 * been created. Some function records can be created after threads have
 * started. In that case, thread storage will be allocated at the time of
 * creation of function records in the Image function. 
 *
 * Thread IDs are not necessarily consecutively increasing. We may see
 * a thread 0 followed by a thread 2, for instance. (I.e., this may happen
 * if we are running pin and debugging the app at the same time.). In order
 * to speed up the thread's access to its local data, we want to be able
 * to index the array of per-thread data records by thread id. So we
 * if we see a gap in thread IDs we will create an extra record in the
 * array to fill that gap, even though that record will never be used.  
 *
 * I don't know if this call-back can be executed concurrently by 
 * multiple threads. If yes, then we should be acquiring a lock here.
 * However, when I acquire and release a lock (even just around the 
 * two printouts in the beginning), I get a deadlock. My guess is that
 * there is something within Pin that doesn't like when we acquire
 * locks within a ThreadStart routine. 
 */

VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{

    cerr << "Thread " << threadid << " [" << syscall(SYS_gettid)<< "] is starting " << endl;
    cout << "Thread " << threadid << " [" << syscall(SYS_gettid)<< "] is starting " << endl;

    /* We need to refresh our info on stack ranges every time
     * a new thread starts. We pass the tid, which is Linux-specific.
     * If need arises, this needs to be changed to be more platform-independent.
     */
    get_and_refresh_thread_stacks(getpid(), syscall(SYS_gettid));

    /* Thread IDs are monotonically increasing and are not reused
     * if a thread exits. */
    largestUnusedThreadID = threadid+1;

    /* If we are tracking everything set the inTracked flag to YES, 
       otherwise to NO.
    */
    if(selectiveInstrumentation)
	inTracked.push_back(NO);
    else
	inTracked.push_back(YES);

    /* A thread is not in an alloc func when it starts */
    inAlloc.push_back(false);

    for(FuncRecord *fr: funcRecords)
    {
	while(fr->thrAllocData->size() < (threadid + 1))
	{

	    ThreadAllocData *tr = new ThreadAllocData();
	    memset(tr, 0, sizeof(ThreadAllocData));
	    fr->thrAllocData->push_back(tr);
	}
    }

}

VOID ThreadFini(THREADID threadid, const CONTEXT *ctxt, INT32 code, VOID *v)
{

    cerr << "Thread " << threadid << " [" << syscall(SYS_gettid)<< "] is exiting " << endl;
    cout << "Thread " << threadid << " [" << syscall(SYS_gettid)<< "] is exiting " << endl;

    threadStacks[threadid] = 0;
}


VOID Fini(INT32 code, VOID *v)
{
    cout << "PR DONE" << endl;
}



/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    // Initialize pin & symbol manager
    PIN_InitSymbols();
    if( PIN_Init(argc,argv) )
    {
	return Usage();
    }    
    
    PIN_InitLock(&lock);

    /* If the user wants to trace only the specific function (and whatever is
     * called from them), they would provide a list of functions of interest. 
     */
    selectiveInstrumentation = 
	parseFunctionList(KnobTrackedFuncsFile.Value().c_str(), TrackedFuncsList, TRACKED);

    /* Parse the allocation function prototypes */
    parseFunctionList(KnobAllocFuncsFile.Value().c_str(), AllocFuncsList, ALLOC);
    parseAllocFuncsProto(AllocFuncsList);

    /* Instrument all functions to output when they begin and end */
    RTN_AddInstrumentFunction(instrumentRoutine, 0);

    /* Instrument tracing memory accesses */
    INS_AddInstrumentFunction(Instruction, 0);

    IMG_AddInstrumentFunction(Image, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);
    PIN_AddFiniFunction(Fini, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
    
}

/* ===================================================================== *
 * Various helper routines that do not directly instrument or analyze code
 * ===================================================================== */
void growThreadStacks(int newSize)
{
    threadStacks = (Stack**)realloc(threadStacks, newSize*sizeof(Stack*));
    assert(threadStacks);
    threadStacksSize = newSize;
}

void printThreadStacks()
{
    size_t i;
    for(i = 0; i< threadStacksSize; i++)
    {
	if(threadStacks[i])
	    cerr << *threadStacks[i] << endl;
	else
	    cerr << "NULL stack at index " << i << endl;
    }
}


Stack* get_thread_stack(pid_t pid, pid_t tid)
{
    FILE *cmd_output;
    char *line = NULL;
    ssize_t bytes_read = 0;
    size_t len = 0;
    Stack *s = NULL;

    stringstream maps_stream;
    maps_stream << "cat /proc/"<< pid << "/task/" << tid << "/maps";
    string maps_command = maps_stream.str();
 
    /* This will execute the cat command and pipe
     * the output to a file handle cmd_output.
     */
    cmd_output = popen(maps_command.c_str(), "r");
    if(cmd_output == NULL)
    {
	cerr << "Could not read output from command: "<< maps_command << endl;
	return NULL;
    }
    
    /* Let's read the output line by line until we find the record
     * corresponding to the stack region. */
    while(bytes_read != -1)
    {
	bytes_read = getline(&line, &len, cmd_output);
	if(bytes_read > 0)
	{
	    string lineStr(line);
	    size_t pos;

	    if((pos = lineStr.find("[stack]")) != string::npos)
	    {
		char *end_ptr = 0;

		/* Let's parse the stack boundaries. 
		 * The first value on the line should be the stack start, 
		 * then, a '-', and then the stack end.
		 */
		size_t stack_start = strtol(line, &end_ptr, 16);
		if(!end_ptr || end_ptr[0] != '-')
		{
		    cerr << "Unexpected line format when parsing thread stacks " << endl;
		    cerr << "Offending line is: " << endl;
		    cerr << line << endl;
		    free(line);
	    pclose(cmd_output);
		    return NULL;
		}
		end_ptr++;

		size_t stack_end = strtol(end_ptr, NULL, 16);

		s = new Stack(stack_start, stack_end, tid);
	    }
	}
	if(len > 0)
	{
	    free(line);
	    line = NULL;
	}
    }

    pclose(cmd_output);
    return s;

}

/* This function finds the stack for the
 * newly created thread (tid) and refreshes
 * the stacks for existing threads in case they 
 * have been reallocates. 
 */
void
get_and_refresh_thread_stacks(pid_t pid, pid_t tid)
{
    /* Let's go over existing thread stacks to see
     * if they were reallocates */
    size_t i;
    for(i = 0; i < threadStacksSize; i++)
    {
	if(!threadStacks[i])
	    continue;

	Stack *oldStack = threadStacks[i];
	Stack *newStack = get_thread_stack(pid, oldStack->tid);

	if(!newStack)
	    continue;
	
	if(*oldStack != *newStack)
	{
	    cerr << "Stack for thread " << i << " has changed" << endl;
	    cerr << "Old stack: " << *oldStack << endl;
	    cerr << "New stack: " << *newStack << endl;
	    *threadStacks[i] = *newStack;
	}
	delete newStack;
    }
    
    /* Let's get the stack for the newly allocated thread */
    if(tid)
    {
	pid_t pin_tid = PIN_ThreadId();
	Stack *stack = get_thread_stack(pid, tid);

	if(threadStacksSize < (size_t)(pin_tid + 1))
	    growThreadStacks(pin_tid+1);
	threadStacks[pin_tid] = stack;
	if(stack)
	    cerr << "Stack " << *stack << " associated with thread " << 
		pin_tid << endl; 
	else
	    cerr << "Null stack for thread " << tid << "-" << pin_tid << endl;
    }
}


void
get_process_stack(pid_t pid)
{
    FILE *cmd_output;
    char *line = NULL;
    ssize_t bytes_read = 0;
    size_t len = 0;

    stringstream pmap_stream;
    pmap_stream << "pmap -d " << pid;
    string pmap_command = pmap_stream.str();

    /* This will execute the pmap command and pipe
     * the output to a file handle cmd_output.
     */
    cmd_output = popen(pmap_command.c_str(), "r");
    if(cmd_output == NULL)
    {
	cerr << "Could not read output from command: "<< pmap_command << endl;
	return;
    }

    /* Let's read the output line by line until we find the
     * process stack region. Once we do, parse the base and
     * length.
     */
    bool foundStack = false;
    while(bytes_read != -1 && !foundStack)
    {
	bytes_read = getline(&line, &len, cmd_output);
	if(bytes_read > 0)
	{
	    string lineStr(line);
	    if(lineStr.find("[ stack ]") != string::npos)
	    {
		char *end_ptr = 0;

		/* The first value should be the stack base, 
		 * the second is stack size in KB.
		 */
		size_t process_stack_base = strtol(line, &end_ptr, 16);
		size_t process_stack_size = strtol(end_ptr, NULL, 10);

		cerr << "Process stack base is: 0x" << hex 
		     << process_stack_base << dec << endl; 
		cerr << "Process stack size is: " << process_stack_size << "K" << endl;
		
		processStack.start = process_stack_base;
		processStack.end = process_stack_base + process_stack_size * KILOBYTE;
		processStack.tid = pid;
		foundStack = true;
	    }
	}
	if(len > 0)
	{
	    free(line);
	    line = NULL;
	}
    }

    pclose(cmd_output);
    return;

}



/* ===================================================================== */
/* eof */
/* ===================================================================== */
