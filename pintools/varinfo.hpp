/// Class that provides information about variables types
/// in a program compiled with '-g' key. Information is
/// extracted from a non-striped binary using libelf and
/// libdwarf.
///
/// Nik Zaborovsky, Sep - 2014 
///
#pragma once

#include <map>
#include <string>
#include <memory>
#include "varinfo_i.hpp"


class VarInfo : public IVarInfo {
public:
	VarInfo();

	/// \!brief Constructs variables data base by a binary file.
	bool init(const std::string& file);

	/// \!brief Returns variable base type given its occurence in the file and its name.
	const std::string type(const std::string& file, const size_t line, const std::string& name) const;

	const std::string fieldname(const std::string& file, const size_t line, const std::string& name, const unsigned offset) const;

private:
	VarInfo(const VarInfo&);
	VarInfo& operator=(const VarInfo&);

private:
	std::string _file;

	class Imp;	
	const std::auto_ptr<Imp> _imp;
};
