/// Class that provides information about variables types
/// in a program compiled with '-g' key. Information is
/// extracted from a non-striped binary using libelf and
/// libdwarf.
///
/// Nik Zaborovsky, Sep - 2014 
///

#ifdef __linux
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif // __linux

#include <stdlib.h>
#include <string.h>

#ifdef __linux
#include <fcntl.h>
#include <libelf.h>
#include <libdwarf.h>
#include <gelf.h>
#endif // __linux

#include <cstdio>
#include <memory>
#include <vector>
#include <string>
#include <cassert>
#include <sstream>
#include <algorithm>
#include <map>

#include "varinfo.hpp"
#include "scoping.h"


//#define DEBUG_PRINT

#ifdef DEBUG_PRINT
#define MY_PRINT(...)	fprintf(stderr, __VA_ARGS__)
#else
#define MY_PRINT(...)
#endif

namespace {
	// BaseTypes describe build-in and derived system data types like
	// "int", "char", etc. Base types are identified by an offset in
	//.debug_info section. Offsets are defined per compilation unit.
	struct basetype_desc {
		size_t size;
		size_t count;
		std::string name;
	};

	typedef std::map<size_t, basetype_desc> BaseTypesFile_t;
	typedef std::map<std::string, BaseTypesFile_t> BaseTypes_t;

	// int - hash(type offset + file name)
	auto hasher = std::hash<std::string>();
	struct fieldname_desc {
		size_t typeoffset;
		std::string name;
	};
	typedef std::map<unsigned, fieldname_desc> FieldsNames_t;
	typedef std::map<int, FieldsNames_t> StructFields_t;

	// BaseType suffix describes intermediate base type modifier such as const or 'pointer'
	typedef std::map<size_t, std::string> BaseTypeSuffixFile_t;
	typedef std::map<std::string, BaseTypeSuffixFile_t> BaseTypeSuffix_t;
	// SrcFiles describe source files described in .debug_info section.
	typedef std::map<size_t, std::string> SrcFiles_t;

	// @sa ::validate_member
	enum {
			VRES_NOT_ARRAY = -1,
			VRES_NESTED_STRUCTURE = -2,
			VRES_UNKNOWN = -3,
		};

	// Variables describe every variable declared in a program
	struct Variable {
		enum {VALUE_NOT_SET = -1};
		Variable(SrcFiles_t *const srcfiles,
			BaseTypes_t *const basetypes,
			BaseTypeSuffix_t *const basetypesuffix) :
			_srcfiles(srcfiles), _basetypes(basetypes),
			_basetypesuffix(basetypesuffix),
			_line(VALUE_NOT_SET), _vis_ended_line(VALUE_NOT_SET),
			_file_id(VALUE_NOT_SET), _type_offset(VALUE_NOT_SET) {};

		void setLine(size_t line) { _line = line; }
		void setFile(const std::string& file) {
			auto it = _srcfiles->end();
			for (it = _srcfiles->begin(); _srcfiles->end() != it; ++it) {
				if (file == it->second) {
					_file_id = it->first;
					break;
				}
			}
			if (_srcfiles->end() == it) {
				(*_srcfiles)[_srcfiles->size()] = file;
				_file_id = _srcfiles->size() - 1;
			}
		}
		inline void setVisEndLine(size_t vis_end_line) {
			_vis_ended_line = vis_end_line;
		};
		inline void setName(const std::string& name) { _name = name; }
		inline void setTypeOffset(size_t type_offset) {
			 _type_offset = type_offset;
		}

		inline size_t line() const { return _line; }
		inline size_t visEndsLine() const { return _vis_ended_line; }
		const std::string& file() const {
			return (*_srcfiles).at(_file_id);
		}
		inline const std::string& name() const { return _name; }
		const std::string type() const {
			size_t current_offset = _type_offset;
			static const int max_refs = 256;
			int i = max_refs;	
			int next_offset = 0;
			//printf("TYPING: %d\n", current_offset);
			std::string suffix;
			do {
				std::stringstream ss((*_basetypes)[file()][current_offset].name);
				ss >> next_offset;

				//printf("TYPING: %d\n", next_offset);
				if (ss.rdstate() & std::ios::failbit) {
					if ((*_basetypes)[file()][current_offset].name.empty())
						return "void" + (suffix.empty() ? "*" : suffix);
					else
						return (*_basetypes)[file()][current_offset].name +
							suffix;
				}
				suffix = (*_basetypesuffix)[file()][current_offset] +
					suffix;
				current_offset = next_offset;
			} while(--i > 0);
			return std::string();
		}
	
