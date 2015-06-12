/*
 * This tool reads a file containing a memtracker trace, the text version. 
 * The trace has the following format:
 * <access_type> <tid> <addr> <size> <func> <access_source> <alloc_source> <name> <type>
 *
 * It runs the trace through a very simple cache simulator, whose size, associativity
 * and the block-size can be configured via command line options, and for each evicted
 * cache line outputs a record showing:
 * - The number of bytes that were used in that cache line between the time it was
 *   created and evicted.
 * - The number of times the cache line was reused. 
 * - The source code location, which caused this cache line to be created in the cache.
 * - The information on the variable that was accessed upon the faulting access. 
 */

#include <sys/types.h>
#include <assert.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/time.h>
#include <stdio.h>
#include <sstream> 
#include <map>
#include <utility>
#include <bitset>
#include <unistd.h>
#include <sys/syscall.h>
#include <ctgmath>
#include <stdlib.h>
#include <algorithm>
#include <unordered_map>
#include <tuple>

using namespace std;

#define VERBOSE 0

bool WANT_RAW_OUTPUT = 0;

#define MAX_LINE_SIZE 64 /* We need this in order to use the bitset class */

/* Default cache size parameters for a 2MB 4-way set associative cache */
int NUM_SETS = 8*1024;
int ASSOC = 4; /* 4-way set associative */
int CACHE_LINE_SIZE = 64;  /* in bytes */

/* If percent cache line utilization is below that value, 
 * we say that the cache line had a low utilization. 
 */
const float LOW_UTIL_THRESHOLD = 0.5;

/* These values are computed once the cache parameters are set */
int lineOffsetBits;
int indexBits;
int tagMaskBits;

/* The following data structures are used to summarize
 * the cache waste per source location. 
 */
class WasteRecord
{
public:
    string varInfo;
    size_t address;

    WasteRecord(string vI = "", size_t addr=0)
	: varInfo(vI), address(addr){}
};
    

class ZeroReuseRecord: public WasteRecord
{
public:
    ZeroReuseRecord(string vI, size_t addr)
	{
	    varInfo = vI;
	    address = addr;
	}

    friend std::ostream& operator<< (std::ostream& stream, const ZeroReuseRecord& zrr)
	{
	    cout << "\t" << zrr.varInfo << endl;
	    cout << "\t0x" << hex << zrr.address << dec << endl;
	}
};

class LowUtilRecord: public WasteRecord
{
public:
    int byteUseCount;

    LowUtilRecord(string vI, size_t addr, int bC)
	{
	    this->varInfo = vI;
	    this->byteUseCount = bC;
	    this->address = addr;
	}

    friend std::ostream& operator<< (std::ostream& stream, const LowUtilRecord& lur)
	{
	    cout << "\t--------------------------------------------" << endl;
	    cout << "\t" << lur.varInfo << endl;
	    cout << "\t0x" << hex << lur.address << dec << endl;
	    cout << "\t" << lur.byteUseCount << "/" << CACHE_LINE_SIZE << endl;
	}

};

unordered_multimap <string, ZeroReuseRecord> zeroReuseMap;
unordered_multimap <string, LowUtilRecord> lowUtilMap;

multimap <int, tuple<string, vector<ZeroReuseRecord>>> groupedZeroReuseMap;
multimap <int, tuple<string, vector<LowUtilRecord>>> groupedLowUtilMap;

/***************************************************************************
 * BEGIN CACHE SIMULATION CODE
/****************************************************************************/

/* These data structures relate to cache simulation. 
 * For each line in the cache, we keep track of the address,
 * the corresponding source code location that performed the
 * access and whatever we know about the variable name and type, 
 * the number of bytes that were actually used before the cache
 * line was evicted and how many times the cache lines was used
 * before being evicted. 
 */

class CacheLine
{
    int lineSize;     /* In bytes */
public:
    size_t address;    /* virtual address responsible for populating this cache line */
    size_t tag; 
    string accessSite; /* which code location caused that data to be brought 
			* into the cache line? */
    unsigned short initAccessSize; /* The size of the access that brought 
				      this line into cache */
    string varInfo;    /* the name and the type of the corresponding variable,
			* if we know it. */
    bitset<MAX_LINE_SIZE> *bytesUsed; /* This is a bitmap. There is a bit for each byte in the
			* cache line. If a byte sitting in the cache line is
			* accessed by the user program, we mark it as "accessed"
			* by setting the corresponding bit to "1".
			*/
    size_t timeStamp;  /* Virtual time of access */
    unsigned short timesReusedBeforeEvicted;

