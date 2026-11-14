// The "file" node type's spec and class (docs/node-types.md#file): private to the type, shared by file.cpp and its
// tests, which run it through ear::testing::TypeHarness by the same bank handles. In the type's own named namespace,
// never an unnamed one (type_spec.hpp says why). Its configure asks for the recording (the framework claims its path
// and opens it) and a sink on its inside output bank `writer`, whose consumer (file.cpp's WavWriter, the only code that sees libsndfile) opens and writes the
// WAV on the sink's own thread; its code is the framework's pass-through from `record` to `writer`.
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "ear/core/lifecycle.hpp"

namespace ear::nodes::file {

/// The path as given, nothing when empty.
inline std::optional<std::string> nonEmpty(std::string_view value) {
	if (value.empty()) return std::nullopt;
	return std::string(value);
}

inline constexpr auto pathRow = setting::required("path", setting::text<nonEmpty>("a non-empty string"));
inline constexpr auto record = bank::input<"record">;
inline constexpr auto writer = bank::insideOutput<"writer">; // the sink's bank: no route names it, never faded
inline constexpr TypeSpec spec{.settings = SettingsTable<pathRow>{"file"}, .banks = Banks<record, writer>{}};
using Checked = CheckedOf<spec>;
inline constexpr std::chrono::seconds ringLength{2}; // the sink's ring

class File {
public:
	explicit File(NodeOf<spec>& node) : node_(node) {}

	static Members<spec> ports(const Checked&) {
		return {record(members("left", "right")), writer(members("left", "right"))};
	}

	/// A path relative to the recordings folder that names no "..". The recording's open enforces the same at every
	/// lookup step (symlinks included); this check makes the mistake an invalid-settings error, before any node starts.
	static std::expected<void, std::string> check(const Checked& s) {
		const std::filesystem::path relative(s.get<pathRow>());
		bool inside = !relative.is_absolute();
		for (const auto& component : relative) inside = inside && component != "..";
		if (!inside)
			return std::unexpected(std::string(R"(path: must be relative to the recordings folder, without "..")"));
		return {};
	}

	/// Asks for the recording (claimed and opened beneath the recordings folder by the framework), the sink with a 2 s
	/// ring and its WavWriter, and uses the pass-through from record to writer. file.cpp, the only file that sees
	/// libsndfile.
	Configured configure(CreateOf<spec>& cx, const Checked& s);

private:
	NodeOf<spec>& node_;
};

} // namespace ear::nodes::file