		int validate_member(const size_t in_str_offset, const size_t type_offset, const size_t nearest_field_offset) const {
			unsigned tsize = 0, tcount = 0;

			size_t current_offset = type_offset;
			static const int max_refs = 256;
			int i = max_refs;	
			int next_offset = 0;
			do {
				const auto& str = (*_basetypes)[file()][current_offset];
				if (!tcount && str.count)
					tcount = str.count;
				if (!tsize && str.size)
					tsize = str.size;
					
				std::stringstream ss(str.name);
				ss >> next_offset;
				if (0 == next_offset)
					break;;
				current_offset = next_offset;
			} while(--i > 0);
			if (0 == tcount) {
				if (in_str_offset < nearest_field_offset + tsize)
					return VRES_NESTED_STRUCTURE;
				else
					return VRES_UNKNOWN;
			}

			if ((in_str_offset < tsize * tcount) &&
				(in_str_offset % tsize == 0))
				return in_str_offset / tsize;
			return VRES_NOT_ARRAY;
		}

		inline size_t type_offset() const { return _type_offset; }
		// Go to chain of types to get to a main type
		// `typedef struct { int a, int b; } mytype;`
		const size_t get_top_offset() const {
			size_t current_offset = _type_offset;
			static const int max_refs = 256;
			int i = max_refs;	
			int next_offset = 0;
			do {
				std::stringstream ss((*_basetypes)[file()][current_offset].name);
				ss >> next_offset;
				if (0 == next_offset)
					return current_offset;
				current_offset = next_offset;
			} while(--i > 0);
			return _type_offset;
	
		}
	private:
		SrcFiles_t*		_srcfiles;
		BaseTypes_t*	_basetypes;
		BaseTypeSuffix_t* _basetypesuffix;

		size_t		_line;			// declaration line (start of the scope for the arguments)
		size_t		_vis_ended_line;// line where local visibility of the var ends
		size_t		_file_id;		// declaration file id (@sa SrcFiles_t::first)
		std::string	_name;			// variable name
		size_t		_type_offset;	// type description offset (@sa BaseTypes_t::first)
	};

	typedef std::vector<Variable> Vars_t;
};


class VarInfo::Imp {
public:
	bool init(const std::string&);

	const std::string fieldname(const std::string &file, const size_t line, const std::string &name,
		const unsigned offset) const {

		const Variable *const var = get_var(file, line, name);
		if (!var)
			return "<Unknown>";
		int hash = hasher(var->file() + std::to_string(var->get_top_offset()));
		//printf("REQUIRES: off=%d file=%s\n", var->get_top_offset(),
		//	var->file().c_str());
		
		//unsigned __off = _struct_fields[hash][offset].typeoffset;
		const auto &str = _struct_fields[hash];
		//for (auto j : str) {
		//	printf("<%u> %s\n", j.first, j.second.name.c_str());
		//}
		auto i = str.rbegin();
		for (; str.rend() != i; ++i) {
			if (i->first <= offset) break;
		}
		if (str.rend() == i)
			return "<Unknown>";
		int idx = var->validate_member(offset, i->second.typeoffset, i->first);
		if (VRES_NOT_ARRAY == idx) {
			if (i->first == offset)
				return i->second.name;
			else
				return "<Unknown>";
		}
		else if (VRES_NESTED_STRUCTURE == idx)
			return i->second.name;
		else if (VRES_UNKNOWN == idx)
			return "<Unknown>";
		return i->second.name + "[" + std::to_string(idx) + "]";
	}

