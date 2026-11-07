// The "file" node type's spec and behaviour (docs/design/m1-white-to-file.md section 2; docs/design/node-framework.md
// sections 7.1 to 7.3 and 7.6): private to the type, shared by file.cpp and its tests, which run it through
// ear::testing::TypeHarness<spec, File> by the same bank handles. In the type's own named namespace, never an unnamed
// one (type_spec.hpp says why). start is file.cpp's, the only file that sees libsndfile.
#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "ear/core/type_spec.hpp"

namespace ear::nodes::file {

/// The path as given, nothing when empty.
inline std::optional<std::string> nonEmpty(std::string_view value) {
	if (value.empty()) return std::nullopt;
	return std::string(value);
}

inline constexpr auto pathRow = setting::required("path", setting::text<nonEmpty>("a non-empty string"));
inline constexpr auto record = bank::input<"record">;
inline constexpr TypeSpec spec{.settings = SettingsTable<pathRow>{"file"}, .banks = Banks<record>{}};
using Checked = CheckedOf<spec>;
inline constexpr std::uint32_t channelCount = 2;
inline constexpr std::uint32_t ringSeconds = 2;

struct File {
	static Members<spec> ports(const Checked&) { return {record(members("left", "right"))}; }

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

	/// Acquires the recording (opened beneath the recordings folder by the framework) and a handoff, whose own thread
	/// writes the frames with libsndfile.
	static StartResult start(StartContext& context, const Checked& s);
};

} // namespace ear::nodes::file
