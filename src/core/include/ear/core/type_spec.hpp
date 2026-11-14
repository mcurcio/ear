// What a node type writes, the spec half (docs/writing-a-node-type.md#the-spec): one
// constexpr TypeSpec (its settings table and its banks, each a compile-time constant with its name and direction), the
// bank handles, Members for a class's ports(), Io for its code and bankPair for the framework's pass-through. The class
// half (configure, its contexts, makeNodeType) is lifecycle.hpp's (docs/writing-a-node-type.md#the-class),
// which includes this header; a type's private header, src/nodes/<type>/<type>_spec.hpp, includes that one:
//
//   namespace ear::nodes::white {
//   inline constexpr auto noise = bank::output<"noise">;
//   inline constexpr TypeSpec spec{.settings = SettingsTable<>{"white"}, .banks = Banks<noise>{}};
//   struct WhiteCode { void process(Io<spec> io, const GraphCycle&) noexcept { for (auto ch : io[noise]) ...; } };
//   struct White {
//   	static Members<spec> ports(const CheckedOf<spec>&) { return {noise(members("mono"))}; }
//   	Configured configure(CreateOf<spec>& cx, const CheckedOf<spec>&) {
//   		cx.use(WhiteCode{});
//   		return Configured::applied();
//   	}
//   };
//   }
//   std::unique_ptr<const NodeType> makeWhiteNodeType() { return makeNodeType<white::spec, white::White>(); }
//
// Its code reads and writes only through Io<spec>, which takes only the spec's bank handles: an output bank gives
// std::span<float> members, an input bank std::span<const float> members, so a bank the spec lacks, a misspelt bank or
// a write into an input does not compile. A bank handle is its name, direction and reach, nothing more: another type's
// bank with the same three is the same handle, and Io resolves it by name against this node's ports, so it is sound. A
// code written against another type's spec compiles but is refused at `use` (the node fails). A prepare or release is
// found by its name and must then have exactly that signature. Every spec, row and bank list is declared in a named
// namespace (see TypeSpec).
//
// Pure: nothing but the standard library and ear_core.
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
#include "ear/core/loop.hpp"
#include "ear/core/node_type.hpp"
#include "ear/core/settings_table.hpp"
#include "ear/core/store.hpp"
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

/// One bank's members, for a type's ports(): numbered ("1".."n") or named.
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
/// An inside output bank: only the node's own companions and code reach it (a sound card's playback).
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

/// A type's one declaration: its settings table, its banks, its stores (docs/writing-a-node-type.md#stores) and its
/// loops (docs/writing-a-node-type.md#loops), the last two none by default. The later tables (status, events, log
/// codes, actions: docs/writing-a-node-type.md#the-spec) join as members with empty defaults, so a spec gives only what
/// it uses, in this order (designated initialisers follow it): {.settings, .banks}, {.settings, .banks, .stores},
/// {.settings, .banks, .loops} and {.settings, .banks, .stores, .loops} each deduce the rest.
///
/// A spec and its rows are declared in a named namespace, the type's own (ear::nodes::<type>, in its private header
/// src/nodes/<type>/<type>_spec.hpp, which the type's tests include too): every template a type instantiates with its
/// spec or rows (Io<spec>, Members<spec>, its config, its table) is named by their qualified names, and GCC 14 gives such
/// an instantiation external linkage even in an unnamed namespace, so two files' `(anonymous namespace)::spec` would be
/// one symbol and the linker would keep one type's code for both. Io, Members, makeNodeType and SettingsTable refuse
/// one in an unnamed namespace (linkage::named, settings_table.hpp).
template <class Settings, class BankList, class StoreList = Stores<>, class LoopList = Loops<>>
	requires(std::derived_from<Settings, AnySettingsTable> && isBanks<BankList> && isStores<StoreList> &&
             isLoops<LoopList>)
struct TypeSpec {
	Settings settings;
	BankList banks;
	StoreList stores{};
	LoopList loops{};
};