	const std::string type(const std::string& file,
		const size_t line,
		const std::string& name) const {
		const Variable *const var = get_var(file, line, name);
		if (!!var)
			return var->type();
		return "<Unknown>";
	}
private:
	const Variable *const get_var(const std::string& file,
		const size_t line, const std::string& name) const {
 
		std::map<size_t, Variable*> variants;
		for (unsigned i = 0; i < _vars.size(); ++i) {
			Variable *const v = const_cast<Variable *const>(&_vars[i]);
			if (v->line() <= line && line <= v->visEndsLine() &&
				 name == v->name() && file == v->file()) {
				
				variants[v->line()] = v;
			}
		}
#ifndef __linux
#pragma warning(suppress : 4172)
#endif
		if (0 != variants.size())
			return variants.rbegin()->second;
		return 0;
	}

private:
	Variable& newVar() {
		_vars.push_back(Variable(&_src_files, &_base_types,
			&_base_type_suffix));
		return _vars[_vars.size() - 1];
	}

	void cancelVar() {
		_vars.pop_back();
	}

	basetype_desc& newBaseType(const size_t offset, const std::string& file) {
		return _base_types[file][offset];
	}


private:

	Vars_t		_vars;
	SrcFiles_t	_src_files;
	BaseTypes_t	_base_types;

	BaseTypeSuffix_t _base_type_suffix;
	mutable StructFields_t _struct_fields;


	scoping		_scoping;

	// Required to gather all info about the structure (@sa StructFields_t)
	struct TypeContainer {
		bool		_valid;
		unsigned _type_offset;
		unsigned _field_type_offset;
		std::string _fieldname;
		int			_offset;
		FieldsNames_t *_fields;
		basetype_desc *_basetype;
	};

#ifdef __linux
private:
	std::map<Dwarf_Addr, Dwarf_Unsigned> _pcaddr2line;
	std::string _file;
	std::string _comp_dir;

	int _die_stack_indent_level;	// nesting level of the current DIEs
	int _vis_start_line;			// line where the current scope starts
	int _vis_end_line;				// line where the current scope ends

