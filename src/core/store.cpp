#include "ear/core/store.hpp"

#include <stdexcept>
#include <utility>

#include "caught.hpp"
#include "stores.hpp"

namespace ear {

std::string storeFileName(std::string_view type, std::string_view node, std::string_view suffix) {
	std::string file(type);
	file += '-';
	file += node;
	file += '.';
	file += suffix;
	return file;
}

namespace detail {

std::string StoreState::text() const {
	const std::scoped_lock lock(mutex_);
	if (!opened_) throw std::logic_error("store " + file + " was read before it was opened");
	return text_.value_or(std::string());
}

bool StoreState::opened() const {
	const std::scoped_lock lock(mutex_);
	return opened_;
}

std::expected<void, std::string> StoreState::open() {
	calling = "StoreAdapter::read";
	auto read = adapter.read(file, rule.maxBytes);
	if (!read) return std::unexpected(std::move(read.error()));
	std::optional<std::string> found = std::move(*read);
	if (found) {
		// Every open, and before the text is judged: a file refused for its text may still hold a secret, and a mount
		// (an fsGroup) may widen it each time (docs/writing-a-node-type.md#stores).
		calling = "StoreAdapter::narrow";
		if (auto narrowed = adapter.narrow(file, rule.mode); !narrowed)
			return std::unexpected(std::move(narrowed.error()));
		if (rule.distrust != nullptr) {
			auto why = caughtFrom([&] { return rule.distrust(*found); });
			if (!why) return std::unexpected(file + ": distrust threw: " + why.error());
			if (*why) return std::unexpected(file + ": " + **why);
		}
	} else if (rule.initial != nullptr) {
		// Only a missing file is created: never a new identity over one that is distrusted or unreadable.
		auto initial = caughtFrom([&] { return rule.initial(); });
		if (!initial) return std::unexpected(file + ": initial threw: " + initial.error());
		if (initial->size() > rule.maxBytes)
			return std::unexpected(file + ": its initial text is larger than " + std::to_string(rule.maxBytes) +
			                       " bytes");
		calling = "StoreAdapter::replaceIf";
		auto created = adapter.replaceIf(file, std::nullopt, *initial, rule.maxBytes, rule.mode);
		if (!created) return std::unexpected(std::move(created.error()));
		if (*created != StoreWrite::replaced) return std::unexpected(file + ": appeared while being created");
		found = std::move(*initial);
	}
	calling = "StoreAdapter";
	const std::scoped_lock lock(mutex_);
	text_ = std::move(found);
	opened_ = true;
	return {};
}

std::expected<void, std::string> StoreState::write(std::string text) {
	if (text.size() > rule.maxBytes)
		return std::unexpected(file + ": larger than " + std::to_string(rule.maxBytes) + " bytes");
	std::optional<std::string> expected;
	{
		// Only this thread writes text_, so what is read here is still the file's last known text below.
		const std::scoped_lock lock(mutex_);
		expected = text_;
	}
	if (expected) {
		// Narrowed first: a file it replaces keeps its mode, so a mode widened mid-run must not outlive this write.
		calling = "StoreAdapter::narrow";
		if (auto narrowed = adapter.narrow(file, rule.mode); !narrowed) {
			calling = "StoreAdapter::read";
			auto there = adapter.read(file, rule.maxBytes);
			calling = "StoreAdapter";
			if (there && !*there) return std::unexpected(file + ": changed by something else");
			return std::unexpected(std::move(narrowed.error()));
		}
	}
	calling = "StoreAdapter::replaceIf";
	auto replaced = adapter.replaceIf(file, expected, text, rule.maxBytes, rule.mode);
	calling = "StoreAdapter";
	if (!replaced) return std::unexpected(std::move(replaced.error()));
	// Someone changed or removed it: theirs stands.
	if (*replaced != StoreWrite::replaced) return std::unexpected(file + ": changed by something else");
	const std::scoped_lock lock(mutex_);
	text_ = std::move(text);
	return {};
}

} // namespace detail
} // namespace ear
