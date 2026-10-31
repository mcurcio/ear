// The graph file model: ear.conf loaded with knobly and read with ear's directives
// (docs/design/knobly-config.md, "File format"; graph_file_model.hpp). knobly finds the language's
// problems; this file finds ear's (unknown and malformed directives, missing settings, @include),
// and turns knobly's places into the wire's through placeIn.
#include "graph_file_model.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <knobly/knobly.hpp>
#include <knobly/rendered.hpp>
#include <knobly/written.hpp>

#include "place.hpp"

namespace ear::config {
namespace {

namespace vl = knobly;
namespace vw = knobly::written;
namespace vr = knobly::rendered;

constexpr std::string_view graphFrequencyName = "graph_frequency";
constexpr std::string_view blockSizeName = "block_size";
constexpr std::string_view nodeName = "node";
constexpr std::string_view routeName = "route";
constexpr std::string_view includeWord = "@include";
constexpr std::string_view includeMessage = "@include is not supported yet: write the whole graph in ear.conf";

control::TextPlace placeOf(std::string_view text, const vl::place& place) {
	const auto start = place.span().start();
	return placeIn(text, static_cast<std::uint32_t>(start.line()), static_cast<std::uint32_t>(start.column()));
}

control::Diagnostic plain(std::string_view text, const vl::located_diagnostic& diagnostic) {
	const auto code = diagnostic.code();
	std::string name = std::holds_alternative<vl::diagnostic_code>(code)
	                       ? vl::to_string(std::get<vl::diagnostic_code>(code))
	                       : std::string(std::get<vl::program_code>(code).view());
	return control::Diagnostic{
	    diagnostic.severity() == vl::severity::warning ? control::Severity::warning : control::Severity::error,
	    std::move(name), diagnostic.message(), placeOf(text, diagnostic.place())};
}

/// knobly's E3008 for the @include word: ear has no include extension yet.
bool isUnsupportedInclude(std::string_view text, const vl::located_diagnostic& diagnostic) {
	const auto code = diagnostic.code();
	if (!std::holds_alternative<vl::diagnostic_code>(code) ||
	    std::get<vl::diagnostic_code>(code) != vl::diagnostic_code::unknown_extension)
		return false;
	const auto span = diagnostic.place().span();
	const auto start = static_cast<std::size_t>(span.start().offset());
	const auto end = static_cast<std::size_t>(span.end().offset());
	return start <= end && end <= text.size() && text.substr(start, end - start) == includeWord;
}

/// The byte range of one statement of ear.conf: a diagnostic starting inside it is placed on it.
struct ByteRange {
	std::int64_t start;
	std::int64_t end;
	bool holds(const vl::place& place) const {
		const auto at = place.span().start().offset();
		return at >= start && at < end;
	}
};

ByteRange rangeOf(const vr::directive& directive) {
	const auto span = directive.written().place().span();
	return ByteRange{span.start().offset(), span.end().offset()};
}

/// One of the two format lines as read: its value when it read, and where it is written.
struct FormatLine {
	std::optional<std::uint32_t> value;
	std::optional<ByteRange> range;
	bool seen = false;
};

/// Reads ear.conf's rendered tree with ear's directives, collecting ear's diagnostics.
class Reader {
public:
	Reader(std::string_view text, const NodeCatalogue& catalogue) : text_(text), catalogue_(catalogue) {}

	void readTop(const vr::rendered& tree) {
		for (const vr::directive& directive : tree.block()) {
			const std::string name = directive.name();
			if (name == graphFrequencyName) readFormatLine(directive, frequency_);
			else if (name == blockSizeName) readFormatLine(directive, blockSize_);
			else if (name == nodeName) readNode(directive);
			else if (name == routeName) readRoute(directive);
			else
				report(directive.error(std::format("{} is not one of ear's directives: ear.conf takes graph_frequency, "
				                                   "block_size, node and route",
				                                   name),
				                       vl::program_code(unknownDirectiveCode)));
		}
		for (const auto& [line, missing] : {std::pair{graphFrequencyName, &frequency_}, std::pair{blockSizeName, &blockSize_}})
			if (!missing->seen)
				report(tree.error(std::format("{} is missing: write {} <whole number>;", line, line),
				                  vl::program_code(missingDirectiveCode)));
	}