	void get_attribute(
		Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attr,
		Dwarf_Attribute attr_in, int die_indent_level,
		const char *tag_name, char **srcfiles, const std::vector<std::string>& srclist, const char **const cfile,
		Dwarf_Signed cnt, Dwarf_Off parent_offset,
		Variable *const var = 0, basetype_desc *const basetype = 0,
		TypeContainer ** tcon = 0) {

		const char *v = 0;
		Dwarf_Error_s *err;

		#define SAY_AND_GO(x)	{ assert(false && x); MY_PRINT(x); }
		#define SEQ(s) (0== strcmp(v, s))

		int res = dwarf_get_AT_name(attr, &v);
		if (DW_DLV_OK != res) {
			SAY_AND_GO("Failed to get attribute's name\n");
			return;
		}

		int sres = 0;

		MY_PRINT("%*s%s : ", 2 * die_indent_level, " ", v);
		const char * form = 0;
		Dwarf_Half theform = 0;
		res = dwarf_whatform(attr_in, &theform, &err);
		if (DW_DLV_OK != res) {
			SAY_AND_GO("whatform error\n");
			goto dealloc_attr;
		}
		res = dwarf_get_FORM_name(theform, &form);
		if (DW_DLV_OK != res){
			SAY_AND_GO("failed to read form information\n");
			goto dealloc_form;
		}
		MY_PRINT("[%s]", form);

		if (SEQ("DW_AT_data_member_location")) {
			Dwarf_Block *tempb = 0;
			sres = dwarf_formblock(attr_in, &tempb, &err);
			if (DW_DLV_OK != sres) { MY_PRINT("failed to read block at attribute"); goto dealloc_form; }
//			for (unsigned u = 0; u < tempb->bl_len; ++u) {
//				MY_PRINT("%02x ", *(u + (unsigned char *)tempb->bl_data));
//			}
			short offset = 0;
			short cnt = 0;
			if (tempb->bl_len >= 3)
				cnt = *(2 + (unsigned char *)tempb->bl_data);
			if (tempb->bl_len >= 2)
				offset = *(1 + (unsigned char *)tempb->bl_data);
	
			offset %= 128;
			offset += cnt * 128;

			MY_PRINT("%d", offset);

			if (!!(*tcon) && (*tcon)->_valid) {
				(*tcon)->_offset = offset;
				(*(*tcon)->_fields)[offset].name = (*tcon)->_fieldname;
				(*(*tcon)->_fields)[offset].typeoffset = (*tcon)->_field_type_offset;
				MY_PRINT("@FIELD: [%d] off=%d field=%s fieldtype=%d\n",
					(*tcon)->_type_offset, offset,
					(*tcon)->_fieldname.c_str(),
					(*tcon)->_field_type_offset);
			}
	
			dwarf_dealloc(dbg, tempb, DW_DLA_BLOCK);

		} else if (SEQ("DW_AT_comp_dir")) {
			char *name = 0;
			sres = dwarf_formstring(attr_in, &name, &err);
			if (DW_DLV_OK != sres) { MY_PRINT("failed to read string attribute\n"); goto dealloc_form; }
			_file = std::string() + name + '/' + _file;
			*cfile = _file.c_str();	
			MY_PRINT("\"%s\" ", name);
			_comp_dir = name;
			_scoping.init(srclist, _comp_dir + '/');
			dwarf_dealloc(dbg, name, DW_DLA_STRING); 
		} else if (SEQ("DW_AT_name")) {
			char *name = 0;
			sres = dwarf_formstring(attr_in, &name, &err);
			if (DW_DLV_OK != sres) { MY_PRINT("failed to read string attribute\n"); goto dealloc_form; }
			if (0 == die_indent_level)
				_file = name;
			MY_PRINT("\"%s\" ", name);
			if (!!var) {
				var->setName(name);
				var->setVisEndLine(_vis_end_line);
			} else if (!!basetype) {
				basetype->name = name;
			}
			if (!!(*tcon) && (*tcon)->_valid) {
				(*tcon)->_fieldname = name;
			}
			dwarf_dealloc(dbg, name, DW_DLA_STRING);
		} else if (SEQ("DW_AT_decl_file") || SEQ("DW_AT_call_file")) {
			Dwarf_Signed val = 0;
			Dwarf_Unsigned uval = 0;
			sres = dwarf_formudata(attr_in, &uval, &err);
			if (DW_DLV_OK != sres) {
				sres = dwarf_formsdata(attr_in, &val, &err);
				if (DW_DLV_OK != sres) { SAY_AND_GO("failed to read data attribute\n"); goto dealloc_form; }
				uval = (Dwarf_Unsigned)val;
			}
			*cfile = srcfiles[uval - 1];
			std::string full_path = *cfile;
			if ('/' != full_path[0]) {
				full_path = _comp_dir + '/' + full_path;
				//printf("%s\n", full_path.c_str());
			}
			if (!!var)
				var->setFile(full_path);
			if (!!(*tcon) && (0 == strcmp(tag_name, "DW_TAG_structure_type") || 0 == strcmp(tag_name, "DW_TAG_class_type")
				)
			) {
			
			}
			MY_PRINT("\"%s\" ", *cfile);
		}
		else if (SEQ("DW_AT_decl_line")) {
			Dwarf_Signed val = 0;
			Dwarf_Unsigned uval = 0;
			sres = dwarf_formudata(attr_in, &uval, &err);
			if (DW_DLV_OK != sres) {
				sres = dwarf_formsdata(attr_in, &val, &err);
				if (DW_DLV_OK != sres) { SAY_AND_GO("failed to read data attribute\n"); goto dealloc_form; }
				uval = (Dwarf_Unsigned)val;	
			}
			MY_PRINT("\"%lli\" ", uval);
			if (0 == strcmp(tag_name, "DW_TAG_formal_parameter") && !!var)
				uval = _scoping.nextScope(var->file(), uval);
			if (!!var)
				var->setLine(uval);
		}
		else if (SEQ("DW_AT_upper_bound") || SEQ("DW_AT_byte_size")) {
			Dwarf_Unsigned val = 0;
			sres = dwarf_formudata(attr_in, &val, &err);
			if (DW_DLV_OK != sres) { SAY_AND_GO("failed to read data attribute\n"); goto dealloc_form; }
			MY_PRINT("\"%lli\"", val);
			if (SEQ("DW_AT_byte_size") && !!basetype) {
				basetype->size = val;
			}
			if (SEQ("DW_AT_upper_bound")) {
				if (!!tcon && !!*tcon) {
					(*tcon)->_basetype->count = val;
				}
			}
		}
		else if (SEQ("DW_AT_low_pc") || SEQ("DW_AT_high_pc")) {
			Dwarf_Addr addr = 0;
			sres = dwarf_formaddr(attr_in, &addr, &err);
			if (DW_DLV_OK != sres) {
				MY_PRINT("failed to read address attribute\n");
				goto dealloc_form;
			}
			if (SEQ("DW_AT_low_pc"))
				_vis_start_line = _pcaddr2line[addr];
			if (SEQ("DW_AT_high_pc"))	
				_vis_end_line = _pcaddr2line[addr];
			MY_PRINT("line:%llu \"0x%08llx\" ",
				_pcaddr2line[addr], addr);
		}
		else if (SEQ("DW_AT_type")) {
			Dwarf_Off offset = 0;
			sres = dwarf_formref(attr_in, &offset, &err);
			if (DW_DLV_OK != sres) {
				MY_PRINT("failed to read ref attribute\n");
				goto dealloc_form;
			}
			if (!!var) {
				var->setTypeOffset(offset);
			}
			else if (!!basetype) {
				std::stringstream ss;
				ss << offset;
				basetype->name = ss.str();
				if (0 == strcmp(tag_name, "DW_TAG_pointer_type"))
					_base_type_suffix[_file][parent_offset] = "*";
				else if (0 == strcmp(tag_name, "DW_TAG_const_type"))
					_base_type_suffix[_file][parent_offset] = " const";
				else if (0 == strcmp(tag_name, "DW_TAG_reference_type"))
					_base_type_suffix[_file][parent_offset] = "&";
				else if (0 == strcmp(tag_name, "DW_TAG_volatile_type"))
					_base_type_suffix[_file][parent_offset] = " volatile";
			}

			if (!!(*tcon) && (*tcon)->_valid) {
				(*tcon)->_field_type_offset = offset;
			}	
			MY_PRINT("<0x%08llu> ", offset);
		}
		MY_PRINT("\n");
dealloc_form:
		//dwarf_dealloc(dbg, (char *)form, DW_DLA_STRING);
dealloc_attr:;
		//dwarf_dealloc(dbg, (void *)v, DW_DLA_STRING);
	}

