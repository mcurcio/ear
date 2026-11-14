# cmake -DROOT=<folder> [-DGLOB=<pattern>] [-DEXEMPT=<paths>] -P property_guard.cmake
#
# The property guard of docs/engine.md#properties-ear-sets. A key ear sets is a PropertyKey, which only the property
# table makes (src/pipewire/engine_properties.hpp), and the only code that hands keys to PipeWire's property writers is
# src/pipewire/engine_property_set.{hpp,cpp}, whose functions take only PropertyKeys. This guard keeps it the only code:
# every file under ROOT matching GLOB (default: C and C++ sources and headers), except the paths EXEMPT names relative
# to ROOT (default: those two files, exactly) and tests (anything under a folder named tests, which drive PipeWire
# themselves), is read a line at a time with its character and string literals emptied first and then its comments
# removed (so a "//" inside a string hides nothing), and a line that names any of these, as a whole word (a call, a
# function pointer, anything), is printed with its place and fails the script:
#   any pw_properties_new*, pw_properties_set*, pw_properties_update*, pw_properties_add* (the _checked, _string,
#     _dict, f and va variants included); any pw_<object>_update_properties; pw_core_create_object;
#     pw_context_load_module and any pw_impl_module_load*; any SPA_DICT_ITEM* or SPA_DICT_INIT*;
#   spa_dict_item anywhere but as "const spa_dict_item*" (a pointer to read through);
#   a spa_dict made here: a variable with an initializer, a temporary or a compound literal.
# It fails, too, when ROOT and GLOB leave it no file to check. Raw string literals are not emptied (src/ has none).
cmake_policy(VERSION 3.28) # while() and if() under today's rules
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "property_guard: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()
if(NOT DEFINED EXEMPT)
	set(EXEMPT "pipewire/engine_property_set.hpp;pipewire/engine_property_set.cpp")
endif()

set(before "(^|[^A-Za-z0-9_])")
set(rules
	"${before}pw_properties_(new|set|update|add)"
	"${before}pw_[a-z_]+_update_properties([^A-Za-z0-9_]|$)"
	"${before}(pw_core_create_object|pw_context_load_module)([^A-Za-z0-9_]|$)"
	"${before}pw_impl_module_load"
	"${before}SPA_DICT_(ITEM|INIT)"
	"${before}spa_dict_item([^A-Za-z0-9_]|$)"
	"${before}spa_dict[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*(\\{|=|\\()"
	"${before}spa_dict[ \t]*\\{"
	"\\([ \t]*(struct[ \t]+)?spa_dict[ \t]*\\)[ \t]*\\{")
# The one way to name spa_dict_item that builds nothing: a pointer to read through. Cut before the rules are applied.
set(allowedItem "const[ \t]+(struct[ \t]+)?spa_dict_item[ \t]*\\*")

set(patterns)
foreach(pattern IN LISTS GLOB)
	list(APPEND patterns "${ROOT}/${pattern}")
endforeach()
file(GLOB_RECURSE found LIST_DIRECTORIES false ${patterns})
set(files)
foreach(file IN LISTS found)
	file(RELATIVE_PATH shown "${ROOT}" "${file}")
	list(FIND EXEMPT "${shown}" exempt)
	if(NOT exempt EQUAL -1 OR shown MATCHES "(^|/)tests/")
		continue()
	endif()
	list(APPEND files "${file}")
endforeach()
list(SORT files)

set(offences 0)
foreach(file IN LISTS files)
	file(STRINGS "${file}" lines)
	file(RELATIVE_PATH shown "${ROOT}" "${file}")
	set(number 0)
	set(inBlock FALSE)
	foreach(line IN LISTS lines)
		math(EXPR number "${number} + 1")
		# Literals first, so neither a quote nor a comment marker inside one counts; then comments.
		string(REGEX REPLACE [=['([^'\]|\\.)*']=] "''" code "${line}")
		string(REGEX REPLACE [=["([^"\]|\\.)*"]=] [=[""]=] code "${code}")
		if(inBlock)
			if(code MATCHES [=[\*/]=])
				string(REGEX REPLACE [=[^.*\*/]=] "" code "${code}")
				set(inBlock FALSE)
			else()
				continue()
			endif()
		endif()
		string(REGEX REPLACE [=[/\*.*\*/]=] "" code "${code}")
		string(REGEX REPLACE "//.*$" "" code "${code}")
		if(code MATCHES [=[/\*]=])
			string(REGEX REPLACE [=[/\*.*$]=] "" code "${code}")
			set(inBlock TRUE)
		endif()
		string(REGEX REPLACE "${allowedItem}" "" code "${code}")
		foreach(rule IN LISTS rules)
			if(code MATCHES "${rule}")
				message(STATUS "${shown}:${number}: writes a PipeWire property outside engine_property_set: ${line}")
				math(EXPR offences "${offences} + 1")
				break()
			endif()
		endforeach()
	endforeach()
endforeach()

list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "property_guard: the guard would check nothing: no file under ${ROOT} matches ${GLOB} "
		"outside engine_property_set and tests")
endif()
if(offences GREATER 0)
	message(FATAL_ERROR "property_guard: ${offences} lines under ${ROOT} write PipeWire properties themselves; write "
		"them through src/pipewire/engine_property_set.hpp with keys from the property table "
		"(docs/engine.md#properties-ear-sets)")
endif()
message(STATUS "property_guard: ${count} files under ${ROOT}, none writes a property outside engine_property_set")
