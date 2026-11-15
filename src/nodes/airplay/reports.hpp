// What shairport-sync reports through its feed, and the loop that acts on it (docs/node-types.md#airplay): the feed's
// items parsed, its listing announced through ear_mdns, its sessions said as notes. Private to the type and its tests.
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ear/core/loop.hpp"
#include "ear/core/node_handle.hpp"
#include "ear/mdns/advertiser.hpp"

namespace ear::nodes::airplay {

/// One item of shairport-sync's metadata feed: its type and code (four characters each, as big-endian numbers) and its
/// data, base64-decoded.
struct Report {
	std::uint32_t type;
	std::uint32_t code;
	std::string data;
	friend bool operator==(const Report&, const Report&) = default;
};

/// The items the node reads ('ssnc', shairport-sync's own): its listing ('svrg', from the "metadata" mDNS backend), and
/// a session's start and end ('pbeg', 'pend').
inline constexpr std::uint32_t ssnc = 0x73736e63, svrg = 0x73767267, pbeg = 0x70626567, pend = 0x70656e64;
/// The longest item kept, from its "<item>" to its "</item>": a longer one (cover art) is dropped.
inline constexpr std::size_t maxItemBytes = 65'536;

/// shairport-sync's metadata feed, read incrementally: each item is
/// `<item><type>%x</type><code>%x</code><length>%u</length>`, then optionally
/// `\n<data encoding="base64">\n<base64></data>`, then `</item>\n`. Chunks may split it anywhere. An item past
/// maxItemBytes, or one that does not parse, is dropped up to the next "<item>" and counted; anything else between
/// items but whitespace is skipped the same way.
class ReportParser {
public:
	/// Every item `bytes` completes, in order.
	std::vector<Report> feed(std::string_view bytes);
	/// How many items, or runs of garbage, were dropped so far.
	std::uint64_t dropped() const noexcept { return dropped_; }

private:
	std::string pending_;   // what was fed and not yet parsed: from an "<item>", or the start of one
	bool skipping_ = false; // dropping up to the next "<item>"
	std::uint64_t dropped_ = 0;
};

/// The listing an 'svrg' item describes (its lines `service=`, `name=`, `port=`, then a `txt=` per record), on host
/// label `host`. Error, in words: a missing line, a port that is not 1 to 65535, or what listingRefused refuses.
std::expected<mdns::Listing, std::string> listingOf(std::string_view svrgData, std::string_view host);

/// The report loop's state: the feed's items in order, on the node's strand. It opens the node's advertiser on the
/// first listing and answers its questions when its socket is readable; a later listing of the same instance updates
/// its TXT records, one of another instance replaces it. Sessions are notes, never the node's state.
class Reports {
public:
	Reports(NodeHandle& handle, Loop& loop, std::string host);
	/// The watch, then the advertiser, whose destructor is the listing's goodbye: at teardown step 4, once shairport-sync
	/// was reaped.
	~Reports();
	void onFeed(std::string_view bytes);
	Reports(const Reports&) = delete;
	Reports& operator=(const Reports&) = delete;

private:
	void onListing(std::string_view data);
	NodeHandle& handle_;
	Loop& loop_;
	std::string host_;
	ReportParser parser_;
	std::unique_ptr<mdns::Advertiser> advertiser_;
	std::unique_ptr<Timer> watch_; // readable: a question waits on the advertiser's socket
};

} // namespace ear::nodes::airplay
