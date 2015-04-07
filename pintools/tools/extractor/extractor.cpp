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
map<void *, set<int>> rwIndices;

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

    // make non-unique idx signatures
    for (auto it = valuemap.begin(); it != valuemap.end(); it++) {
        void * key = it->first;
        void *outKey = allocmap[key].allocPoint;
        auto fieldmap = it->second;
        for (auto it2 = fieldmap.begin(); it2 != fieldmap.end(); it2++) {
            if (!(it2->second.unique)) {
               rwIndices[outKey].insert(it2->first);
            }
        }
    }

    for (auto it = rwIndices.begin(); it != rwIndices.end(); it++) {
        cout << it->first << ":";
        for (auto it2 = it->second.begin(); it2 != it->second.end(); it2++) {
            cout << " " << *it2;
        }
        cout << endl;
    }


    logfiles[LOG_ACCESS].clear();
    logfiles[LOG_ACCESS].seekg(0, logfiles[LOG_ACCESS].beg);

#define OF_ACCESSES 0 
#define OF_VALUES 1

    map<void *, ofstream *> outfiles[2];
    int i = 0;
    for (auto it = allocPointSet.begin(); it != allocPointSet.end(); it++) {
        char filename[20];
        snprintf(filename, 19, "out%04d_acc.dat", i);
        cout << *it << " " << filename << endl;
        ofstream *temp = new ofstream();
        temp->open(filename, ios::out);
        *temp << *it << endl;
        outfiles[OF_ACCESSES][*it] = temp;
        snprintf(filename, 19, "out%04d_val.dat", i++);
        temp = new ofstream();
        temp->open(filename, ios::out);
        *temp << *it << endl;
        outfiles[OF_VALUES][*it] = temp;
    }

    for (auto it = valuemap.begin(); it != valuemap.end(); it++) {
        void * key = it->first;
        auto fieldmap = it->second;
        void *outKey = allocmap[it->first].allocPoint;

        int uniqueCount = 0;
        *(outfiles[OF_VALUES][outKey]) << key << ":";
        for (auto it2 = fieldmap.begin(); it2 != fieldmap.end(); it2++) {
            if (rwIndices[key].count(it2->first) == 0) {
                *(outfiles[OF_VALUES][outKey]) << " (" << it2->first << ", " << it2->second.value << ")";
            }
        }
        *(outfiles[OF_VALUES][outKey]) << endl;

    }

    while (logfiles[LOG_ACCESS]) {
        logfiles[LOG_ACCESS].read((char*)&acle, sizeof(AccessLogEntry));  
        if (allocmap.count(acle.allocBase) != 0) {
            void *outKey = allocmap[acle.allocBase].allocPoint;
            *(outfiles[OF_ACCESSES][outKey]) << acle.time << ", " << acle.allocBase << endl;
        }
    }
    for (auto it = outfiles[OF_ACCESSES].begin(); it != outfiles[OF_ACCESSES].end(); it++) {
        it->second->flush();
        it->second->close();
    }
    
    return 0;
}
