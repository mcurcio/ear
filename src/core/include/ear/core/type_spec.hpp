// What a node type writes (docs/design/node-framework.md sections 7.1, 7.2 and 9.2, slice 2): one constexpr TypeSpec
// (its settings table and its banks, each a compile-time constant with its name and direction) and one plain behaviour
// class, made a NodeType by makeNodeType<spec, Behaviour>() from the type's one named function:
//
//   constexpr auto noise = bank::output<"noise">;
//   constexpr TypeSpec spec{.settings = SettingsTable<>{"white"}, .banks = Banks<noise>{}};
//   struct WhiteCode { void process(Io<spec> io, const GraphCycle&) noexcept { for (auto ch : io[noise]) ...; } };
//   struct White {
//   	static Members<spec> ports(const CheckedOf<spec>&) { return {noise(members("mono"))}; }
//   	static StartResult start(StartContext&, const CheckedOf<spec>&) { return WhiteCode{}; }
//   };
//   std::unique_ptr<const NodeType> makeWhiteNodeType() { return makeNodeType<spec, White>(); }
//
// The behaviour may add `static std::expected<void, std::string> check(const CheckedOf<spec>&)`, a rule across
// settings, which parse runs after the table's walk (a refusal is invalid-settings). Its code reads and writes only
// through Io<spec>, which takes only the spec's bank handles: an output bank gives std::span<float> members, an input
// bank std::span<const float> members, so a bank of another type, a misspelt bank or a write into an input does not
// compile. Pure: nothing but the standard library and ear_core.
#pragma once
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "ear/core/node_type.hpp"
#include "ear/core/settings_table.hpp"
namespace ear {
namespace spec_detail {
/// A bank's name as a template argument, so each bank of a spec is its own type.
template <std::size_t Size> struct BankName {
	char text[Size]{};
	consteval BankName(const char (&name)[Size]) { // NOLINT: implicit on purpose, from a string literal
		std::copy_n(name, Size, text);
	}
	constexpr std::string_view view() const noexcept { return {text, Size - 1}; }
};
/// As portName's parts: 1 or more of [A-Za-z0-9_-].
consteval bool validBankName(std::string_view name) {
	return !name.empty() && std::ranges::all_of(name, [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
}
template <std::size_t Count> consteval bool uniqueNames(const std::array<std::string_view, Count>& names) {
	for (std::size_t at = 0; at < Count; ++at)
		for (std::size_t earlier = 0; earlier < at; ++earlier)
			if (names[at] == names[earlier]) return false;
	return true;
}
} // namespace spec_detail

/// One bank's members, for a behaviour's ports(): numbered ("1".."n") or named.
struct MemberList {
	std::vector<std::string> names;
};
/// "1".."<count>".
inline MemberList members(std::uint32_t count) { return {numbered(count)}; }
/// The members by name, in order: members("left", "right").
template <class... Names>
	requires(sizeof...(Names) > 0 && (std::convertible_to<const Names&, std::string_view> && ...))
MemberList members(const Names&... names) {
	return {{std::string(std::string_view(names))...}};
}
/// A bank's members, as its handle gives them: stream(members(n)).
template <class Handle> struct BankMembers {
	MemberList list;
};

/// One bank of a type, as a compile-time constant: its name, direction and reach. Made only by bank::output, input,
/// insideOutput and insideInput; a name that is not 1 or more of [A-Za-z0-9_-] does not compile.
template <spec_detail::BankName Name, Direction Dir, PortBank::Reach Reach> struct BankHandle {
	static_assert(spec_detail::validBankName(Name.view()), "a bank's name is 1 or more of [A-Za-z0-9_-]");
	static constexpr std::string_view name = Name.view();
	static constexpr Direction direction = Dir;
	static constexpr PortBank::Reach reach = Reach;
	/// What a member's samples are in Io: written for an output, read only for an input.
	using Sample = std::conditional_t<Dir == Direction::output, float, const float>;
	/// This bank's members, for ports().
	BankMembers<BankHandle> operator()(MemberList list) const { return {std::move(list)}; }
};
template <class T> inline constexpr bool isBankHandle = false;
template <spec_detail::BankName Name, Direction Dir, PortBank::Reach Reach>
inline constexpr bool isBankHandle<BankHandle<Name, Dir, Reach>> = true;

namespace bank {
/// A routable output bank: routes name its ports ("<bank>.<member>").
template <spec_detail::BankName Name>
inline constexpr BankHandle<Name, Direction::output, PortBank::Reach::routable> output{};
/// A routable input bank.
template <spec_detail::BankName Name>
inline constexpr BankHandle<Name, Direction::input, PortBank::Reach::routable> input{};
/// An inside output bank: only the node's own outside nodes and code reach it (a sound card's playback).
template <spec_detail::BankName Name>
inline constexpr BankHandle<Name, Direction::output, PortBank::Reach::inside> insideOutput{};
/// An inside input bank (a sound card's capture, an RTP receiver).
template <spec_detail::BankName Name>
inline constexpr BankHandle<Name, Direction::input, PortBank::Reach::inside> insideInput{};
} // namespace bank

/// A spec's banks, in declaration order (which is the order of their ports in the spans the code gets, by direction
/// and reach). Their names are unique.
template <auto... Handles>
	requires(isBankHandle<std::remove_cvref_t<decltype(Handles)>> && ...)
struct Banks {
	static constexpr std::size_t count = sizeof...(Handles);
	static_assert(spec_detail::uniqueNames(std::array<std::string_view, count>{decltype(Handles)::name...}),
	              "a type's banks have unique names");
	/// Whether `Handle` is one of these banks.
	template <class Handle> static constexpr bool has =
		(std::same_as<Handle, std::remove_cvref_t<decltype(Handles)>> || ...);
	/// Where `Handle` sits among these banks; only for a bank `has` holds for.
	template <class Handle> static constexpr std::size_t indexOf = [] {
		constexpr std::array<bool, count> same{std::same_as<Handle, std::remove_cvref_t<decltype(Handles)>>...};
		return static_cast<std::size_t>(std::ranges::find(same, true) - same.begin());
	}();
	struct Declared {
		std::string_view name;
		Direction direction;
		PortBank::Reach reach;
	};
	static constexpr std::array<Declared, count> declared{
		Declared{decltype(Handles)::name, decltype(Handles)::direction, decltype(Handles)::reach}...};
};
template <class T> inline constexpr bool isBanks = false;
template <auto... Handles> inline constexpr bool isBanks<Banks<Handles...>> = true;

/// A type's one declaration: its settings table and its banks. The later tables (status, events, log codes, actions,
/// stores, latency: docs/design/node-framework.md sections 7.1 and 8) join as members with empty defaults, so a spec
/// that gives none stays these two.
///
/// `Unique` is never given: its default, a lambda's type, is new at every spec, and local to its file. It is what keeps
/// two types apart. Every template a type instantiates with its spec (Io<spec>, Members<spec>, its config and code) is
/// named by the spec's qualified name, and GCC 14 gives such an instantiation external linkage even when the spec is
/// in an unnamed namespace, so two files' `(anonymous namespace)::spec` would be one symbol and the linker would keep
/// one type's code for both. A spec whose type is local to its file makes them local too.
template <class Settings, class BankList, class Unique = decltype([] {})>
	requires(std::derived_from<Settings, AnySettingsTable> && isBanks<BankList>)
struct TypeSpec {
	Settings settings;
	BankList banks;
};

template <const auto& Spec> using SettingsOf = std::remove_cvref_t<decltype(Spec.settings)>;
/// The settings a spec's table passed: what ports(), check() and start() read, by row (`s.get<row>()`).
template <const auto& Spec> using CheckedOf = typename SettingsOf<Spec>::Checked;
template <const auto& Spec> using BanksOf = std::remove_cvref_t<decltype(Spec.banks)>;

/// Every bank of a spec with its members: what a behaviour's ports() returns, {stream(members(n)), from(members(n))}.
/// Each bank of the spec once, in any order; a bank of another spec, or one left out, does not compile.
template <const auto& Spec> class Members {
	using List = BanksOf<Spec>;

public:
	template <class... Handles> Members(BankMembers<Handles>... given) { // NOLINT: implicit on purpose
		static_assert((List::template has<Handles> && ...), "ports() names only its spec's banks");
		static_assert(sizeof...(Handles) == List::count &&
		                  spec_detail::uniqueNames(std::array<std::string_view, sizeof...(Handles)>{Handles::name...}),
		              "ports() gives every bank of its spec, each once");
		(place<Handles>(std::move(given.list)), ...);
	}
	/// The node's ports, the spec's banks in its order. A member that is not 1 or more of [A-Za-z0-9_-], or one given
	/// twice in a bank, throws std::logic_error (a type bug).
	Ports ports() const {
		std::vector<PortBank> banks;
		for (std::size_t at = 0; at < List::count; ++at) {
			const auto& bank = List::declared[at];
			const auto& names = members_[at].names;
			for (std::size_t one = 0; one < names.size(); ++one)
				if (std::ranges::find(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(one), names[one]) !=
				    names.begin() + static_cast<std::ptrdiff_t>(one))
					throw std::logic_error("bank \"" + std::string(bank.name) + "\" names member \"" + names[one] +
					                       "\" twice");
			banks.push_back({bank.direction, bank.name, names, bank.reach});
		}
		return Ports(banks);
	}

private:
	template <class Handle> void place(MemberList list) { members_[List::template indexOf<Handle>] = std::move(list); }
	std::array<MemberList, List::count> members_;
};

/// One cycle's audio, by bank (docs/design/node-framework.md section 9.2): io[bank] is the bank's members, each a span
/// of exactly frames() samples, an output's to write (every sample), an input's to read (an unlinked input reads zeros).
/// Only the spec's own banks are taken. The layout is resolved once, before the first cycle, so io[bank] is an index.
template <const auto& Spec> class Io {
	using List = BanksOf<Spec>;

public:
	/// Where each bank's ports sit in the spans the framework gives: resolved by resolve(), in prepare.
	struct Layout {
		std::array<std::size_t, List::count> first{};
		std::array<std::size_t, List::count> size{};
	};
	/// The layout of `ports` for this spec. Throws std::logic_error (a type bug) when `ports` are not the spec's banks
	/// in name, direction and reach.
	static Layout resolve(const Ports& ports) {
		if (ports.banks().size() != List::count)
			throw std::logic_error("the ports have " + std::to_string(ports.banks().size()) + " banks, the spec " +
			                       std::to_string(List::count));
		Layout layout;
		for (std::size_t at = 0; at < List::count; ++at) {
			const auto& declared = List::declared[at];
			const Ports::Bank& bank = ports.bank(declared.name);
			if (bank.direction != declared.direction || bank.reach != declared.reach)
				throw std::logic_error("bank \"" + std::string(declared.name) +
				                       "\" differs from the spec's in direction or reach");
			layout.first[at] = bank.first;
			layout.size[at] = bank.ports.size();
		}
		return layout;
	}
	/// `inputs` are inputs then inside inputs, `outputs` outputs then inside outputs, as AudioCode::process gets them.
	Io(const Layout& layout, std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	   std::uint32_t frames) noexcept
		: layout_(&layout), inputs_(inputs), outputs_(outputs), frames_(frames) {}
	/// The bank's members, in order.
	template <class Handle> std::span<const std::span<typename Handle::Sample>> operator[](Handle) const noexcept {
		static_assert(isBankHandle<Handle>, "Io takes a bank handle");
		static_assert(List::template has<Handle>, "Io takes only its spec's banks");
		constexpr std::size_t at = List::template indexOf<Handle>;
		if constexpr (Handle::direction == Direction::output)
			return outputs_.subspan(layout_->first[at], layout_->size[at]);
		else return inputs_.subspan(layout_->first[at], layout_->size[at]);
	}
	std::uint32_t frames() const noexcept { return frames_; }

private:
	const Layout* layout_;
	std::span<const std::span<const float>> inputs_;
	std::span<const std::span<float>> outputs_;
	std::uint32_t frames_;
};

/// One pass-through pair by handle: an input bank's ports copied to an output bank's.
template <class From, class To> BankPair bankPair(From, To) {
	static_assert(isBankHandle<From> && isBankHandle<To>, "bankPair takes bank handles");
	static_assert(From::direction == Direction::input && To::direction == Direction::output,
	              "a pass-through copies an input bank to an output bank");
	return {std::string(From::name), std::string(To::name)};
}

namespace spec_detail {
/// The spec a code is written against, read from its process: void process(Io<spec>, const GraphCycle&) noexcept.
template <class Process> struct ProcessOf {
	static_assert(sizeof(Process) == 0, "a code's process is void process(Io<spec>, const GraphCycle&) noexcept");
};
template <class Code, const auto& S> struct ProcessOf<void (Code::*)(Io<S>, const GraphCycle&) noexcept> {
	static constexpr const auto& spec = S;
};
/// A code whose layout the type's config hands over: SpecConfig binds it to the node's ports after start.
class BankBoundCode : public AudioCode {
public:
	virtual void bindPorts(const Ports& ports) noexcept = 0;
};
template <const auto& Spec, class Code> class ErasedSpecCode final : public BankBoundCode {
public:
	explicit ErasedSpecCode(Code code) : code_(std::move(code)) {}
	void bindPorts(const Ports& ports) noexcept override { ports_ = &ports; }
	void prepare(const AudioFormat& format) override {
		if (ports_ == nullptr)
			throw std::logic_error("a code written against a spec runs only in a type made by makeNodeType");
		layout_ = Io<Spec>::resolve(*ports_);
		if constexpr (requires { code_.prepare(format); }) code_.prepare(format);
	}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		code_.process(Io<Spec>(layout_, inputs, outputs, frames), cycle);
	}
	void release() noexcept override {
		if constexpr (requires { code_.release(); }) code_.release();
	}

private:
	Code code_;
	const Ports* ports_ = nullptr; // the config's, which outlives the code (I5 step 5)
	typename Io<Spec>::Layout layout_;
};

template <const auto& Spec, class Behaviour> class SpecConfig final : public NodeConfig {
public:
	explicit SpecConfig(const CheckedOf<Spec>& settings)
		: settings_(settings), ports_(Behaviour::ports(settings_).ports()) {}
	const Ports& ports() const override { return ports_; }
	StartResult start(StartContext& context) const override {
		StartResult result = Behaviour::start(context, settings_);
		if (!result.failed())
			if (auto* bound = dynamic_cast<BankBoundCode*>(result.code().own().get())) bound->bindPorts(ports_);
		return result;
	}

private:
	CheckedOf<Spec> settings_;
	Ports ports_;
};

/// The type makeNodeType makes. It derives from NodeType itself, handing it the spec's table at run time, not from
/// NodeTypeOf<Spec.settings>: GCC 14 gives a template argument that names a subobject (or a variable template's
/// specialization) external linkage whatever its object's, so two types' `spec.settings`, each in its own file's
/// unnamed namespace, became one symbol and the linker kept one type's table for both.
template <const auto& Spec, class Behaviour> class SpecNodeType final : public NodeType {
#ifdef EAR_NODE_TYPE_NAME // ear_node_type() (cmake/NodeType.cmake) gives a type's library its folder's name
	static_assert(Spec.settings.name() == EAR_NODE_TYPE_NAME, "a type's table is named for its folder");
#endif
	using Checked = CheckedOf<Spec>;
	static_assert(
		requires(const Checked& settings) {
			{ Behaviour::ports(settings) } -> std::same_as<Members<Spec>>;
		}, "a behaviour has static Members<spec> ports(const CheckedOf<spec>&)");
	static_assert(
		requires(StartContext& context, const Checked& settings) {
			{ Behaviour::start(context, settings) } -> std::same_as<StartResult>;
		}, "a behaviour has static StartResult start(StartContext&, const CheckedOf<spec>&)");

public:
	SpecNodeType() noexcept : NodeType(Spec.settings) {}

private:
	/// The table's walk passed: the behaviour's check, a rule across settings, then the config.
	MadeConfig makeConfig(const NodeName&, CheckedSettings walked) const override {
		const Checked settings(std::move(walked));
		if constexpr (requires { Behaviour::check(settings); }) {
			static_assert(
				std::same_as<decltype(Behaviour::check(settings)), std::expected<void, std::string>>,
				"a behaviour's check is static std::expected<void, std::string> check(const CheckedOf<spec>&)");
			if (auto checked = Behaviour::check(settings); !checked) return std::unexpected(std::move(checked.error()));
		}
		return std::make_unique<const SpecConfig<Spec, Behaviour>>(settings);
	}
};
} // namespace spec_detail

template <SpecCode Code> std::unique_ptr<AudioCode> eraseSpecCode(Code code) {
	using Process = spec_detail::ProcessOf<decltype(&Code::process)>;
	return std::make_unique<spec_detail::ErasedSpecCode<Process::spec, Code>>(std::move(code));
}

/// The type `Behaviour` gives `Spec`, called from the type's one named function. Its name, setting kinds and checks are
/// the spec's table's, and (in a type's own library) the table must be named for the type's folder.
template <const auto& Spec, class Behaviour> std::unique_ptr<const NodeType> makeNodeType() {
	return std::make_unique<const spec_detail::SpecNodeType<Spec, Behaviour>>();
}
} // namespace ear
