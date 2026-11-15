// shairport-sync's metadata feed and the report loop (reports.hpp; docs/node-types.md#airplay).
#include "reports.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "notes.hpp"

namespace ear::nodes::airplay {
namespace {

constexpr std::string_view itemOpen = "<item>";
constexpr std::string_view itemClose = "</item>";
constexpr std::string_view dataOpen = "\n<data encoding=\"base64\">\n";
constexpr std::string_view dataClose = "</data>";

bool isSpace(char c) noexcept { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; }

/// The text between `open` and `close` at the start of `text`, which then moves past `close`.
std::optional<std::string_view> element(std::string_view& text, std::string_view open, std::string_view close) {
	if (!text.starts_with(open)) return std::nullopt;
	const std::size_t end = text.find(close, open.size());
	if (end == std::string_view::npos) return std::nullopt;
	const std::string_view inside = text.substr(open.size(), end - open.size());
	text.remove_prefix(end + close.size());
	return inside;
}

template <class Number> std::optional<Number> number(std::string_view text, int base) {
	Number value{};
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
	if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
	return value;
}

/// RFC 4648's base64, padded; shairport-sync writes it in runs of 76 characters with nothing between them.
std::optional<std::string> base64(std::string_view text) {
	constexpr std::array<signed char, 256> values = [] {
		std::array<signed char, 256> table{};
		table.fill(-1);
		constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		for (std::size_t i = 0; i < alphabet.size(); ++i)
			table[static_cast<unsigned char>(alphabet[i])] = static_cast<signed char>(i);
		return table;
	}();
	if (text.size() % 4 != 0) return std::nullopt;
	std::string bytes;
	bytes.reserve(text.size() / 4 * 3);
	for (std::size_t at = 0; at < text.size(); at += 4) {
		const bool last = at + 4 == text.size();
		std::uint32_t group = 0;
		std::size_t padding = 0;
		for (std::size_t i = 0; i < 4; ++i) {
			const char c = text[at + i];
			if (c == '=' && last && i >= 2) {
				++padding;
				group <<= 6U;
				continue;
			}
			const signed char value = values[static_cast<unsigned char>(c)];
			if (value < 0 || padding > 0) return std::nullopt;
			group = (group << 6U) | static_cast<std::uint32_t>(value);
		}
		bytes += static_cast<char>((group >> 16U) & 0xFFU);
		if (padding < 2) bytes += static_cast<char>((group >> 8U) & 0xFFU);
		if (padding < 1) bytes += static_cast<char>(group & 0xFFU);
	}
	return bytes;
}

/// One whole item, from its "<item>" to its "</item>".
std::optional<Report> parseItem(std::string_view item) {
	if (!item.starts_with(itemOpen) || !item.ends_with(itemClose)) return std::nullopt;
	item.remove_prefix(itemOpen.size());
	item.remove_suffix(itemClose.size());
	const auto type = element(item, "<type>", "</type>");
	const auto code = element(item, "<code>", "</code>");
	const auto length = element(item, "<length>", "</length>");
	if (!type || !code || !length) return std::nullopt;
	const auto typeNumber = number<std::uint32_t>(*type, 16);
	const auto codeNumber = number<std::uint32_t>(*code, 16);
	const auto bytes = number<std::size_t>(*length, 10);
	if (!typeNumber || !codeNumber || !bytes) return std::nullopt;
	Report report{.type = *typeNumber, .code = *codeNumber, .data = {}};
	if (item.empty()) return *bytes == 0 ? std::optional(report) : std::nullopt;
	const auto encoded = element(item, dataOpen, dataClose);
	if (!encoded || !item.empty()) return std::nullopt;
	auto decoded = base64(*encoded);
	if (!decoded || decoded->size() != *bytes) return std::nullopt;
	report.data = std::move(*decoded);
	return report;
}

} // namespace

std::vector<Report> ReportParser::feed(std::string_view bytes) {
	pending_ += bytes;
	std::vector<Report> reports;
	std::size_t at = 0;
	const std::string_view text(pending_);
	while (at < text.size()) {
		if (skipping_) {
			const std::size_t next = text.find(itemOpen, at);
			if (next == std::string_view::npos) {
				// Keep what could be the start of an "<item>" split across chunks.
				at = text.size() - std::min(text.size() - at, itemOpen.size() - 1);
				break;
			}
			skipping_ = false;
			at = next;
			continue;
		}
		while (at < text.size() && isSpace(text[at])) ++at;
		const std::string_view rest = text.substr(at);
		if (rest.empty()) break;
		if (!rest.starts_with(itemOpen)) {
			if (itemOpen.starts_with(rest)) break; // an "<item>" not yet whole
			++dropped_;
			skipping_ = true;
			++at;
			continue;
		}
		const std::size_t close = rest.find(itemClose);
		const std::size_t whole = close == std::string_view::npos ? std::string_view::npos : close + itemClose.size();
		if (whole == std::string_view::npos ? rest.size() > maxItemBytes : whole > maxItemBytes) {
			++dropped_;
			skipping_ = true;
			at += itemOpen.size();
			continue;
		}
		if (whole == std::string_view::npos) break; // the rest of the item has not arrived
		if (auto report = parseItem(rest.substr(0, whole))) {
			reports.push_back(std::move(*report));
			at += whole;
		} else {
			++dropped_;
			skipping_ = true;
			at += itemOpen.size();
		}
	}
	pending_.erase(0, at);
	return reports;
}

std::expected<mdns::Listing, std::string> listingOf(std::string_view svrgData, std::string_view host) {
	std::optional<std::string_view> service;
	std::optional<std::string_view> name;
	std::optional<std::string_view> port;
	std::vector<std::string> txt;
	while (!svrgData.empty()) {
		const std::size_t end = std::min(svrgData.find('\n'), svrgData.size());
		const std::string_view line = svrgData.substr(0, end);
		svrgData.remove_prefix(std::min(end + 1, svrgData.size()));
		if (line.starts_with("service=") && !service) service = line.substr(8);
		else if (line.starts_with("name=") && !name) name = line.substr(5);
		else if (line.starts_with("port=") && !port) port = line.substr(5);
		else if (line.starts_with("txt=")) txt.emplace_back(line.substr(4));
	}
	if (!service) return std::unexpected("the listing has no service line");
	if (!name) return std::unexpected("the listing has no name line");
	if (!port) return std::unexpected("the listing has no port line");
	const auto portNumber = number<std::uint32_t>(*port, 10);
	if (!portNumber || *portNumber < 1 || *portNumber > 65535)
		return std::unexpected("the listing's port " + std::string(*port) + " is not 1 to 65535");
	mdns::Listing listing{.service = std::string(*service),
	                      .instance = std::string(*name),
	                      .host = std::string(host),
	                      .port = static_cast<std::uint16_t>(*portNumber),
	                      .txt = std::move(txt)};
	if (auto why = mdns::listingRefused(listing)) return std::unexpected(std::move(*why));
	return listing;
}

Reports::Reports(NodeHandle& handle, Loop& loop, std::string host)
	: handle_(handle), loop_(loop), host_(std::move(host)) {}

Reports::~Reports() {
	for (auto& [service, one] : listed_) one.watch.reset();
	for (auto& [service, one] : listed_) one.advertiser.reset();
}

void Reports::onFeed(std::string_view bytes) {
	for (const Report& report : parser_.feed(bytes)) {
		if (report.type != ssnc) continue;
		if (report.code == svrg) onListing(report.data);
		else if (report.code == pbeg) handle_.say(std::string(notes::playing));
		else if (report.code == pend) handle_.say(std::string(notes::idle));
	}
}

void Reports::onListing(std::string_view data) {
	auto listing = listingOf(data, host_);
	if (!listing) {
		handle_.say(notes::advertiseFailed(listing.error()));
		return;
	}
	const auto found = listed_.find(listing->service);
	if (found != listed_.end()) {
		Listed& one = found->second;
		// The same instance as asked, whatever a clash renamed it to: its TXT only.
		if (one.asked.instance == listing->instance) {
			one.asked.txt = listing->txt;
			if (auto updated = one.advertiser->update(std::move(listing->txt)); !updated)
				handle_.say(notes::advertiseFailed(updated.error()));
			return;
		}
		// Another instance of the service: the old one's goodbye first, so a sender never sees both.
		listed_.erase(found);
	}
	auto opened = mdns::Advertiser::open(*listing);
	if (!opened) {
		handle_.say(notes::advertiseFailed(opened.error()));
		return;
	}
	const std::string service = listing->service;
	Listed& one = listed_[service];
	one.asked = std::move(*listing);
	one.advertiser = std::move(*opened);
	one.watch = loop_.onReadable(one.advertiser->descriptor(), [this, service] { onTurn(service); });
}

void Reports::onTurn(const std::string& service) {
	// Its watch is the entry's own, so the entry is there whenever it rings.
	const auto found = listed_.find(service);
	Listed& one = found->second;
	auto turn = one.advertiser->turn();
	if (turn.announced) {
		if (turn.announced->instance != one.asked.instance)
			handle_.say(notes::renamed(one.asked.instance, turn.announced->instance));
		handle_.say(notes::listed(turn.announced->instance, turn.announced->port));
	}
	if (turn.failed) {
		handle_.say(notes::advertiseFailed(*turn.failed));
		// Its watch goes before its advertiser, as at teardown: erasing destroys members in reverse order.
		listed_.erase(found);
	}
}

} // namespace ear::nodes::airplay