    /* CacheLine constructor does not take any arguments and instead we set 
     * the parameters from the globals. That enables us to 
     * allocate an entire array of cache lines. That allocation
     * relies on zero-argument constructor and does not work
     * with constructors that take arguments. 
     */
    CacheLine() /* Size is given in bytes */
	{
	    lineSize = CACHE_LINE_SIZE;
	    address = 0;
	    tag = 0;
	    initAccessSize = 0;
	    accessSite = "";
	    varInfo = "";
	    timesReusedBeforeEvicted = 0;
	    timeStamp = 0;
	    bytesUsed = new bitset<MAX_LINE_SIZE>(lineSize); 
	    bytesUsed->reset();
	}

    /* Print info about the access that caused this line to be 
     * brought into the cache */
    void printFaultingAccessInfo()
	{
	    cout<< "0x" << hex << address << dec << " " << initAccessSize << " "
		<< accessSite << varInfo << endl;
	}

    void setAndAccess(size_t address, unsigned short accessSize, 
		      string accessSite, string varInfo, size_t timeStamp)
	{
	    this->address = address;
	    this->initAccessSize = accessSize;
	    tag = address >> tagMaskBits;
	    this->accessSite = accessSite;
	    this->varInfo = varInfo;
	    timesReusedBeforeEvicted = 0;
	    bytesUsed->reset();

	    access(address, accessSize, timeStamp);
	}

    bool valid(size_t address)
	{
	    if(address >> tagMaskBits == tag)
		return true;
	    
	    return false;
	}

    /* Set to '1' the bits corresponding to this address
     * within the cache line, to mark the corresponding bytes
     * as "accessed".
     * If those bits are already marked as accessed, we increment
     * the reuse counter.
     */
    void access(size_t address, unsigned short accessSize, size_t timeStamp)
	{
	    int lineOffset = address % lineSize;
	    
	    assert(valid(address));
	    assert(lineOffset + accessSize <= lineSize);
	    
	    this->timeStamp = timeStamp;

	    /* We only check if the first bit is set, assuming that if
	     * we access the same valid address twice, the data represents
	     * the same variable (and thus the same access size) as before
	     */
	    if(bytesUsed->test(lineOffset))
		timesReusedBeforeEvicted++;
	    else
	    {
		for(int i = lineOffset; i < min(lineOffset + accessSize, lineSize); i++)
		    bytesUsed->set(i);
	    }
	}    
    
    void evict()
	{

	    /* We are being evicted. Print our stats, update waste maps and clear. */
	    if(WANT_RAW_OUTPUT)
	    {
		cout << bytesUsed->count() << "\t" << timesReusedBeforeEvicted 
		     << "\t" << accessSite << "[" << varInfo << "]\t" 
		     << "0x" << hex << address << dec << endl;
	    }

	    if(timesReusedBeforeEvicted == 0)
	    {
		zeroReuseMap.insert(pair<string, ZeroReuseRecord>
				    (accessSite, 
				     ZeroReuseRecord(varInfo, address)));
	    }
	    if((float)(bytesUsed->count()) / (float)lineSize < LOW_UTIL_THRESHOLD)
	    {
		lowUtilMap.insert(pair<string, LowUtilRecord>
				  (accessSite, 
				   LowUtilRecord(varInfo, address, bytesUsed->count())));
	    }


	    address = 0;
	    tag = 0;
	    accessSite = "";
	    varInfo = "";
	    timesReusedBeforeEvicted = 0;
	    bytesUsed->reset();
	}

    void printParams()
	{
	    cout << "Line size = " << lineSize << endl;
	}

	   
};


class CacheSet
{
public:
    int assoc, lineSize;
    CacheLine *lines;
    size_t curTime; /* a virtual time ticks every time someone
		     * accesses this cache set. */
public:
    /* CacheSet constructor does not take any arguments and instead we set 
     * the parameters from the globals. That enables us to 
     * allocate an entire array of cache sets. That allocation
     * relies on zero-argument constructor and does not work
     * with constructors that take arguments. 
     */
    CacheSet()
	{
	    assoc = ASSOC;
	    lineSize = CACHE_LINE_SIZE;
	    curTime = 0;
	    this->assoc = assoc;
	    lines = new CacheLine[assoc];
	    
	}

