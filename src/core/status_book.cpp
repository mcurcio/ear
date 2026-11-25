#include "status_book.hpp"
#include <algorithm>
#include <format>
#include <utility>
namespace ear {
std::string valueText(const StatusValue& value, std::uint8_t decimals) {
	if (std::holds_alternative<std::monostate>(value)) return "unknown";
	if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "true" : "false";
	if (const auto* number = std::get_if<std::int64_t>(&value)) return std::to_string(*number);
	if (const auto* decimal = std::get_if<double>(&value)) {
		std::string text = std::format("{:.{}f}", *decimal, decimals);
		if (text.find_first_not_of("-0.") == std::string::npos)
			text = decimals == 0 ? "0" : text.substr(text[0] == '-');
		return text;
	}
	return std::get<std::string>(value);
}

namespace {
/// The length of the UTF-8 sequence `lead` begins, 0 for a byte that begins none.
std::size_t sequenceLength(unsigned char lead) {
	if (lead < 0x80) return 1;
	if (lead >= 0xC2 && lead <= 0xDF) return 2;
	if (lead >= 0xE0 && lead <= 0xEF) return 3;
	if (lead >= 0xF0 && lead <= 0xF4) return 4;
	return 0;
}

/// Whether `text` holds a well-formed sequence of `length` at `at` (no overlong form, no surrogate, nothing above
/// U+10FFFF).
bool wellFormed(std::string_view text, std::size_t at, std::size_t length) {
	if (at + length > text.size()) return false;
	const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[at + i]); };
	for (std::size_t i = 1; i < length; ++i)
		if ((byte(i) & 0xC0) != 0x80) return false;
	if (length == 3) {
		if (byte(0) == 0xE0 && byte(1) < 0xA0) return false;
		if (byte(0) == 0xED && byte(1) >= 0xA0) return false;
	}
	if (length == 4) {
		if (byte(0) == 0xF0 && byte(1) < 0x90) return false;
		if (byte(0) == 0xF4 && byte(1) >= 0x90) return false;
	}
	return true;
}

constexpr std::string_view replacement = "\xEF\xBF\xBD";
constexpr std::string_view ellipsis = "\xE2\x80\xA6";
} // namespace

std::string safeText(std::string_view text, std::size_t maxBytes) {
	std::string safe;
	safe.reserve(std::min(text.size(), maxBytes));
	std::vector<std::size_t> boundaries; // where each character of `safe` begins
	for (std::size_t at = 0; at < text.size();) {
		const auto lead = static_cast<unsigned char>(text[at]);
		const std::size_t length = sequenceLength(lead);
		boundaries.push_back(safe.size());
		if (length == 0 || !wellFormed(text, at, length)) {
			safe += replacement;
			++at;
			continue;
		}
		if (length == 1 && (lead < 0x20 || lead == 0x7F))
			safe += (lead == '\t' || lead == '\n' || lead == '\r') ? " " : replacement;
		else safe.append(text.substr(at, length));
		at += length;
	}
	if (safe.size() <= maxBytes) return safe;
	if (maxBytes < ellipsis.size()) return {};
	const std::size_t room = maxBytes - ellipsis.size();
	std::size_t cut = 0;
	for (const std::size_t boundary : boundaries)
		if (boundary <= room) cut = boundary;
	// A character that ends exactly at the room's edge fits too.
	const auto next = std::ranges::upper_bound(boundaries, cut);
	const std::size_t end = next == boundaries.end() ? safe.size() : *next;
	if (end <= room) cut = end;
	safe.resize(cut);
	safe += ellipsis;
	return safe;
}

} // namespace ear

