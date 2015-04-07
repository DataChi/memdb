#include <stdio.h>
#include <stdlib.h>
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
        if (acle.type == 'w') {
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
    }

//map<void *, packedvalue> packedvalues; // first is base

    for (auto it = valuemap.begin(); it != valuemap.end(); it++) {
        void * key = it->first;
        auto fieldmap = it->second;

        int uniqueCount = 0;
        for (auto it2 = fieldmap.begin(); it2 != fieldmap.end(); it2++) {
            if (it2->second.unique) {
                uniqueCount++;
            }
        }
        packedvalue pv;
        pv.nelem = uniqueCount;
        pv.values = (uint64_t *) malloc(pv.nelem * sizeof(uint64_t));

        int i = 0;
        for (auto it2 = fieldmap.begin(); it2 != fieldmap.end(); it2++) {
            if (it2->second.unique) {
                pv.values[i++] = it2->second.value;
            }
        }
        
        packedvalues[key] = pv;
    }

    logfiles[LOG_ACCESS].clear();
    logfiles[LOG_ACCESS].seekg(0, logfiles[LOG_ACCESS].beg);

    map<void *, ofstream *> outfiles;
    int i = 0;
    for (auto it = allocPointSet.begin(); it != allocPointSet.end(); it++) {
        char filename[16];
        snprintf(filename, 15, "out%04d.dat", i++);
        cout << *it << " " << filename << endl;
        ofstream *temp = new ofstream();
        temp->open(filename, ios::out);
        outfiles[*it] = temp;
    }

    while (logfiles[LOG_ACCESS]) {
        logfiles[LOG_ACCESS].read((char*)&acle, sizeof(AccessLogEntry));  
        if (allocmap.count(acle.allocBase) != 0) {
            void *outKey = allocmap[acle.allocBase].allocPoint;
            *(outfiles[outKey]) << acle.time << ", " << acle.allocBase << ": ";
            packedvalue pv = packedvalues[acle.allocBase];
            for (i = 0; i < pv.nelem; i++) {
                *(outfiles[outKey]) << pv.values[i] << " ";
            }
            *(outfiles[outKey]) << endl;
        }
    }
    for (auto it = outfiles.begin(); it != outfiles.end(); it++) {
        it->second->flush();
        it->second->close();
    }
    
    return 0;
}