    /* Find a cache line to evict or return a clean time 
     * For evictions we use the true LRU policy based on 
     * virtual timestamps. 
     */
    CacheLine * findCleanOrVictim(size_t timeNow)
	{
	    size_t minTime = curTime, minIndex = -1;
#if VERBOSE
	    cout << "Looking for eviction candidate at time " << timeNow << endl;
#endif

	    /* A clean line will have a timestamp of zero, 
	     * so it will automatically get selected. 
	     */
	    for(int i = 0; i < assoc; i++)
	    {
		if(lines[i].timeStamp < minTime)
		{
		    minTime = lines[i].timeStamp;
		    minIndex = i;
		}
#if VERBOSE
		cout << "block "<< i << " ts is " << lines[i].timeStamp << endl;
#endif
	    }
	    assert(minIndex >= 0);

#if VERBOSE
	    cout << "Eviction candidate is block " << minIndex << endl;
#endif
	    /* Evict the line if it's not empty */
	    if(lines[minIndex].timeStamp != 0)
		lines[minIndex].evict();

	    return &(lines[minIndex]);
	}

    /* See if any of the existing cache lines hold
     * that address. If so, access the cache line. 
     * Otherwise, find someone to evict and populate
     * the cache line with the new data.
     * Return true on a hit, false on a miss. 
     */
    bool access(size_t address, unsigned short accessSize, 
		string accessSite, string varInfo)
	{
	    curTime++;

	    for(int i = 0; i < assoc; i++)
	    {
		if(lines[i].valid(address))
		{
		    lines[i].access(address, accessSize, curTime);
		    return true;
		}
	    }
	    
	    /* If we are here, we did not find the data in cache.
	     * See if there is an empty cache line or find someone to evict. 
	     */
	    CacheLine *line = findCleanOrVictim(curTime);
	    line->setAndAccess(address, accessSize, accessSite, varInfo, curTime);
	    return false;
	}

    void printParams()
	{
	    cout << "Associativity = " << assoc << endl;
	    cout << "Line size = " << lineSize << endl;
	}
};

class Cache
{
public:
    int numSets;
    int assoc;
    int lineSize;
    CacheSet *sets;
    int numMisses, numHits;


    Cache(int ns, int as, int ls)
	: numSets(ns), assoc(as), lineSize(ls)
	{
	    sets = new CacheSet[numSets];
	    numMisses = 0;
	    numHits = 0;

	    /* This is by how many bits we have to shift the
	     * address to compute the tag. */
	    tagMaskBits = log2(lineSize) + log2(numSets);
	}

    void access(size_t address, unsigned short accessSize, 
		string accessSite, string varInfo)
	{
	    /* See if the access spans two cache lines.
	     */
	    int lineOffset = address % lineSize;
	    
	    if(lineOffset + accessSize <= lineSize)
	    {
		__access(address, accessSize, accessSite, varInfo);
		return;
	    }

	    /* If we are here, we have a spanning access.
	     * Determine the address of the first byte that 
	     * spills into another cache line. 
	     */
	    uint16_t bytesFittingIntoFirstLine = lineSize - lineOffset;
	    size_t addressOfFirstByteNotFitting = 
		address + bytesFittingIntoFirstLine;
	    uint16_t sizeOfSpillingAccess = accessSize - bytesFittingIntoFirstLine;
#if VERBOSE
	    cerr << "SPANNING ACCESS: 0x" << hex << address 
		 << dec << " " << accessSize << " " << accessSite 
		 << " " << varInfo << endl;
	    cerr << "Split into: " << endl;
	    cerr << "\t0x" << hex << address << dec << " " 
		 << bytesFittingIntoFirstLine << endl;
	    cerr << "\t0x" << hex << addressOfFirstByteNotFitting << dec << " " 
		 << sizeOfSpillingAccess << endl;
#endif


	    /* Split them into two accesses */
	    __access(address, bytesFittingIntoFirstLine, accessSite, varInfo);

	    /* We recursively call this function in case the spilling access 
	     * spans more than two lines. */
	    access(addressOfFirstByteNotFitting, sizeOfSpillingAccess, 
		   accessSite, varInfo);
	    
	}

