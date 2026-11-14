# The callback boundary's compile-time refusal (docs/architecture.md#the-callback-boundary): a header forced into every
# source of a target that sees PipeWire's headers poisons each raw way to hand PipeWire a callback, after the boundary.

# ear_poison_raw_callbacks(<target> <header>): every source of <target> is compiled with <header> forced in first.
function(ear_poison_raw_callbacks target header)
	target_compile_options(${target} PRIVATE "SHELL:-include ${header}")
endfunction()

# ear_refuse_raw_pipewire(<pipewire> <header> [EXEMPT <target>...]): stops configure, once the whole project has been
# read, at any target that sees the headers of <pipewire> (an imported target) and is not compiled with <header>
# forced in by ear_poison_raw_callbacks, unless EXEMPT names it. A target sees them when it links <pipewire>, directly,
# through an interface library or an alias, or from inside a generator expression (read for the targets it names,
# whatever its condition would come to), or names its include directories itself; a static library's private link
# ($<LINK_ONLY:...> to whoever links the library) gives no headers, and is not seeing them. Interface libraries
# compile nothing and are not checked themselves.
function(ear_refuse_raw_pipewire pipewire header)
	cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "EXEMPT")
	# Deferred calls expand their arguments when they run, in that directory, so the values are written into the call now.
	cmake_language(EVAL CODE
		"cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL ear_refuse_raw_pipewire_now [[${pipewire}]] [[${header}]] ${arg_EXEMPT})")
endfunction()

function(ear_refuse_raw_pipewire_now pipewire header)
	set(exempt ${ARGN})
	set(option "SHELL:-include ${header}")
	get_target_property(pipewire_includes ${pipewire} INTERFACE_INCLUDE_DIRECTORIES)

	set(directories "")
	set(waiting "${CMAKE_SOURCE_DIR}")
	while(waiting)
		list(POP_FRONT waiting directory)
		list(APPEND directories "${directory}")
		get_property(below DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
		list(APPEND waiting ${below})
	endwhile()

	foreach(directory IN LISTS directories)
		get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
		foreach(target IN LISTS targets)
			get_target_property(type ${target} TYPE)
			if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY" OR target IN_LIST exempt)
				continue()
			endif()
			set(through "")
			get_target_property(includes ${target} INCLUDE_DIRECTORIES)
			foreach(include IN LISTS pipewire_includes)
				if(include AND include IN_LIST includes)
					set(through "its include directories")
				endif()
			endforeach()
			if(NOT through)
				_ear_reaches(${target} ${pipewire} through)
			endif()
			if(NOT through)
				continue()
			endif()
			get_target_property(options ${target} COMPILE_OPTIONS)
			if(NOT option IN_LIST options)
				message(FATAL_ERROR "${target} sees PipeWire's headers (through ${through}) without ${header} forced "
					"in: give it ear_poison_raw_callbacks, so a raw callback does not compile "
					"(docs/architecture.md#the-callback-boundary)")
			endif()
		endforeach()
	endforeach()
endfunction()

# Sets <out> to the target through which <start> links <pipewire> for its headers, or empty.
function(_ear_reaches start pipewire out)
	set(seen ${start})
	set(waiting ${start})
	set(property LINK_LIBRARIES) # the start's own links; after it, what each linked target passes on
	while(waiting)
		list(POP_FRONT waiting current)
		get_target_property(linked ${current} ${property})
		foreach(entry IN LISTS linked)
			if(entry MATCHES "^\\$<LINK_ONLY:") # a static library's private link: no headers
				continue()
			endif()
			# A link made from another directory carries that directory's id: name::@(id).
			string(REGEX REPLACE "::@.*$" "" entry "${entry}")
			set(named "${entry}")
			if(entry MATCHES "\\$<")
				string(REGEX MATCHALL "[A-Za-z0-9_.+-]+(::[A-Za-z0-9_.+-]+)*" named "${entry}")
			endif()
			foreach(name IN LISTS named)
				if(NOT TARGET "${name}") # linked-NOTFOUND, a flag, a generator expression's own words
					continue()
				endif()
				get_target_property(aliased "${name}" ALIASED_TARGET)
				if(aliased)
					set(name "${aliased}")
				endif()
				if(name STREQUAL pipewire)
					set(${out} "${current}" PARENT_SCOPE)
					return()
				endif()
				if(name IN_LIST seen)
					continue()
				endif()
				list(APPEND seen "${name}")
				list(APPEND waiting "${name}")
			endforeach()
		endforeach()
		set(property INTERFACE_LINK_LIBRARIES)
	endwhile()
	set(${out} "" PARENT_SCOPE)
endfunction()