	bool print_one_die(Dwarf_Debug dbg, Dwarf_Die die,
		int die_indent_level, char **srcfiles,
		const char* *const cfile, Dwarf_Signed cnt, const std::vector<std::string>& srclist, TypeContainer ** tcon = 0) {

		Dwarf_Error_s *err;
		Dwarf_Half tag = 0;

		int tres = dwarf_tag(die, &tag, &err);
		if (DW_DLV_OK != tres) {
			MY_PRINT("Failed to obtain the tag\n");
			return false;
		}

		const char * tagname = 0;
		Dwarf_Signed atcnt = 0;
		Dwarf_Attribute *atlist = 0;
		int atres = 0;
		Variable *var = 0;
		basetype_desc *basetype = 0;
		Dwarf_Off offset = 0;	
	
		int res = dwarf_get_TAG_name(tag, &tagname);
		if (DW_DLV_OK != res) {
			MY_PRINT("Failed to get the name of the tag\n");
			goto dealloc_tag_name;
		}
		#define SEQ1(s)	(0 == strcmp(s, tagname))
		if (
			!SEQ1("DW_TAG_compile_unit")
			&& !SEQ1("DW_TAG_base_type")
			&& !SEQ1("DW_TAG_formal_parameter")
			&& !SEQ1("DW_TAG_lexical_block")
			&& !SEQ1("DW_TAG_variable")
			&& !SEQ1("DW_TAG_subprogram")
			&& !SEQ1("DW_TAG_pointer_type")
			&& !SEQ1("DW_TAG_const_type")
			&& !SEQ1("DW_TAG_reference_type")
			&& !SEQ1("DW_TAG_volatile_type")
			&& !SEQ1("DW_TAG_typedef")
			&& !SEQ1("DW_TAG_structure_type")
			&& !SEQ1("DW_TAG_class_type")
			&& !SEQ1("DW_TAG_member")
			&& !SEQ1("DW_TAG_array_type")
			&& !SEQ1("DW_TAG_subrange_type")
			)
			goto dealloc_tag_name;

		if (SEQ1("DW_TAG_subprogram"))
			_vis_end_line = 0;

		MY_PRINT("\n%*s[%d]%s ", 2 * die_indent_level, " ", die_indent_level, tagname);
		res = dwarf_die_CU_offset(die, &offset, &err);
		if (DW_DLV_OK != res) {
			MY_PRINT("Failed to get die CU offset\n");
			goto dealloc_tag_name;
		}

		if (0 == strcmp(tagname, "DW_TAG_variable") ||
			0 == strcmp(tagname, "DW_TAG_formal_parameter")) {
			var = &newVar();
		} else if (0 == strcmp(tagname, "DW_TAG_base_type") ||
			0 == strcmp(tagname, "DW_TAG_pointer_type") ||
			0 == strcmp(tagname, "DW_TAG_const_type") ||
			0 == strcmp(tagname, "DW_TAG_reference_type") ||
			0 == strcmp(tagname, "DW_TAG_volatile_type") ||
			0 == strcmp(tagname, "DW_TAG_typedef") ||
			0 == strcmp(tagname, "DW_TAG_structure_type") ||
			0 == strcmp(tagname, "DW_TAG_class_type") ||
			0 == strcmp(tagname, "DW_TAG_array_type")) {
			basetype = &newBaseType(offset, _file);
			//printf("%s ", tagname);
			//printf("=TYPES: off=%d file=%s\n", offset, _file.c_str());
		}

		if (die_indent_level <= 1 && 
			(0 == strcmp(tagname, "DW_TAG_structure_type") ||
			0 == strcmp(tagname, "DW_TAG_class_type") ||
			0 == strcmp(tagname, "DW_TAG_array_type"))) {
			delete (*tcon);
			*tcon = new TypeContainer;
			(*tcon)->_type_offset = offset;
			(*tcon)->_fields = &_struct_fields[hasher(_file + std::to_string((*tcon)->_type_offset))];
			(*tcon)->_basetype = basetype;
				//printf("=FIELDS: off=%d file=%s\n", (*tcon)->_type_offset, _file.c_str());

		}
		
		if (!!(*tcon)) {
			if (0 == strcmp(tagname, "DW_TAG_member"))
				(*tcon)->_valid = true;
			else
				(*tcon)->_valid = false;
		}
		MY_PRINT("<0x%08llu>\r\n", offset);

		atres = dwarf_attrlist(die, &atlist, &atcnt, &err);
		if (DW_DLV_ERROR == atres)
			MY_PRINT("Error while getting the attributes\n");
		else if (DW_DLV_NO_ENTRY == atres)
			atcnt = 0;

		for (Dwarf_Signed i = 0; i < atcnt; ++i) {
			Dwarf_Half attr;
			int ares = dwarf_whatattr(atlist[i], &attr, &err);
			if (DW_DLV_OK != ares) {
				MY_PRINT("<Cannot get attributes>\n");
				continue;
			}
			MY_PRINT("%*s", 2 * die_indent_level + 1, " ");
			get_attribute(dbg, die, attr, atlist[i],
				die_indent_level, tagname,
				srcfiles, srclist, cfile, cnt, offset, var, basetype, tcon);
		}
		for (Dwarf_Signed i = 0; i < atcnt; ++i)
			dwarf_dealloc(dbg, atlist[i], DW_DLA_ATTR);
		if (DW_DLV_OK == atres)
			dwarf_dealloc(dbg, atlist, DW_DLA_LIST);

		if (!!var) {
			if (size_t(Variable::VALUE_NOT_SET) == var->line() ||
				var->name().empty()) {
				cancelVar();
				return true;
			}
			// Fix the end line of the scope as debugging info
			// often gives incorrect values.
			std::pair<int, int> ranges = _scoping.scope(var->file(),
				var->line());
			var->setVisEndLine(ranges.second);
			MY_PRINT("@VARIABLE: [%lu] \"%s\" %lu-%lu (%s)\n",
				var->type_offset(),
				var->name().c_str(),
				var->line(), var->visEndsLine(),
				var->file().c_str());
		}
		else if (!!basetype) {
			MY_PRINT("@BASETYPE: %llu[%s] -> %s \"%s\", size=%lu, count=%lu (%s)\n", offset,
				tagname, basetype->name.c_str(),
				_base_type_suffix[_file][offset].c_str(), basetype->size, basetype->count, _file.c_str());
		}
		//dwarf_dealloc(dbg, (void *)tagname, DW_DLA_STRING);
		return true;
dealloc_tag_name:
		//dwarf_dealloc(dbg, (void *)tagname, DW_DLA_STRING);
		return false;
	}