	/// Both format lines read, and no diagnostic (knobly's or ear's) is placed on either.
	std::optional<AudioFormat> format(const vl::sequence<vl::located_diagnostic>& knobly) const {
		if (!frequency_.value || !blockSize_.value) return std::nullopt;
		const auto placedOn = [&](const vl::place& place) {
			return frequency_.range->holds(place) || blockSize_.range->holds(place);
		};
		for (const auto& diagnostic : knobly)
			if (placedOn(diagnostic.place())) return std::nullopt;
		for (const auto& diagnostic : found_)
			if (placedOn(diagnostic.place())) return std::nullopt;
		return AudioFormat{*frequency_.value, *blockSize_.value};
	}

	/// ear's diagnostics, in text order.
	std::vector<control::Diagnostic> diagnostics() const {
		std::vector<control::Diagnostic> all;
		for (const auto& diagnostic : found_) all.push_back(plain(text_, diagnostic));
		std::stable_sort(all.begin(), all.end(), [](const control::Diagnostic& a, const control::Diagnostic& b) {
			return std::pair{a.place.line, a.place.column} < std::pair{b.place.line, b.place.column};
		});
		return all;
	}

	std::vector<NodeSpec> nodes;
	std::vector<Route> routes;
	Placements placements;

private:
	void report(const vl::diagnostic_error& error) { found_.push_back(error.diagnostic()); }

	/// Runs one knobly reader; its diagnostic (a value of the wrong kind or spelling) is recorded.
	template <class Read> auto attempt(Read read) -> std::optional<decltype(read())> {
		try {
			return read();
		} catch (const vl::diagnostic_error& error) {
			report(error);
			return std::nullopt;
		}
	}

	bool oneValueNoBlock(const vr::directive& directive) {
		if (directive.values().size() == 1 && !directive.block()) return true;
		report(directive.error(std::format("{} takes one value and no block", directive.name()),
		                       vl::program_code(badDirectiveCode)));
		return false;
	}

	void readFormatLine(const vr::directive& directive, FormatLine& line) {
		if (line.seen) {
			report(directive.error(std::format("{} is given twice", directive.name()), vl::program_code(badDirectiveCode)));
			return;
		}
		line.seen = true;
		line.range = rangeOf(directive);
		if (!oneValueNoBlock(directive)) return;
		const auto value = directive.values().at(0);
		const auto number = attempt([&] { return value.integer(); });
		if (!number) return;
		if (*number < 1 || *number > std::numeric_limits<std::uint32_t>::max()) {
			report(value.error(std::format("{} takes a whole number from 1 to {}", directive.name(),
			                               std::numeric_limits<std::uint32_t>::max()),
			                   vl::program_code(badDirectiveCode)));
			return;
		}
		line.value = static_cast<std::uint32_t>(*number);
	}

	SettingKind kindOf(const NodeType* type, std::string_view key) const {
		if (type != nullptr)
			for (const auto& declared : type->settingKinds())
				if (declared.key == key) return declared.kind;
		return SettingKind::text;   // undeclared, or an unknown type: the plan judges it
	}

	void readNode(const vr::directive& directive) {
		const auto values = directive.values();
		if (values.size() != 2) {
			report(directive.error("node takes a type and a name: node <type> <name> [{ <setting> <value>; ... }]",
			                       vl::program_code(badDirectiveCode)));
			return;
		}
		const auto type = attempt([&] { return values.at(0).string(); });
		const auto name = attempt([&] { return values.at(1).string(); });
		bool ok = type && name;
		NodeSettings settings = NodeSettings::object();
		if (const auto block = directive.block()) {
			const NodeType* nodeType = type ? catalogue_.find(*type) : nullptr;
			for (const vr::directive& setting : *block) {
				const std::string key = setting.name();
				if (settings.contains(key)) {
					report(setting.error(std::format("setting {} is given twice", key), vl::program_code(badDirectiveCode)));
					ok = false;
					continue;
				}
				if (!oneValueNoBlock(setting)) {
					ok = false;
					continue;
				}
				const auto value = setting.values().at(0);
				if (kindOf(nodeType, key) == SettingKind::wholeNumber) {
					if (const auto number = attempt([&] { return value.integer(); })) settings[key] = *number;
					else ok = false;
				} else {
					if (const auto text = attempt([&] { return value.string(); })) settings[key] = *text;
					else ok = false;
				}
			}
		}
		if (!ok) return;
		nodes.push_back(NodeSpec{*name, *type, std::move(settings)});
		placements.nodes.push_back(placeOf(text_, directive.written().place()));
	}