namespace ear::detail {

bool ReportQueue::push(ReportRecord record) {
	const std::scoped_lock lock(mutex_);
	if (records_.size() >= bound && record.kind != ReportRecord::Kind::lost) {
		++dropped_;
		return false;
	}
	records_.push_back(std::move(record));
	return !std::exchange(woken_, true);
}

std::deque<ReportRecord> ReportQueue::take() {
	const std::scoped_lock lock(mutex_);
	woken_ = false;
	return std::exchange(records_, {});
}

std::uint64_t ReportQueue::takeDropped() {
	const std::scoped_lock lock(mutex_);
	return std::exchange(dropped_, 0);
}

StatusBook::StatusBook(std::span<const StatusDeclaration* const> fields, std::span<const EventDeclaration> events)
	: events_(events) {
	fields_.reserve(fields.size());
	for (const StatusDeclaration* declared : fields) {
		Field field{.declared = declared, .value = declared->initial(), .written = declared->initial()};
		fields_.push_back(std::move(field));
	}
}

void StatusBook::refillOf(double& tokens, StatusClock::time_point& filled, StatusClock::time_point now) {
	if (now > filled) {
		const double seconds = std::chrono::duration<double>(now - filled).count();
		tokens = std::min(burst, tokens + seconds / std::chrono::duration<double>(refill).count());
	}
	filled = now;
}

std::vector<FieldValue> StatusBook::view() const {
	std::vector<FieldValue> fields;
	fields.reserve(fields_.size());
	for (const Field& field : fields_) fields.push_back({field.declared->name, field.value});
	return fields;
}

void StatusBook::flush(StatusBookEvents& out) {
	std::vector<FieldValue> marked;
	for (Field& field : fields_)
		if (std::exchange(field.marked, false)) marked.push_back({field.declared->name, field.value});
	if (!marked.empty()) out.changed(marked);
}

void StatusBook::written(Field& field, StatusClock::time_point now, StatusBookEvents& out) {
	refillOf(field.tokens, field.filled, now);
	if (field.tokens < 1 || field.heldBack > 0) {
		// Held back: a trailing record writes the view once the pace allows (tick), so the last record matches it.
		++field.heldBack;
		++statusHeld_;
		return;
	}
	field.tokens -= 1;
	out.written({field.declared->name, field.written, field.value, 0, now});
	field.written = field.value;
}

void StatusBook::flapped(Field& field, StatusClock::time_point now, StatusBookEvents& out) {
	const FlapsAbove limit = field.declared->flapsAbove;
	const auto window = std::chrono::milliseconds(limit.windowMs);
	field.transitions.push_back(now);
	while (!field.transitions.empty() && field.transitions.front() <= now - window) field.transitions.pop_front();
	if (field.transitions.size() <= limit.transitions) return;
	field.lastAbove = now;
	if (field.flapping && now - field.flapSaid < window) return;
	field.flapping = true;
	field.flapSaid = now;
	const auto changes = static_cast<std::int64_t>(field.transitions.size());
	out.diagnosed({.code = "ear.status-flapping",
	               .subject = std::string(field.declared->name),
	               .condition = true,
	               .active = true,
	               .detail = std::format("{} flapping: {} changes in {}", field.declared->name, changes,
	                                     limit.windowMs % 1000 == 0 ? std::format("{}s", limit.windowMs / 1000)
	                                                                : std::format("{}ms", limit.windowMs)),
	               .fields = {{"transitions", changes}, {"windowMs", static_cast<std::int64_t>(limit.windowMs)}}});
}

void StatusBook::raise(const ReportRecord& record, StatusClock::time_point now, StatusBookEvents& out) {
	if (record.index >= events_.size()) return;
	refillOf(eventTokens_, eventsFilled_, now);
	if (eventTokens_ < 1) {
		++eventsHeld_;
		return;
	}
	eventTokens_ -= 1;
	const EventDeclaration& declared = events_[record.index];
	RaisedEvent event{.code = declared.code, .severity = declared.severity, .args = {}, .msg = {}, .at = record.at};
	for (std::size_t at = 0; at < declared.args.size() && at < record.args.size(); ++at)
		event.args.push_back({declared.args[at].name, record.args[at]});
	const std::string_view sentence = declared.sentence;
	for (std::size_t at = 0; at < sentence.size(); ++at) {
		if (sentence[at] != '{') {
			event.msg += sentence[at];
			continue;
		}
		const std::size_t end = sentence.find('}', at);
		const std::string_view place = sentence.substr(at + 1, end - at - 1);
		for (std::size_t one = 0; one < declared.args.size() && one < record.args.size(); ++one)
			if (declared.args[one].name == place) event.msg += valueText(record.args[one], declared.args[one].decimals);
		at = end;
	}
	out.raised(event);
}

void StatusBook::apply(std::deque<ReportRecord> records, std::uint64_t queueDropped, StatusClock::time_point now,
                       StatusBookEvents& out) {
	queueDropped_ += queueDropped;
	for (ReportRecord& record : records) {
		switch (record.kind) {
		case ReportRecord::Kind::status: {
			if (record.index >= fields_.size()) break;
			Field& field = fields_[record.index];
			if (field.value == record.value) break; // no change: no record, no transition
			field.value = std::move(record.value);
			field.marked = true;
			written(field, record.at, out);
			flapped(field, record.at, out);
			break;
		}
		case ReportRecord::Kind::event:
			flush(out);
			raise(record, record.at, out);
			break;
		case ReportRecord::Kind::lost:
			flush(out);
			out.lost(std::move(record.detail));
			break;
		}
	}
	static_cast<void>(now);
}

void StatusBook::dropsSaid(StatusClock::time_point now, StatusBookEvents& out) {
	const auto mayTell = [&](const std::optional<StatusClock::time_point>& said) {
		return !said || now - *said >= refill;
	};
	if (eventsHeld_ > 0 && mayTell(eventsSaid_)) {
		const auto held = static_cast<std::int64_t>(std::exchange(eventsHeld_, 0));
		eventsSaid_ = now;
		out.diagnosed({.code = "ear.events-dropped",
		               .detail = std::format("{} {} held back by the node's event rate", held,
		                                     held == 1 ? "event was" : "events were"),
		               .fields = {{"heldBack", held}}});
	}
	if (statusHeld_ > 0 && mayTell(statusSaid_)) {
		const auto held = static_cast<std::int64_t>(std::exchange(statusHeld_, 0));
		statusSaid_ = now;
		out.diagnosed({.code = "ear.records-dropped",
		               .subject = "status",
		               .detail = std::format("{} status {} held back by a field's pace", held,
		                                     held == 1 ? "record was" : "records were"),
		               .fields = {{"count", held}}});
	}
	if (queueDropped_ > 0 && mayTell(queueSaid_)) {
		const auto dropped = static_cast<std::int64_t>(std::exchange(queueDropped_, 0));
		queueSaid_ = now;
		out.diagnosed({.code = "ear.records-dropped",
		               .subject = "queue",
		               .detail = std::format("{} {} for the node's full queue", dropped,
		                                     dropped == 1 ? "record had no room" : "records had no room"),
		               .fields = {{"count", dropped}}});
	}
}

void StatusBook::tick(StatusClock::time_point now, StatusBookEvents& out) {
	flush(out);
	for (Field& field : fields_) {
		if (field.heldBack > 0) {
			refillOf(field.tokens, field.filled, now);
			if (field.tokens >= 1) {
				field.tokens -= 1;
				out.written({field.declared->name, field.written, field.value, field.heldBack, now});
				field.written = field.value;
				field.heldBack = 0;
			}
		}
		if (field.flapping) {
			const FlapsAbove limit = field.declared->flapsAbove;
			const auto window = std::chrono::milliseconds(limit.windowMs);
			while (!field.transitions.empty() && field.transitions.front() <= now - window)
				field.transitions.pop_front();
			if (field.transitions.size() <= limit.transitions && now - field.lastAbove >= window) {
				field.flapping = false;
				const auto changes = static_cast<std::int64_t>(field.transitions.size());
				out.diagnosed(
					{.code = "ear.status-flapping",
				     .subject = std::string(field.declared->name),
				     .condition = true,
				     .active = false,
				     .detail = std::format("{} settled: {} changes in the last {} ms", field.declared->name, changes,
				                           limit.windowMs),
				     .fields = {{"transitions", changes}, {"windowMs", static_cast<std::int64_t>(limit.windowMs)}}});
			}
		}
	}
	dropsSaid(now, out);
}

std::optional<StatusClock::time_point> StatusBook::due() const {
	std::optional<StatusClock::time_point> soonest;
	const auto sooner = [&](StatusClock::time_point at) {
		if (!soonest || at < *soonest) soonest = at;
	};
	for (const Field& field : fields_) {
		if (field.heldBack > 0) {
			// The pace allows the trailing record once a token refilled.
			const double missing = std::max(0.0, 1.0 - field.tokens);
			sooner(field.filled + std::chrono::duration_cast<StatusClock::duration>(std::chrono::duration<double>(
									  missing * std::chrono::duration<double>(refill).count())));
		}
		if (field.flapping) sooner(field.lastAbove + std::chrono::milliseconds(field.declared->flapsAbove.windowMs));
	}
	if (eventsHeld_ > 0) sooner(eventsSaid_ ? *eventsSaid_ + refill : StatusClock::time_point{});
	if (statusHeld_ > 0) sooner(statusSaid_ ? *statusSaid_ + refill : StatusClock::time_point{});
	if (queueDropped_ > 0) sooner(queueSaid_ ? *queueSaid_ + refill : StatusClock::time_point{});
	return soonest;
}

void StatusBook::reset(StatusClock::time_point now, StatusBookEvents& out) {
	flush(out);
	if (fields_.empty()) return;
	for (Field& field : fields_) {
		field.value = field.declared->initial();
		// A held-back change is moot: the reset record follows and says every field's value.
		field.heldBack = 0;
		field.written = field.value;
	}
	out.reset(view());
	static_cast<void>(now);
}
} // namespace ear::detail