	void print_die_and_children(Dwarf_Debug dbg,
		Dwarf_Die in_die_in, Dwarf_Bool is_info, char **srcfiles,
		const char **const cfile, Dwarf_Signed cnt, const std::vector<std::string>& srclist, TypeContainer **tcon = 0) {

		Dwarf_Die in_die = in_die_in;
		Dwarf_Error_s *err;
		Dwarf_Die child = 0;
		Dwarf_Die sibling = 0;

		int cdres = 0;

		for (;;) {
			if (print_one_die(dbg, in_die, _die_stack_indent_level,
				srcfiles, cfile, cnt, srclist, tcon)) {
				
				cdres = dwarf_child(in_die, &child, &err);
	
				if (DW_DLV_OK == cdres) {
					++_die_stack_indent_level;
					print_die_and_children(dbg, child, is_info,
						srcfiles, cfile, cnt, srclist, tcon);
					--_die_stack_indent_level;
					dwarf_dealloc(dbg, child, DW_DLA_DIE);
					child = 0;
				}
				else if (DW_DLV_ERROR == cdres) {
					MY_PRINT("Error while obtaining a child\n");
					return;
				}
			}
		
			cdres = dwarf_siblingof_b(dbg, in_die, is_info, &sibling, &err);
			if (DW_DLV_ERROR == cdres) {
				MY_PRINT("Failed to get a sibling\n");
				return;
			}

			if (in_die != in_die_in) {
				dwarf_dealloc(dbg, in_die, DW_DLA_DIE);
				in_die = 0;
			}
			if (DW_DLV_OK == cdres)
				in_die = sibling;
			else
				break;
		}
	}

	
	void print_line_numbers_info(Dwarf_Debug dbg, Dwarf_Die cu_die) {

		Dwarf_Error_s *err;
		Dwarf_Line *linebuf = NULL;
		Dwarf_Signed linecount = 0;

		int lres = dwarf_srclines(cu_die, &linebuf,
			&linecount, &err);
		switch(lres) {
		case DW_DLV_ERROR:
			MY_PRINT("Error in reading line numbers information\n");
			return;
		case DW_DLV_NO_ENTRY:
			MY_PRINT("No line numbers information\n");
			return;
		default:;
		}

		int ares = 0, sres = 0;
		Dwarf_Addr pc = 0;
		Dwarf_Unsigned lineno = 0;
		char * filename = 0;
		for (Dwarf_Signed i = 0; i < linecount; ++i) {
			Dwarf_Line line = linebuf[i];
			filename = (char *)"<unknown>";
			sres = dwarf_linesrc(line, &filename, &err);
			if (DW_DLV_ERROR == sres) {
				MY_PRINT("cannot read a source file that corresponds " 
					"to the line\n");
			} else {
				ares = dwarf_lineaddr(line, &pc, &err);
			}
			if (DW_DLV_ERROR == ares) {
				MY_PRINT("failed to obtain source - pc association\n");
				continue;
			}
			ares = dwarf_lineno(line, &lineno, &err);
			
			if (DW_DLV_ERROR == ares) {
				MY_PRINT("failed to get a line number for the pc addr\n");
				continue;
			}
			if (DW_DLV_NO_ENTRY == ares)
				continue;
			
			_pcaddr2line[pc] = lineno;

			if (DW_DLV_OK == sres)
				dwarf_dealloc(dbg, filename, DW_DLA_STRING);
		}
		dwarf_srclines_dealloc(dbg, linebuf, linecount);
	} 

