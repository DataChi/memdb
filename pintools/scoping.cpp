/// Simple scopes parser for C/C++ programs where scopes are represented
/// as pairs of opened and closed brackets '{..}'.
///
/// Sep - 2014, Nik Zaborovsky
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "scoping.h"

namespace {
	struct tri_t {
			static tri_t *const make(int level, int start, int end) { return new tri_t(level, start, end); }
			int _level, _start, _end;
		private:
			tri_t(int level, int start, int end) : _level(level), _start(start), _end(end) {};
		};
}


bool scoping::init(const std::vector<std::string>& srcfiles, const std::string& paths_prefix) {
	_scopes.clear();
	_path_prefix = paths_prefix;
	static const std::string built_in = "<built-in>";
	std::vector<tri_t*> scopes;
	for (const std::string& f : srcfiles) {
		std::ifstream fstream;
		scopes.clear();
	
		std::string file_path;
		if ('/' != f[0])
			file_path = _path_prefix;
		file_path += f;
		if (0 == file_path.compare(file_path.size() - built_in.size(),
			built_in.size(), built_in.c_str()))
			continue;
		fstream.open(file_path.c_str());
		if (!fstream.is_open()) {
			
			printf("Scoping: cannot open file %s\n", file_path.c_str());
			continue;
		}
		int nesting_level = 0;
		int lineno = 0;
		struct look_for_empty_end_of_nesting_level {
			look_for_empty_end_of_nesting_level(int level) : _level(level) {};
			bool operator() (tri_t *& item) { return item->_level == _level && NO_END_LINE == item->_end; }
		private:
			const int _level;
		};

		scopes.push_back(tri_t::make(nesting_level, 1, NO_END_LINE));
		++nesting_level;
		std::string line;

		while(std::getline(fstream, line)) {
			++lineno;
			for (unsigned i = 0; i < line.size(); ++i) {
				if ('{' == line[i]) {
					scopes.push_back(tri_t::make(nesting_level, lineno, NO_END_LINE));
					++nesting_level;
				}
				else if ('}' == line[i]) {
					--nesting_level;
					auto item = std::find_if(scopes.begin(), scopes.end(), look_for_empty_end_of_nesting_level(nesting_level));
					if (scopes.end() == item) {
						printf("Closing bracked without opening one in line %d\n", lineno);
						assert(false && "Closing bracket without opening bracket");
						return false;
					}
					(*item)->_end = lineno;
				}
			}
		}
		if (1 != nesting_level) {
			printf("Not balanced brackets in file %s\n", f.c_str());
			printf("Number of not balanced brackets is: %d\n",
				nesting_level - 1);
			printf("There can be incorrect scoping in file %s\n", f.c_str());
		}
		//assert(1 == nesting_level && "Not balanced brackets");
		--nesting_level;
		scopes[0]->_end = lineno;
		fstream.close();

		for (auto &i : scopes) {
			_scopes[file_path][i->_start] = i->_end;
			delete i;
		}
	}
	return true;
}