template <const auto& Spec> using SettingsOf = std::remove_cvref_t<decltype(Spec.settings)>;
/// The settings a spec's table passed: what ports(), check() and configure() read, by row (`s.get<row>()`).
template <const auto& Spec> using CheckedOf = typename SettingsOf<Spec>::Checked;
template <const auto& Spec> using BanksOf = std::remove_cvref_t<decltype(Spec.banks)>;
template <const auto& Spec> using StoresOf = std::remove_cvref_t<decltype(Spec.stores)>;
template <const auto& Spec> using LoopsOf = std::remove_cvref_t<decltype(Spec.loops)>;

/// Every bank of a spec with its members: what a type's ports() returns, {stream(members(n)), from(members(n))}.
/// Each bank of the spec once, in any order; a bank of another spec, or one left out, does not compile.
template <const auto& Spec> class Members {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
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

/// One cycle's audio, by bank (docs/writing-a-node-type.md#the-code): io[bank] is the bank's members, each a span of
/// exactly frames() samples, an output's to write (every sample), an input's to read (an unlinked input reads zeros).
/// Only the spec's own banks are taken. The layout is resolved once, before the first cycle, so io[bank] is an index.
template <const auto& Spec> class Io {
	static_assert(linkage::named<Spec>, "a spec is declared in a named namespace, never an unnamed one");
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
/// A code whose layout the type's config hands over: CreateOf::use binds it to the node's ports (and the legacy
/// config, after its start).
class BankBoundCode : public AudioCode {
public:
	virtual void bindPorts(const Ports& ports) noexcept = 0;
	/// The spec the code was written against, to check it is its type's.
	virtual const void* spec() const noexcept = 0;
};
template <const auto& Spec, class Code> class ErasedSpecCode final : public BankBoundCode {
	// Found by name, then held to its exact signature, so a misspelt signature is refused rather than never called.
	static constexpr bool hasPrepare = requires { &Code::prepare; };
	static constexpr bool hasRelease = requires { &Code::release; };
	static consteval bool preparesExactly() {
		if constexpr (hasPrepare)
			return std::same_as<decltype(&Code::prepare), void (Code::*)(const AudioFormat&)> ||
			       std::same_as<decltype(&Code::prepare), void (Code::*)(const AudioFormat&) noexcept>;
		else return true;
	}
	static consteval bool releasesExactly() {
		if constexpr (hasRelease) return std::same_as<decltype(&Code::release), void (Code::*)() noexcept>;
		else return true;
	}
	static_assert(preparesExactly(), "a code's prepare is void prepare(const AudioFormat&)");
	static_assert(releasesExactly(), "a code's release is void release() noexcept");

public:
	explicit ErasedSpecCode(Code code) : code_(std::move(code)) {}
	void bindPorts(const Ports& ports) noexcept override { ports_ = &ports; }
	const void* spec() const noexcept override { return &Spec; }
	void prepare(const AudioFormat& format) override {
		if (ports_ == nullptr)
			throw std::logic_error("a code written against a spec runs only in a type made by makeNodeType");
		layout_ = Io<Spec>::resolve(*ports_);
		if constexpr (hasPrepare) code_.prepare(format);
	}
	void process(std::span<const std::span<const float>> inputs, std::span<const std::span<float>> outputs,
	             std::uint32_t frames, const GraphCycle& cycle) noexcept override {
		code_.process(Io<Spec>(layout_, inputs, outputs, frames), cycle);
	}
	void release() noexcept override {
		if constexpr (hasRelease) code_.release();
	}

private:
	Code code_;
	const Ports* ports_ = nullptr; // the config's, which outlives the code (teardown step 5)
	typename Io<Spec>::Layout layout_;
};
} // namespace spec_detail

template <SpecCode Code> std::unique_ptr<AudioCode> eraseSpecCode(Code code) {
	using Process = spec_detail::ProcessOf<decltype(&Code::process)>;
	return std::make_unique<spec_detail::ErasedSpecCode<Process::spec, Code>>(std::move(code));
}

} // namespace ear
