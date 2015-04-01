#include <iostream>
#include <fstream>
#include <map>
#include <set>

#include "../../binarylogging.hpp"

using namespace std;

enum {
    LOG_FUNC,
    LOG_ALLOC,
    LOG_ACCESS,
    NR_LOGS
};

typedef struct fieldvalue_ {
    bool unique;
    uint64_t value;
} fieldvalue;

typedef struct packedvalue_ {
    int nelem;
    uint64_t timestamp; // always changing, used when processing accesses
    uint64_t *values;
} packedvalue;

map<void *, map<int, fieldvalue>> valuemap; // first is base
map<void *, packedvalue> packedvalues; // first is base
set<void *> allocPointSet;

ifstream logfiles[NR_LOGS];

string logpath = "../../";

int main() {
    string path = logpath + string("log_func.dat");
    logfiles[LOG_FUNC].open(path.c_str(), ios::in | ios::binary);
    path = logpath + string("log_access.dat");
    logfiles[LOG_ACCESS].open(path.c_str(), ios::in | ios::binary);
    path = logpath + string("log_alloc.dat");
    logfiles[LOG_ALLOC].open(path.c_str(), ios::in | ios::binary);

    AccessLogEntry acle;
    AllocLogEntry alle;
    map<void *, AllocLogEntry> allocmap;

    while (logfiles[LOG_ALLOC]) {
        logfiles[LOG_ALLOC].read((char*)&alle, sizeof(AllocLogEntry));  

        // process allocs
        std::pair<void *, AllocLogEntry> value(alle.addr, alle);
        //allocmap[alle.addr] = alle;
        allocmap.insert(value);
        allocPointSet.insert(alle.allocPoint);
    }

    // get field values
    while (logfiles[LOG_ACCESS]) {
        logfiles[LOG_ACCESS].read((char*)&acle, sizeof(AccessLogEntry));  

//map<void *, map<int, fieldvalue>> valuemap; // first is base
//set<void *> allocPointSet;
        // process access
        if (allocmap.count(acle.allocBase) != 0) {
            AllocLogEntry alloc = allocmap[acle.allocBase];
            uint64_t offset = (uint64_t)acle.addr - (uint64_t)alloc.addr;
            if (valuemap[acle.allocBase].count(offset) == 0) {
                fieldvalue fv;
                fv.unique = true;
                fv.value = acle.value;
                valuemap[acle.allocBase][offset] = fv;
            } else {
                valuemap[acle.allocBase][offset].unique = false;
            }
        }
    }

//map<void *, packedvalue> packedvalues; // first is base

    for (auto it = valuemap.begin(); it != valuemap.end(); it++) {

    }

    
    return 0;
}