	int print_info(Dwarf_Debug &dbg, bool src_lines_nfo) {

		Dwarf_Error_s *err;
		Dwarf_Unsigned cu_header_length = 0;
		Dwarf_Half version_stamp = 0;
		Dwarf_Unsigned abbrev_offset = 0;
		Dwarf_Half address_size = 0;
		Dwarf_Half length_size = 0;
		Dwarf_Half extension_size = 0;
		Dwarf_Sig8 signature;
		Dwarf_Unsigned typeoffset = 0;
		Dwarf_Unsigned next_cu_offset = 0;

		MY_PRINT("[[Section .debug_info]]\n");

		unsigned iteration = 0;
		int nres = 0;
		int sres = DW_DLV_OK;
		Dwarf_Die cu_die = 0;
		TypeContainer *tcon = 0;
		// REF print_die.c : 400	
		for (;;++iteration) {
//			MY_PRINT("*\n");

			nres = dwarf_next_cu_header_c(dbg, 1, &cu_header_length,
				&version_stamp, &abbrev_offset, &address_size,
				&length_size, &extension_size, &signature,
				&typeoffset, &next_cu_offset, &err);

			if (DW_DLV_NO_ENTRY == nres || DW_DLV_OK != nres)
				return nres;

			sres = dwarf_siblingof_b(dbg, NULL, 1, &cu_die, &err);
			if (DW_DLV_OK != sres) {
				MY_PRINT("error in reading siblings");
				return sres;
			}
	
			if (src_lines_nfo) {
				print_line_numbers_info(dbg, cu_die);
			} else {
				Dwarf_Signed cnt = 0;
				char **srcfiles = 0;
				int srcf = dwarf_srcfiles(cu_die, &srcfiles, &cnt,
					&err);
				if (DW_DLV_OK != srcf) {
					srcfiles = 0;
					cnt = 0;
				}
				std::vector<std::string> srclist;
				for (int j = 0; j < cnt; ++j) {
					srclist.push_back(srcfiles[j]);
				}

				const char * filename = 0;
				print_die_and_children(dbg, cu_die, 1, srcfiles,
					&filename, cnt, srclist, &tcon);
				if (DW_DLV_OK == srcf) {
					for (int si = 0; si < cnt; ++si)
						dwarf_dealloc(dbg, srcfiles[si], DW_DLA_STRING);
					dwarf_dealloc(dbg, srcfiles, DW_DLA_LIST);
				}
			}
			dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
			cu_die = 0;
		}
		delete tcon;
	};