	std::optional<PortRef> readEnd(const vw::expanded_value& value) {
		const auto text = attempt([&] { return value.string(); });
		if (!text) return std::nullopt;
		const auto colon = text->find(':');
		if (colon == std::string::npos || colon == 0 || colon + 1 == text->size()) {
			report(value.error(std::format("{} is not <node>:<port>", *text), vl::program_code(badDirectiveCode)));
			return std::nullopt;
		}
		return PortRef{text->substr(0, colon), text->substr(colon + 1)};
	}

	void readRoute(const vr::directive& directive) {
		const auto values = directive.values();
		if (values.size() != 2 || directive.block()) {
			report(directive.error("route takes two ports and no block: route <node>:<port> <node>:<port>;",
			                       vl::program_code(badDirectiveCode)));
			return;
		}
		const auto from = readEnd(values.at(0));
		const auto to = readEnd(values.at(1));
		if (!from || !to) return;
		routes.push_back(Route{*from, *to});
		placements.routes.push_back(placeOf(text_, directive.written().place()));
	}

	std::string_view text_;
	const NodeCatalogue& catalogue_;
	FormatLine frequency_;
	FormatLine blockSize_;
	std::vector<vl::located_diagnostic> found_;
};

GraphFileRead onlyError(std::string_view code, std::string message) {
	GraphFileRead read;
	read.diagnostics.push_back(
	    control::Diagnostic{control::Severity::error, std::string(code), std::move(message), control::TextPlace{1, 1}});
	return read;
}

} // namespace

bool GraphFileRead::hasErrors() const noexcept {
	return std::any_of(diagnostics.begin(), diagnostics.end(),
	                   [](const control::Diagnostic& d) { return d.severity == control::Severity::error; });
}

struct GraphFileModel::Impl {
	std::string text;
	std::optional<vw::document_graph> graph;   // absent when ear.conf is missing or unreadable
	GraphFileRead read;
};

GraphFileModel::GraphFileModel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GraphFileModel::GraphFileModel(GraphFileModel&& other) noexcept = default;
GraphFileModel& GraphFileModel::operator=(GraphFileModel&& other) noexcept = default;
GraphFileModel::~GraphFileModel() = default;

const std::string& GraphFileModel::text() const noexcept { return impl_->text; }

const GraphFileRead& GraphFileModel::read() const noexcept { return impl_->read; }

GraphFileModel GraphFileModel::load(const DiskText& disk, const NodeCatalogue& catalogue) {
	auto impl = std::make_unique<Impl>();
	if (!disk) {
		impl->read = onlyError(unreadableFileCode, std::format("{} cannot be read: {}", rootDocument, disk.error()));
		return GraphFileModel(std::move(impl));
	}
	if (!disk->has_value()) {
		impl->read = onlyError(missingFileCode, std::format("{} is not there", rootDocument));
		return GraphFileModel(std::move(impl));
	}
	impl->text = **disk;

	auto files = std::make_shared<vl::ordered_map<std::string, std::string>>();
	files->insert_or_assign(std::string(rootDocument), impl->text);
	const vw::processor processor(vw::processor_options{.source = vl::memory_source(files)});
	impl->graph = processor.load(rootDocument);
	const vr::rendered tree = vr::render(*impl->graph);
	const auto knobly = tree.diagnostics();

	GraphFileRead& read = impl->read;
	for (const auto& diagnostic : knobly) {
		if (isUnsupportedInclude(impl->text, diagnostic))
			read.diagnostics.push_back(control::Diagnostic{control::Severity::error, std::string(includeUnsupportedCode),
			                                               std::string(includeMessage),
			                                               placeOf(impl->text, diagnostic.place())});
		else
			read.diagnostics.push_back(plain(impl->text, diagnostic));
	}
	Reader reader(impl->text, catalogue);
	reader.readTop(tree);
	for (auto& diagnostic : reader.diagnostics()) read.diagnostics.push_back(std::move(diagnostic));
	read.format = reader.format(knobly);
	if (!read.hasErrors() && read.format) {
		read.wanted = GraphSpec{read.format->rate, read.format->maxBlock, std::move(reader.nodes), std::move(reader.routes)};
		read.placements = std::move(reader.placements);
	}
	return GraphFileModel(std::move(impl));
}

} // namespace ear::config
