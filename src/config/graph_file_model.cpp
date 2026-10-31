// The graph file model: ear.conf loaded with knobly and read with ear's directives
// (docs/design/knobly-config.md, "File format"; graph_file_model.hpp). knobly finds the language's
// problems; this file finds ear's (unknown and malformed directives, missing settings, @include),
// and turns knobly's places into the wire's through placeIn. Edits are made at the source on a
// fresh load of the text, through knobly's written layer, and the result is loaded again.
#include "graph_file_model.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
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
using json = nlohmann::json;

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

/// The kind `type` declares for `key`; nothing when it declares none or the type is unknown.
std::optional<SettingKind> declaredKind(const NodeType* type, std::string_view key) {
	if (type != nullptr)
		for (const auto& declared : type->settingKinds())
			if (declared.key == key) return declared.kind;
	return std::nullopt;
}

/// `<node>:<port>`, split at its first ':', each part non-empty; nothing otherwise.
std::optional<PortRef> splitEnd(const std::string& text) {
	const auto colon = text.find(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) return std::nullopt;
	return PortRef{text.substr(0, colon), text.substr(colon + 1)};
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

	/// Both format lines read, and none of knobly's diagnostics is placed on either. (ear's own never
	/// is once both read: each of them on a format line leaves its value unset, and a repeated line's
	/// sits on the repeat, outside the first line's range.)
	std::optional<AudioFormat> format(const vl::sequence<vl::located_diagnostic>& knobly) const {
		if (!frequency_.value || !blockSize_.value) return std::nullopt;
		for (const auto& diagnostic : knobly)
			if (frequency_.range->holds(diagnostic.place()) || blockSize_.range->holds(diagnostic.place())) return std::nullopt;
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
				// Undeclared, or an unknown type: read as text, and the plan judges it.
				if (declaredKind(nodeType, key).value_or(SettingKind::text) == SettingKind::wholeNumber) {
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
		auto end = splitEnd(*text);
		if (!end) report(value.error(std::format("{} is not <node>:<port>", *text), vl::program_code(badDirectiveCode)));
		return end;
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

/// knobly's processor with no extension registered, over a memory source holding only rootDocument.
vw::document_graph loadGraph(const std::string& text) {
	auto files = std::make_shared<vl::ordered_map<std::string, std::string>>();
	files->insert_or_assign(std::string(rootDocument), text);
	const vw::processor processor(vw::processor_options{.source = vl::memory_source(files)});
	return processor.load(rootDocument);
}

// ---- Edits at the source (S3's edit rules) ----

/// A text knobly can write as a string: its grammar's STRING has escapes for newline, tab and
/// carriage return only, so any other control character (U+0000 to U+001F, U+007F) has no written
/// form, and knobly's quote() would give text its own reader refuses.
bool writable(std::string_view text) {
	return std::none_of(text.begin(), text.end(), [](char c) {
		const auto byte = static_cast<unsigned char>(c);
		return (byte < 0x20 && c != '\n' && c != '\t' && c != '\r') || byte == 0x7F;
	});
}

/// knobly's quote of a text, escapes included.
std::string quoted(std::string_view text) {
	const vl::config_text quote = vl::quote(text);
	return std::string(quote.view());
}

/// A name, type or port: bare when every character is in [A-Za-z0-9_-], otherwise quoted.
std::string wordText(std::string_view word) {
	const bool bare = !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
	return bare ? std::string(word) : quoted(word);
}

std::string endText(const PortRef& end) {
	const std::string joined = end.node + ":" + end.port;
	return wordText(end.node) == end.node && wordText(end.port) == end.port ? joined
	                                                                        : quoted(joined);
}

/// A setting's key is written as a directive name: a word starting with a letter or one of _ . / * ?,
/// then letters, digits and _ . / * ? - @ # + : (knobly's language page).
bool writableKey(std::string_view key) {
	const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
	const auto first = [&](char c) { return letter(c) || std::string_view("_./*?").find(c) != std::string_view::npos; };
	const auto rest = [&](char c) {
		return first(c) || (c >= '0' && c <= '9') || std::string_view("-@#+:").find(c) != std::string_view::npos;
	};
	return !key.empty() && first(key.front()) && std::all_of(key.begin() + 1, key.end(), rest);
}

/// A text setting's value as knobly text: quoted, or refused when knobly cannot write it.
std::expected<std::string, InvalidSettings> textValue(const NodeName& node, const std::string& key, const std::string& text) {
	if (!writable(text)) return std::unexpected(InvalidSettings{node, key + ": holds a control character ear.conf cannot hold"});
	return quoted(text);
}

/// One setting's value as knobly text: text is knobly's quote of the JSON string, a whole number the
/// bare integer; an undeclared key takes a string (quoted) or an integer (bare).
std::expected<std::string, InvalidSettings> valueText(const NodeName& node, const NodeType* type,
                                                      const std::string& key, const json& value) {
	if (!writableKey(key)) return std::unexpected(InvalidSettings{node, key + ": cannot be written"});
	if (const auto kind = declaredKind(type, key)) {
		if (*kind == SettingKind::text) {
			if (!value.is_string()) return std::unexpected(InvalidSettings{node, key + ": expected text"});
			return textValue(node, key, value.get<std::string>());
		}
		if (!value.is_number_integer()) return std::unexpected(InvalidSettings{node, key + ": expected a whole number"});
		return value.dump();
	}
	if (value.is_string()) return textValue(node, key, value.get<std::string>());
	if (value.is_number_integer()) return value.dump();
	return std::unexpected(InvalidSettings{node, key + ": cannot be written"});
}

/// `<key> <value>; ...` for every key of `settings`, in JSON order.
std::expected<std::string, InvalidSettings> settingsText(const NodeName& node, const NodeType* type,
                                                         const json& settings) {
	std::string all;
	for (const auto& [key, value] : settings.items()) {
		auto written = valueText(node, type, key, value);
		if (!written) return std::unexpected(written.error());
		if (!all.empty()) all += ' ';
		all += std::format("{} {};", key, *written);
	}
	return all;
}

/// A fresh load of one text, rendered, with what the edits look up in it.
class Editing {
public:
	explicit Editing(const std::string& text) : text_(text), graph_(loadGraph(text)), tree_(vr::render(graph_)) {}

	std::optional<vr::directive> node(const NodeName& name) const {
		for (const vr::directive& directive : tree_.block().directives(nodeName))
			if (nameOf(directive) == name) return directive;
		return std::nullopt;
	}

	/// Every route naming `node` at either end, in text order.
	std::vector<vr::directive> routesOf(const NodeName& node) const {
		std::vector<vr::directive> found;
		for (const vr::directive& directive : tree_.block().directives(routeName))
			if (const auto route = routeOf(directive); route && (route->from.node == node || route->to.node == node))
				found.push_back(directive);
		return found;
	}

	std::optional<vr::directive> route(const Route& wanted) const {
		for (const vr::directive& directive : tree_.block().directives(routeName))
			if (routeOf(directive) == wanted) return directive;
		return std::nullopt;
	}

	/// SharedDefinition when the statement makes more than one rendered directive.
	std::optional<control::FileRefusal> shared(const vw::statement& statement) const {
		if (statement.copies().size() <= 1) return std::nullopt;
		return control::SharedDefinition{placeOf(text_, statement.place())};
	}

	/// Appends statements after the last top-level statement.
	void append(const std::string& statements) const { graph_.root().block().add_statements(vl::text(statements)); }

	std::string text() const { return graph_.root().text(); }
	const std::string& original() const { return text_; }

private:
	static std::optional<std::string> nameOf(const vr::directive& node) {
		const auto values = node.values();
		if (values.size() != 2) return std::nullopt;
		try {
			return values.at(1).string();
		} catch (const vl::diagnostic_error&) {
			return std::nullopt;
		}
	}

	static std::optional<Route> routeOf(const vr::directive& directive) {
		const auto values = directive.values();
		if (values.size() != 2) return std::nullopt;
		const auto end = [](const vw::expanded_value& value) -> std::optional<PortRef> {
			try {
				return splitEnd(value.string());
			} catch (const vl::diagnostic_error&) {
				return std::nullopt;
			}
		};
		const auto from = end(values.at(0));
		const auto to = end(values.at(1));
		if (!from || !to) return std::nullopt;
		return Route{*from, *to};
	}

	std::string text_;
	vw::document_graph graph_;
	vr::rendered tree_;
};

/// Removes statements last first, so a removal never moves one still to come.
void removeAll(std::vector<vw::statement> statements) {
	std::sort(statements.begin(), statements.end(), [](const vw::statement& a, const vw::statement& b) {
		return a.place().span().start().offset() > b.place().span().start().offset();
	});
	for (const auto& statement : statements) statement.remove();
}

/// Rewrites a node written without a block with one holding `settings`: the block goes after its last
/// value, in place of the `;`, and everything else the statement owns (comments) is kept.
void addBlock(const vw::written_directive& node, const std::string& text, const std::string& settings) {
	const auto span = node.place().span();
	const auto start = static_cast<std::size_t>(span.start().offset());
	const auto end = static_cast<std::size_t>(span.end().offset());
	const auto values = node.values();
	std::size_t at = static_cast<std::size_t>(values.at(values.size() - 1).place().span().end().offset());
	while (at < end && text[at] != ';') {   // whitespace and comments up to the statement's ;
		if (text[at] == '#') at = std::min(end, text.find('\n', at));
		else ++at;
	}
	if (at >= end) throw std::logic_error("graph file model: a node without a block has no ;");
	const std::string rewritten = text.substr(start, at - start) + " { " + settings + " }" + text.substr(at + 1, end - at - 1);
	node.replace_with_text(vl::text(rewritten));
}

GraphFileRead onlyError(std::string_view code, std::string message) {
	GraphFileRead read;
	read.diagnostics.push_back(
	    control::Diagnostic{control::Severity::error, std::string(code), std::move(message), control::TextPlace{1, 1}});
	return read;
}

const GraphSpec& wantedOf(const GraphFileRead& read) {
	if (!read.wanted) throw std::logic_error("graph file model: an edit needs a file without errors");
	return *read.wanted;
}

const NodeSpec* heldNode(const GraphSpec& wanted, const NodeName& name) {
	const auto found = std::find_if(wanted.nodes.begin(), wanted.nodes.end(), [&](const NodeSpec& n) { return n.name == name; });
	return found == wanted.nodes.end() ? nullptr : &*found;
}

std::unexpected<EditRefusal> refuse(CommandError error) { return std::unexpected(EditRefusal{std::move(error)}); }
std::unexpected<EditRefusal> refuse(control::FileRefusal refusal) { return std::unexpected(EditRefusal{std::move(refusal)}); }

/// Runs one edit. Every text an edit writes is checked first, so knobly's written layer throwing is a
/// backstop: an invalid-text refusal, never an exception escaping to the keeper's thread (which would
/// end ear). knobly_error: the text is not what the place takes; misuse_error: quote() was given
/// text that is not UTF-8. ear's own logic_errors are bugs and are not caught.
template <class Edit> EditOutcome guarded(Edit edit) {
	const auto unwritable = [](std::string_view what) {
		return refuse(control::FileRefusal{control::InvalidText{{control::Diagnostic{
		    control::Severity::error, std::string(unwritableEditCode),
		    std::format("the edit cannot be written in {}: {}", rootDocument, what), control::TextPlace{1, 1}}}}});
	};
	try {
		return edit();
	} catch (const vl::knobly_error& error) {
		return unwritable(error.what());
	} catch (const vl::misuse_error& error) {
		return unwritable(error.what());
	}
}

} // namespace

bool GraphFileRead::hasErrors() const noexcept {
	return std::any_of(diagnostics.begin(), diagnostics.end(),
	                   [](const control::Diagnostic& d) { return d.severity == control::Severity::error; });
}

struct GraphFileModel::Impl {
	const NodeCatalogue* catalogue;
	std::string text;
	std::optional<vw::document_graph> graph;   // absent when ear.conf is missing or unreadable
	GraphFileRead read;
};

GraphFileModel::GraphFileModel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GraphFileModel::GraphFileModel(GraphFileModel&& other) noexcept = default;
GraphFileModel& GraphFileModel::operator=(GraphFileModel&& other) noexcept = default;
GraphFileModel::~GraphFileModel() = default;

GraphFileModel GraphFileModel::fresh(const DiskText& disk) const { return load(disk, *impl_->catalogue); }

const std::string& GraphFileModel::text() const noexcept { return impl_->text; }

const GraphFileRead& GraphFileModel::read() const noexcept { return impl_->read; }

GraphFileModel GraphFileModel::load(const DiskText& disk, const NodeCatalogue& catalogue) {
	auto impl = std::make_unique<Impl>();
	impl->catalogue = &catalogue;
	if (!disk) {
		impl->read = onlyError(unreadableFileCode, std::format("{} cannot be read: {}", rootDocument, disk.error()));
		return GraphFileModel(std::move(impl));
	}
	if (!disk->has_value()) {
		impl->read = onlyError(missingFileCode, std::format("{} is not there", rootDocument));
		return GraphFileModel(std::move(impl));
	}
	if ((*disk)->size() > maxDocumentBytes) {   // an edit's or a set's candidate: the disk refuses a larger file
		impl->read = onlyError(tooLargeCode, std::format("{} would be larger than {} bytes, the most ear reads",
		                                                 rootDocument, maxDocumentBytes));
		return GraphFileModel(std::move(impl));
	}
	impl->text = **disk;

	impl->graph = loadGraph(impl->text);
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

namespace {

/// The candidate: the edited text loaded again; nothing when the text did not change.
EditOutcome finish(const Editing& editing, const NodeCatalogue& catalogue) {
	std::string after = editing.text();
	if (after == editing.original()) return std::optional<GraphFileModel>{};
	return std::optional<GraphFileModel>(GraphFileModel::load(DiskText{std::move(after)}, catalogue));
}

} // namespace

EditOutcome GraphFileModel::updateNode(const NodeName& name, const NodeSettings& settings) const {
	return guarded([&]() -> EditOutcome {
		const NodeSpec* held = heldNode(wantedOf(impl_->read), name);
		if (held == nullptr) return refuse(UnknownNode{name});
		if (!settings.is_object()) return refuse(InvalidSettings{name, "settings: must be an object"});
		if (held->settings == settings) return std::optional<GraphFileModel>{};
		const NodeType* type = impl_->catalogue->find(held->type);
		std::map<std::string, std::string> written;   // every key's value as knobly text, checked before any edit
		for (const auto& [key, value] : settings.items()) {
			auto text = valueText(name, type, key, value);
			if (!text) return refuse(text.error());
			written.emplace(key, std::move(*text));
		}

		const Editing editing(impl_->text);
		const auto node = editing.node(name);
		if (!node) throw std::logic_error("graph file model: a wanted node is not in its render");
		std::vector<std::pair<vw::written_value, std::string>> changes;
		std::vector<vw::statement> removals;
		std::set<std::string> present;
		if (const auto block = node->block())
			for (const vr::directive& setting : *block) {
				const std::string key = setting.name();
				present.insert(key);
				if (!settings.contains(key)) {
					if (auto shared = editing.shared(setting.written())) return refuse(std::move(*shared));
					removals.push_back(setting.written());
				} else if (settings.at(key) != held->settings.at(key)) {
					const auto value = setting.values().at(0);
					changes.emplace_back(value.definition().value_or(value.written()), written.at(key));
				}
			}
		std::vector<std::string> added;   // `<key> <value>;`, in JSON order
		for (const auto& [key, text] : written)
			if (!present.contains(key)) added.push_back(std::format("{} {};", key, text));
		const vw::written_directive statement = node->written();
		if (!added.empty())
			if (auto shared = editing.shared(statement)) return refuse(std::move(*shared));

		const auto joined = [&](std::string_view between) {
			std::string all;
			for (const auto& one : added) all += (all.empty() ? "" : std::string(between)) + one;
			return all;
		};
		for (const auto& [value, text] : changes) value.set(vl::text(text));
		if (!added.empty()) {
			if (const auto block = statement.block()) block->add_statements(vl::text(joined("\n")));   // a line each
			else addBlock(statement, editing.original(), joined(" "));
		}
		removeAll(std::move(removals));
		return finish(editing, *impl_->catalogue);
	});
}

EditOutcome GraphFileModel::addNode(const NodeSpec& node) const {
	return guarded([&]() -> EditOutcome {
		const NodeSpec* held = heldNode(wantedOf(impl_->read), node.name);
		if (held != nullptr && *held == node) return std::optional<GraphFileModel>{};
		// No type or valid name holds a control character (InvalidName: 1-64 of [A-Za-z0-9_-]).
		if (!writable(node.type)) return refuse(UnknownNodeType{node.type});
		if (!writable(node.name)) return refuse(InvalidName{node.name});
		if (!node.settings.is_object()) return refuse(InvalidSettings{node.name, "settings: must be an object"});
		auto settings = settingsText(node.name, impl_->catalogue->find(node.type), node.settings);
		if (!settings) return refuse(settings.error());
		const Editing editing(impl_->text);
		const std::string head = std::format("node {} {}", wordText(node.type), wordText(node.name));
		editing.append(settings->empty() ? head + ";" : std::format("{} {{ {} }}", head, *settings));
		return finish(editing, *impl_->catalogue);
	});
}

EditOutcome GraphFileModel::removeNode(const NodeName& name) const {
	return guarded([&]() -> EditOutcome {
		if (heldNode(wantedOf(impl_->read), name) == nullptr) return refuse(UnknownNode{name});
		const Editing editing(impl_->text);
		const auto node = editing.node(name);
		if (!node) throw std::logic_error("graph file model: a wanted node is not in its render");
		std::vector<vw::statement> removals{node->written()};
		for (const vr::directive& route : editing.routesOf(name))
			if (std::find(removals.begin(), removals.end(), route.written()) == removals.end())
				removals.push_back(route.written());
		for (const auto& statement : removals)
			if (auto shared = editing.shared(statement)) return refuse(std::move(*shared));
		removeAll(std::move(removals));
		return finish(editing, *impl_->catalogue);
	});
}

EditOutcome GraphFileModel::addRoute(const Route& route) const {
	return guarded([&]() -> EditOutcome {
		const GraphSpec& wanted = wantedOf(impl_->read);
		const auto& routes = wanted.routes;
		if (std::find(routes.begin(), routes.end(), route) != routes.end()) return std::optional<GraphFileModel>{};
		// A node in the graph has a valid name and no type declares a port with a control character, so
		// an end knobly cannot write names nothing in the graph: refused as the plan would refuse it.
		for (const PortRef& end : {route.from, route.to}) {
			if (writable(end.node) && writable(end.port)) continue;
			if (heldNode(wanted, end.node) == nullptr) return refuse(UnknownNode{end.node});
			return refuse(UnknownPort{end});
		}
		const Editing editing(impl_->text);
		editing.append(std::format("route {} {};", endText(route.from), endText(route.to)));
		return finish(editing, *impl_->catalogue);
	});
}

EditOutcome GraphFileModel::removeRoute(const Route& route) const {
	return guarded([&]() -> EditOutcome {
		const auto& routes = wantedOf(impl_->read).routes;
		if (std::find(routes.begin(), routes.end(), route) == routes.end()) return refuse(UnknownRoute{route});
		const Editing editing(impl_->text);
		const auto found = editing.route(route);
		if (!found) throw std::logic_error("graph file model: a wanted route is not in its render");
		if (auto shared = editing.shared(found->written())) return refuse(std::move(*shared));
		found->written().remove();
		return finish(editing, *impl_->catalogue);
	});
}

} // namespace ear::config