	int collect_vars_info(Elf * elf) {
		Dwarf_Debug dbg;
		Dwarf_Error_s *err;
		int dres = dwarf_elf_init(elf, DW_DLC_READ, NULL, NULL, &dbg, &err);
		if (DW_DLV_NO_ENTRY == dres) {
			MY_PRINT("No DWARF information.\n");
			return 0;
		}
		if (DW_DLV_OK != dres) {
			MY_PRINT("error reading DWARF info\n");
			return 0;
		}
	
		print_info(dbg, /*gather_lines_info=*/true);
		print_info(dbg, false);	

		dwarf_finish(dbg, &err);
		return 1;
	};

	int parse_debug_info(int fd) {

		if (elf_version(EV_CURRENT) == EV_NONE) {
			MY_PRINT("libelf.a is out of date\n");
		}

		Elf * elf = elf_begin(fd, ELF_C_READ, NULL);
		if (ELF_K_AR == elf_kind(elf)) {
			MY_PRINT("the file is an archive\n");
			close(fd);
			return 0;
		}
		Elf *f_elf = elf;
		// FIXME: check the there is an ELF32 or ELF64 header
		Elf_Cmd cmd = ELF_C_READ;
		while(0 != (elf = elf_begin(fd, cmd, elf))) {
			collect_vars_info(elf);
			cmd = elf_next(elf);
			elf_end(elf);
		}
		elf_end(f_elf);
		return 1;
	};

	bool read_file_debug(const char * file) {	
		int fd = open(file, O_RDONLY);
		if (-1 == fd) {
		  MY_PRINT("cannot open file %s\n", file);
			return false;
		}

		struct stat elf_stats;
		if ((fstat(fd, &elf_stats))) {
		  MY_PRINT("cannot stat file %s\n", file);
			return false;
		}

		int e = parse_debug_info(fd);
		close(fd);
		return 1 == e;
	}
#endif // __linux
};


bool VarInfo::Imp::init(const std::string& file) {
#ifdef __linux
	_file = file;
	_die_stack_indent_level = 0;
	return read_file_debug(file.c_str());
#else // __linux
	return false; // NOT_IMPLEMENTED
#endif // __linux
};


VarInfo::VarInfo() : _imp(new VarInfo::Imp) {}

const std::string VarInfo::type(const std::string& file, const size_t line, const std::string& name) const {
	return _imp->type(file, line, name);
}

const std::string VarInfo::fieldname(const std::string& file, const size_t line, const std::string& name, const unsigned offset) const {
	return _imp->fieldname(file, line, name, offset);
}

bool VarInfo::init(const std::string& file) {
	_file = file;
	return _imp->init(_file);
}