    void printParams()
	{
	    cout << "Line size      = " << lineSize << endl;
	    cout << "Number of sets = " << numSets << endl;
	    cout << "Associativity  = " << assoc << endl;
	}

    void printStats()
	{
	    cout << "Number of hits: " << numHits << endl;
	    cout << "Number of misses: " << numMisses << endl;
	}
    
private:
    /* Here we assume that accesses would not be spanning cache
     * lines. The calling function should have taken care of this.
     */
    void __access(size_t address, unsigned short accessSize, 
		string accessSite, string varInfo)
	{
	    /* Locate the set that we have to access */
	    int setNum = (address >> (int)log2(lineSize)) % numSets;
	    
	    assert(setNum < numSets);
#if VERBOSE
	    cout << hex << address << dec << " maps into set #" << setNum << endl;
#endif
	    bool hit = sets[setNum].access(address, accessSize, accessSite, varInfo);
	    if(hit)
		numHits++;
	    else
		numMisses++;
	}

};
/***************************************************************************
 * END CACHE SIMULATION CODE
/****************************************************************************/

void parseAndSimulate(string line, Cache *c)
{
    istringstream str(line);
    string word;
    size_t address;
    unsigned short accessSize;
    string accessSite;
    string varInfo;

    /* Let's determine if this is an access record */
    if(!str.eof())
    {
	str >> word;
	if(!(word.compare("read:") == 0) && !(word.compare("write:") == 0))
	    return;
    }

    /* We are assuming the memtracker trace output, the text 
     * version. It has the following format:
     * <access_type> <tid> <addr> <size> <func> <access_source> <alloc_source> <name> <type>
     */
    int iter = 1;
    while(!str.eof())
    {
	str >> word;

	switch(iter++)
	{
	case 1:	    // Skip the tid
	    break;
	case 2:     // Parse the address
	    address = strtol(word.c_str(), 0, 16);
	    if(errno == EINVAL || errno == ERANGE)
	    {
		cerr << "The following line caused error when parsing address: " << endl;
		cerr << line << endl;
		exit(-1);
	    }
	    break;
	case 3:     // Parse the size
	    accessSize = (unsigned short) strtol(word.c_str(), 0, 10);
	    if(errno == EINVAL || errno == ERANGE)
	    {
		cerr << "The following line caused error when parsing access size: " << endl;
		cerr << line << endl;
		exit(-1);
	    }
	    break;
	case 4: 
	case 5: 
	    accessSite += word + " ";
	    break;
	case 6: 
	case 7: 
	case 8:
	    varInfo += word + " ";
	    break;
	}
    }

#if VERBOSE
    cout << line << endl;
    cout << "Parsed: " << endl;
    cout << hex << "0x" << address << dec << endl;
    cout << accessSize << endl;
    cout << accessSite << endl;
    cout << varInfo << endl;
#endif

    c->access(address, accessSize, accessSite, varInfo);

}

/***************************************************************************
 * BEGIN DATA ANALYSIS CODE
/****************************************************************************/

/*
 * These functions summarize the maps of waste records, so we can 
 * display them in a user-friendly way. We will group the records
 * by source code line(access site) and display them in the order of decreasing
 * waste occurrences.
 */
template <class T>
void summarizeWasteMap(unordered_multimap<string, T> &ungroupedMap,
		       multimap<int, tuple<string, vector<T>>> &groupedMap)
{

    /* Iterate the map. Once we encounter a new source line,
     * count the number of its associated waste records, 
     * put that in the summarized map, where the count is the key, 
     * and the value is the list (vector) of associated waste records.
     */
    for(auto it = ungroupedMap.begin(); it != ungroupedMap.end(); it++) 
    {

	vector<T> curVector;	
	string curAccessSite = it->first;

	do 
	{
	    curVector.push_back(it->second);
	    ++it;
	} while (it != ungroupedMap.end() && curAccessSite.compare(it->first)==0);
	
	tuple<string, vector<T>> gRecs = make_tuple(curAccessSite, curVector); 
	groupedMap.insert(make_pair(curVector.size(), gRecs));	

	if(it == ungroupedMap.end())
	  break;
    }
}

