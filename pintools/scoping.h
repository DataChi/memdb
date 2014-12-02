/// Simple scopes parser for C/C++ programs where scopes are represented
/// as pairs of opened and closed brackets '{..}'.
///
/// Sep - 2014, Nik Zaborovsky
#pragma once
#include <map>
#include <string>
#include <vector>
#include <cassert>


struct scoping {
	enum {NO_END_LINE = -1};
	bool init(const std::vector<std::string>& /*srcfiles*/,
		const std::string& paths_prefix = std::string());
	int endline(const std::string& file, int startline) const {
		assert(scope_t() != _scopes.at(file) && "Scoping: no file");
		if (0 == _scopes.at(file).at(startline))
			return NO_END_LINE;
		return _scopes.at(file).at(startline);
	}
	// 'scope' is a lexical scope which includes declline and which
	// left end is the closest to the declaration of all file scopes.
	std::pair<int, int> scope(const std::string& file, int declline) {
		const scope_t& sc = _scopes[file];
		for(auto i = sc.rbegin(); sc.rend() != i; ++i) {
			if (i->first > declline || i->second < declline) continue;
			return *i;
		}
		return std::make_pair(0, 0);
	}

	int nextScope(const std::string& file, int line) {
		const scope_t& sc = _scopes[file];
		for (auto i = sc.begin(); sc.end() != i; ++i) {
			if (i->first >= line)
			return i->first;
		}
		return 0;
	}
private:
	typedef std::map<int, int> scope_t;
	std::map<std::string, scope_t> _scopes;
	std::string _path_prefix;
};
