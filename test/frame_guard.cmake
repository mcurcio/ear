# cmake -DROOT=<folder> [-DGLOB=<pattern>] [-DEXEMPT=<paths>] -P frame_guard.cmake
#
# The frame guard of docs/engine.md#dating: FrameClock (src/core/include/ear/core/latency.hpp) is the one code that
# converts between frames and time at the graph's rate, so nothing else multiplies, divides or takes a remainder by a
# rate. Every file under ROOT matching GLOB (default: C and C++ sources and headers), except the paths EXEMPT names
# relative to ROOT and tests (anything under a folder named tests), is read a line at a time with its character and
# string literals emptied and then its comments removed, and a line where a rate is an operand of a binary *, / or %
# (or of *=, /=, %=) is printed with its place and fails the script. EXEMPT's default names, exactly:
#   core/include/ear/core/latency.hpp, core/latency.cpp   the model and FrameClock themselves;
#   pipewire/latency_spa.hpp   a peer's latency in PipeWire's units ({quantum, rate, ns}) converted to frames at the
#                              port, rounded to the nearest frame (docs/engine.md#latency-per-bank-pair);
#   core/levels.cpp            the level window, a stated display rate rounded up (docs/engine.md#levels).
# A rate is an identifier spelled as one: rate, <anything>_rate, <anything>Rate, frequency, <anything>_frequency or
# <anything>Frequency, each with an optional trailing '_' or '()', reached through '.', '->' or '::' and inside a cast
# or braces (static_cast<double>(rate), std::uint64_t{format.rate}). A line that ends in an operator, or in a rate,
# carries it to the next, so an expression split there is still seen. Digit separators (48'000) are removed before
# character literals are emptied, so one never opens a literal that hides the rest of the line; each block comment is
# removed alone, so code between two on one line is still read.
# The limit: the guard sees a rate only by its name. A rate held under another name (hz, sr), a literal rate (48000),
# a rate inside a call's arguments (std::max(rate, 1U) * x), a rate shifted (rate >> 1) and nanoseconds-per-second
# alone are not seen: ns-to-timespec and ms conversions use 1'000'000'000 and 1'000'000 with no rate, and they are not
# frames. It fails closed where it errs the other way: a pointer declared to a type named like a rate (SampleRate* p)
# is refused, and so is a line opening with a dereference after a line that ends in a rate (`if (n == rate)` then
# `*p = 1;` reads as rate) * p); brace or rejoin it. It fails, too, when ROOT and GLOB leave it no file to check.
# Raw string literals are not emptied (src/ has none).
cmake_policy(VERSION 3.28)
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "frame_guard: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()
if(NOT DEFINED EXEMPT)
	set(EXEMPT "core/include/ear/core/latency.hpp;core/latency.cpp;pipewire/latency_spa.hpp;core/levels.cpp")
endif()

set(id "[A-Za-z_][A-Za-z0-9_]*")
set(rate "([A-Za-z0-9_]*[a-z0-9_]Rate|([A-Za-z0-9_]*_)?rate|[A-Za-z0-9_]*[a-z0-9_]Frequency|([A-Za-z0-9_]*_)?frequency)_?(\\(\\))?")
set(chain "(${id}(\\(\\))?(\\.|->|::))*")
set(openers "([ \t(]|${id}<[^>]*>[ \t]*\\(|[A-Za-z0-9_:]+\\{)*")
# Each rule in a variable of its own: a list would not split between them, as '[' and ']' group a list's elements.
# A rate after a binary operator: an operand ends just before it, so a dereference (*p.rate) is not one.
set(rateAfter "[]A-Za-z0-9_)}'.][ \t]*[*/%]=?[ \t]*${openers}${chain}${rate}([^A-Za-z0-9_(]|$)")
# A rate before one.
set(rateBefore "(^|[^A-Za-z0-9_])${rate}[] \t)}]*[*/%]")
# What a line carries to the next: an operator with an operand before it, or a rate at its end.
set(trailingOperator "[]A-Za-z0-9_)}'.][ \t]*[*/%]=?[ \t]*$")
set(trailingRate "(^|[^A-Za-z0-9_])${rate}[] \t)}]*$")

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
	set(carried "")
	foreach(line IN LISTS lines)
		math(EXPR number "${number} + 1")
		# Literals first, so neither a quote nor a comment marker inside one counts; then comments.
		# A digit separator first (twice: a match takes the digit after it, so 1'2'3 needs a second pass), so 48'000
		# opens no character literal.
		string(REGEX REPLACE "([0-9])'([0-9A-Fa-f])" "\\1\\2" code "${line}")
		string(REGEX REPLACE "([0-9])'([0-9A-Fa-f])" "\\1\\2" code "${code}")
		string(REGEX REPLACE [=['([^'\]|\\.)*']=] "''" code "${code}")
		string(REGEX REPLACE [=["([^"\]|\\.)*"]=] [=[""]=] code "${code}")
		if(inBlock)
			if(code MATCHES [=[\*/]=])
				string(REGEX REPLACE [=[^([^*]|\*+[^*/])*\*+/]=] "" code "${code}")
				set(inBlock FALSE)
			else()
				continue()
			endif()
		endif()
		string(REGEX REPLACE [=[/\*([^*]|\*+[^*/])*\*+/]=] "" code "${code}")
		string(REGEX REPLACE "//.*$" "" code "${code}")
		if(code MATCHES [=[/\*]=])
			string(REGEX REPLACE [=[/\*.*$]=] "" code "${code}")
			set(inBlock TRUE)
		endif()
		string(STRIP "${code}" stripped)
		if(stripped STREQUAL "")
			continue() # a comment or a blank line between the two halves carries nothing away
		endif()
		set(joined "${carried} ${code}")
		set(carried "")
		if(code MATCHES "${trailingOperator}")
			set(carried "x *")
		elseif(code MATCHES "${trailingRate}")
			set(carried "rate")
		endif()
		foreach(rule rateAfter rateBefore)
			if(joined MATCHES "${${rule}}")
				message(STATUS "${shown}:${number}: frame-rate arithmetic outside FrameClock: ${line}")
				math(EXPR offences "${offences} + 1")
				break()
			endif()
		endforeach()
	endforeach()
endforeach()

list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "frame_guard: the guard would check nothing: no file under ${ROOT} matches ${GLOB} "
		"outside FrameClock, the named exceptions and tests")
endif()
if(offences GREATER 0)
	message(FATAL_ERROR "frame_guard: ${offences} lines under ${ROOT} do arithmetic with a rate themselves; convert "
		"frames and time through FrameClock (src/core/include/ear/core/latency.hpp, docs/engine.md#dating)")
endif()
message(STATUS "frame_guard: ${count} files under ${ROOT}, none does arithmetic with a rate outside FrameClock")
