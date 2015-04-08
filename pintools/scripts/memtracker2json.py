#!/usr/bin/python 

from os import system
import os.path
import re
import sys
from sys import stdin
import argparse
import ctypes
import commands

def notValidHex(num):
    
    try:
        int(num, 16)
        return False;
    except:
        return True;


class AllocRecord:

    def __init__(self, threadID, addr, funcName, size, numItems, 
                 sourceLoc, varName, varType):
        self.threadID = threadID;
        self.addr = addr;
        self.funcName = funcName;
        self.size = size;
        self.numItems = numItems;
        self.sourceLoc = sourceLoc;
        self.varName = varName;
        self.varType = varType;

    def non_json_str(self):
        return ("alloc: " + self.threadID + " " + self.addr + " " 
                + self.funcName + " " + self.size + " " 
                + self.numItems + " " + self.sourceLoc + " " 
                + self.varName + " " + self.varType);

    def __str__(self):
        return ("{\"event\": \"allocation\", " 
                "\"thread-id\": \"" + self.threadID + "\", "
                "\"alloc-base\": \"" + self.addr + "\", "
                "\"type\": \"" + self.funcName + "\", "
                "\"alloc-size\": \"" + self.size + "\", "
                "\"num-items\": \"" + self.numItems + "\", "
                "\"source-location\": \"" + self.sourceLoc + "\", "
                "\"var-name\": \"" + self.varName + "\", "
                "\"var-type\": \"" + self.varType + "\"}");

class FreeRecord:

    def __init__(self, addr):
        self.addr = addr;

    def __str__(self):
        return ("{\"event\": \"implicit-free\", "
                "\"base\": \"" + self.addr + "\"}");


class AccessRecord:

    def __init__(self, accessType, threadID, addr, size, funcName, sourceLoc,  
                 varName, varType, allocLoc):

        self.accessType = accessType;
        self.threadID = threadID;
        self.addr = addr;
        self.size = size;
        self.funcName = funcName;
        self.sourceLoc = sourceLoc;
        self.varName = varName;
        self.varType = varType;
        self.allocLoc = allocLoc;


    def non_json_str_(self):
        return (self.accessType + " "  + self.threadID + " " + self.addr + " " 
                + self.size + " " + self.funcName + " " + self.sourceLoc + " "
                + self.varName + " " + self.varType + " "
                + self.allocLoc);

    def __str__(self):
        return("{\"event\": \"memory-access\", " 
               "\"type\": \"" + self.accessType + "\", "
               "\"thread-id\": \"" + self.threadID + "\", "
               "\"address\": \"" + self.addr + "\", "
               "\"size\": \"" + self.size + "\", "
               "\"function\": \"" + self.funcName + "\", "
               "\"source-location\": \"" + self.sourceLoc + "\", "
               "\"alloc-location\": \"" + self.allocLoc + "\", "
               "\"var-name\": \"" + self.varName + "\", "
               "\"var-type\": \"" + self.varType + "\"}");


class FuncRecord:

    def __init__(self, eventType, threadID, funcName):
        self.eventType = eventType;
        self.threadID = threadID;
        self.funcName = funcName;

    def non_json_str(self):
        return (self.eventType + " " + self.threadID + " " +
                self.funcName);

    def __str__(self):
        return("{\"event\": \"" + self.eventType + "\", " 
               "\"thread-id\": \"" + self.threadID + "\", "
               "\"name\": \"" + self.funcName + "\"}");


def parseAlloc(line, out):

    threadID = "-";
    addr = "-";
    funcName = "-";
    size = "-";
    numItems = "-";
    sourceLoc = "-";
    varName = "-";
    varType = "-";

    words = line.split(" ");
            
    for i in range(0, len(words)):
        if(i == 0):
            continue;
        if(i == 1):
            threadID = words[i];
        if(i == 2):
            addr = words[i];
        if(i == 3):
            funcName = words[i];
        if(i == 4):
            size = words[i];
        if(i == 5):
            numItems = words[i];
        if(i == 6):
            sourceLoc = words[i];
        if(i == 7):
            varName = words[i];
        if(i == 8):
            varType = words[i];

    
    r = AllocRecord(threadID, addr, funcName, size, numItems,
                sourceLoc, varName, varType);

    out.write(str(r) + "\n");


def parseMemoryAccess(line, out):

    accessType = "-";
    threadID = "-";
    addr = "-";
    size = "-";
    funcName = "-";
    sourceLoc = "-";
    varName = "-";
    varType = "-";
    allocLoc = "-";


    words = line.split(" ");
            
    for i in range(0, len(words)):
        if(i == 0):
            accessType = words[i].strip(':');
        if(i == 1):
            threadID = words[i];
        if(i == 2):
            addr = words[i];
        if(i == 3):
            size = words[i];
        if(i == 4):
            funcName = words[i];
        if(i == 5):
            sourceLoc = words[i];
        if(i == 6):
            allocLoc = words[i];
        if(i == 7):
            varName = words[i];
        if(i == 8):
            varType = words[i]; 
        if(i > 8):
            varType = varType + " " + words[i]; 



    r = AccessRecord(accessType, threadID, addr, size, funcName, sourceLoc,
                     varName, varType, allocLoc);
    
    out.write(str(r) + "\n");



def parseFunction(line, out):

    eventType = "";
    threadID = "-";
    funcName = "-";
    
    words = line.split(" ");
            
    for i in range(0, len(words)):
        if(i == 0):
            eventType = words[i].strip(':');
        if(i == 1):
            threadID = words[i];
        if(i == 2):
            funcName = words[i];

    r = FuncRecord(eventType, threadID, funcName);

    out.write(str(r) + "\n");
    
def parseFree(line, out):

    words = line.split(" ");
    
    if(len(words) < 2):
        sys.stderr.write("implicit-free record without the base parameter");
        return

    r = FreeRecord(words[1]);

    out.write(str(r) + "\n");


def parseLine(line, keepdots, outputstream):

    if(not keepdots):
        if ".plt" in line:
            return

        if ".text" in line:
            return

    line = line.rstrip();

    if line.startswith("alloc:"):
        parseAlloc(line, outputstream);
    if line.startswith("read:") or line.startswith("write:"):
        parseMemoryAccess(line, outputstream);
    if line.startswith("function-begin") or line.startswith("function-end"):
        parseFunction(line, outputstream);
    if line.startswith("implicit-free"):
        parseFree(line, outputstream);



#
# First just parse and convert as-is. Later, we might want to match 
# memory-access records with corresponding allocation records and convert
# memory-access data with the information that the allocation gives us.
#
def parse(fdTrace, keepdots, outputstream):

    for line in fdTrace:
        parseLine(line, keepdots, outputstream);

    

###
def main():

    parser = argparse.ArgumentParser(description='Convert memtracker trace to JSON.')
    parser.add_argument('--infile', 
                        help='Name of the trace file generated by the memtracker pintool. By default the trace is read from stdin.')
    parser.add_argument('--keepdots', action='store_true', 
                        help='Do not skip records from .text and .plt when generating trace');


    args = parser.parse_args()
    

    if args.infile is None:
        parse(sys.stdin, args.keepdots, sys.stdout);
    else:
        if not os.path.exists(args.infile):
            print 'File ' + args.infile + ' does not exist.';
            sys.exit(1);

        fdTrace = open(args.infile, "r");
        parse(fdTrace, args.keepdots, sys.stdout);

if __name__ == '__main__':
    main()