template <class T>
void printSummarizedMap(multimap<int, tuple<string, vector<T>>> & groupedMap)
{
    for(auto it = groupedMap.rbegin(); it != groupedMap.rend(); it++) 
    {
        cout << it->first << " waste occurrences" << endl;

	tuple<string, vector<T>> gRecs = it->second;

	string accessSite = get<0>(gRecs); 
	vector<T> recs = get<1>(gRecs); 

        cout << accessSite << endl;
	for(int i = 0; i < recs.size(); i++)
	    cout << recs[i] << endl;
    }
}

/***************************************************************************
 * END DATA ANALYSIS CODE
/****************************************************************************/

int main(int argc, char *argv[])
{
    char *fname = NULL;
    char *nptr;
    char c;
    ifstream traceFile;

    
    /* Right now we don't check that the number of sets
     * and the cache line size are a power of two, but
     * we probably should. 
     */
    while ((c = getopt (argc, argv, "a:f:l:s:r")) != -1)
	switch(c)
	{
	case 'a': /* Associativity */
	    ASSOC = (int)strtol(optarg, &nptr, 10);
	    if(nptr == optarg && ASSOC == 0)
	    {
		cerr << "Invalid argument for associativity: " 
		     << optarg << endl;
		exit(-1);
	    }
	    else
		cout << "Associativity set to "<< ASSOC << endl;
	    break;
	case 'f':
	    fname = optarg;
	    break;
	case 'l':
	    CACHE_LINE_SIZE = strtol(optarg, &nptr, 10);
	    if(nptr == optarg && CACHE_LINE_SIZE == 0)
	    {
		cerr << "Invalid argument for the cache line size: " 
		     << optarg << endl;
		exit(-1);
	    }
	    else
		cout << "Associativity set to "<< CACHE_LINE_SIZE << endl;
	    break;
	case 'r':
	    WANT_RAW_OUTPUT = true;
	    break;
	case 's': /* Number of cache sets */
    	    NUM_SETS = (int)strtol(optarg, &nptr, 10);
	    if(nptr == optarg && NUM_SETS == 0)
	    {
		cerr << "Invalid argument for the number of cache sets: " 
		     << optarg << endl;
		exit(-1);
	    }
	    else
		cout << "Number of cache sets set to "<< NUM_SETS << endl;
	    break;
	case '?':
	default:
	    cerr << "Unknown option or missing option argument." << endl;
	    exit(-1);
	}


    if(fname == NULL)
    {
	cerr << "Please provide input trace file with the -f option." << endl;
	exit(-1);
    }

    Cache *cache = new Cache(NUM_SETS, ASSOC, CACHE_LINE_SIZE);
    cache->printParams();
 
    /* Let's open the trace file */
    traceFile.open(fname);
    if(!traceFile.is_open())
    {
	cerr << "Failed to open file " << fname << endl;
	exit(-1);
    }

    string line;

    /* Read the input line by line */
    while(!traceFile.eof())
    {
	getline(traceFile, line);
	parseAndSimulate(line, cache);
    }
    
    /* Print the waste maps */
    cout << "*************************************************" << endl;
    cout << "               ZERO REUSE MAP                    " << endl;
    cout << "*************************************************" << endl;

    for(auto it = zeroReuseMap.begin(); it != zeroReuseMap.end(); it++) 
    {
        cout << it->first << endl;
        cout << (ZeroReuseRecord&) it->second << endl;

    }

    cout << endl;
    
    /* Print the waste maps */
    cout << "*************************************************" << endl;
    cout << "               LOW UTILIZATION MAP               " << endl;
    cout << "*************************************************" << endl;

    for(auto it = lowUtilMap.begin(); it != lowUtilMap.end(); it++) 
    {
        cout << it->first << endl;
        cout << (LowUtilRecord&)it->second << endl;

    }

    summarizeWasteMap<ZeroReuseRecord>(zeroReuseMap, groupedZeroReuseMap);
    cout << "*************************************************" << endl;
    cout << "         ZERO REUSE MAP SUMMARIZED               " << endl;
    cout << "*************************************************" << endl;
    printSummarizedMap<ZeroReuseRecord>(groupedZeroReuseMap);

    summarizeWasteMap<LowUtilRecord>(lowUtilMap, groupedLowUtilMap);
    cout << endl;
    cout << "*************************************************" << endl;
    cout << "         LOW UTILIZATION MAP SUMMARIZED          " << endl;
    cout << "*************************************************" << endl;
    printSummarizedMap<LowUtilRecord>(groupedLowUtilMap);

}




