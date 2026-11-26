// A type's own tests run it through this (docs/writing-a-node-type.md#testing-a-type): the NodeHarness of the type
// makeNodeType<Spec, Type>() makes, over a real NodeHost, with banks named by the spec's handles, so a test names banks
// as the type does and a bank the spec lacks does not compile.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "ear/core/lifecycle.hpp"
#include "ear/core/report.hpp"
#include "ear/testing/node_harness.hpp"
namespace ear::testing {

template <const auto& Spec, class Type> class TypeHarness {
	template <class Bank> static constexpr bool ownBank = isBankHandle<Bank> && BanksOf<Spec>::template has<Bank>;
	template <class Handle> static constexpr bool ownStore =
		isStoreHandle<Handle> && StoresOf<Spec>::template has<Handle>;

public:
	using Options = HarnessOptions;
	explicit TypeHarness(Options options = {})
		: type_(makeNodeType<Spec, Type>()), node_(std::make_unique<NodeHarness>(*type_, std::move(options))) {}
	const NodeType& type() const noexcept { return *type_; }
	NodeHarness& node() noexcept { return *node_; }
	NodeHarness* operator->() noexcept { return node_.get(); }

	std::optional<std::string> parse(const NodeSettings& settings) { return node_->parse(settings); }
	void add(AddOptions options = {}) { node_->add(options); }
	void blockReady() { node_->blockReady(); }
	void process(std::uint32_t frames = 0) { node_->process(frames); }
	NodeOutcome change(const NodeSettings& settings) { return node_->change(settings); }
	/// Live row `Row` of the spec now `value` (docs/writing-a-node-type.md#its-own-tests): its slot moved as a gesture
	/// tick moves it, NodeHost::setLive at once, then settled. A value outside the row's range is a std::logic_error,
	/// so a test cannot move a slot where no setting could. A row that is not live, or a family, does not compile.
	template <const auto& Row> void live(typename std::remove_cvref_t<decltype(Row)>::Value value) {
		static_assert(SettingsOf<Spec>::template listed<Row>, "live takes only its spec's rows");
		static_assert(!setting::isIndexed<std::remove_cvref_t<decltype(Row)>>, "a family is moved by member");
		static_assert(setting::isLive(Row.row()), "live moves a live row");
		node_->live(Row.row().key, bitsOf<Row>(value));
	}
	/// Member `member` (from 0) of live family `Family` now `value`, as live.
	template <const auto& Family>
	void live(std::size_t member, typename std::remove_cvref_t<decltype(Family)>::Value value) {
		static_assert(SettingsOf<Spec>::template listed<Family>, "live takes only its spec's rows");
		static_assert(setting::isIndexed<std::remove_cvref_t<decltype(Family)>>, "only a family is moved by member");
		static_assert(setting::isLive(Family.row()), "live moves a live row");
		node_->live(std::string(Family.row().key) + "." + std::to_string(member + 1), bitsOf<Family>(value));
	}
	LifecycleReply stop() { return node_->stop(); }
	LifecycleReply start() { return node_->start(); }
	LifecycleReply restart() { return node_->restart(); }
	void disable() { node_->disable(); }
	void enable() { node_->enable(); }
	void remove() { node_->remove(); }
	/// Trigger `Trigger` of the spec (docs/writing-a-node-type.md#actions): answered at once.
	template <const auto& Trigger> ActionReply trigger() {
		static_assert(ActionsOf<Spec>::template has<Trigger>, "trigger takes only its spec's actions");
		static_assert(!std::remove_cvref_t<decltype(Trigger)>::isCall, "trigger takes a trigger; a call is invoke's");
		return node_->invoke(Trigger.declaration().name).reply();
	}
	/// Call `Call` of the spec with `args`, its handler run on the harness's loops (runLoops): its answer, which is
	/// empty while the node's loops hold it (the call queued behind work the test has not run).
	template <const auto& Call> ActionAnswer invoke(const NodeSettings& args = nullptr) {
		static_assert(ActionsOf<Spec>::template has<Call>, "invoke takes only its spec's actions");
		static_assert(std::remove_cvref_t<decltype(Call)>::isCall, "invoke takes a call; a trigger is trigger's");
		ActionAnswer answer = node_->invoke(Call.declaration().name, args);
		node_->runLoops();
		return answer;
	}
	std::string_view phase() const { return node_->phase(); }
	NodeHealth health() const { return node_->health(); }
	bool gone() const noexcept { return node_->gone(); }
	std::uint64_t droppedFrames() const { return node_->droppedFrames(); }
	std::vector<CompanionView> companionList() const { return node_->companionList(); }
	std::size_t unwritten() const { return node_->unwritten(); }
	const std::vector<CompanionFake>& companions() const noexcept { return node_->companions(); }
	const std::vector<RecordingRequest>& recordingRequests() const noexcept { return node_->recordingRequests(); }
	/// The recordings on the harness's in-memory disk, by path (NodeHarness's recording calls).
	std::optional<std::string> recordingBytes(std::string_view path) const { return node_->recordingBytes(path); }
	void presetRecording(std::string_view path, std::string bytes) { node_->presetRecording(path, std::move(bytes)); }
	void failRecordingOpen(std::string_view path, std::string why) { node_->failRecordingOpen(path, std::move(why)); }
	void pipeRecording(std::string_view path) { node_->pipeRecording(path); }
	std::size_t openRecordings() const { return node_->openRecordings(); }

	const std::vector<ProgramFake>& programs() const noexcept { return node_->programs(); }
	void programStarted(std::size_t index = 0) { node_->programStarted(index); }
	void programReady(std::size_t index = 0) { node_->programReady(index); }
	void programRefused(std::size_t index, std::string why) { node_->programRefused(index, std::move(why)); }
	void feedProgram(std::size_t index, std::string bytes) { node_->feedProgram(index, std::move(bytes)); }
	void programSays(std::size_t index, LogLevel level, std::string line) {
		node_->programSays(index, level, std::move(line));
	}
	void programExited(std::size_t index, ProgramExit exit) { node_->programExited(index, std::move(exit)); }

	/// Field `Field`'s value in the status view after the last drain.
	template <const auto& Field> StatusValue status() const {
		static_assert(StatusOf<Spec>::template has<Field>, "status takes only its spec's fields");
		for (const FieldValue& field : node_->status())
			if (field.name == Field.declaration().name) return field.value;
		return Field.declaration().initial(); // never configured yet
	}
	std::vector<FieldValue> status() const { return node_->status(); }
	const std::vector<SeenStatus>& statusRecord() const noexcept { return node_->statusRecord(); }
	const std::vector<SeenEvent>& events() const noexcept { return node_->events(); }
	void drain() { node_->drain(); }
	std::uint64_t realtimeRings() const noexcept { return node_->realtimeRings(); }

	void runLoops() { node_->runLoops(); }
	void loopsReady() { node_->loopsReady(); }
	void readable(int fd) { node_->readable(fd); }
	/// The sources armed on loop `Handle` whose handles live.
	template <class Handle> std::size_t armed(Handle) const {
		static_assert(isLoopHandle<Handle> && LoopsOf<Spec>::template has<Handle>,
		              "the harness takes only its spec's loops");
		return node_->armed(Handle::name);
	}
	/// The descriptors loop `Handle`'s live readable watches watch (NodeHarness::watchedDescriptors).
	template <class Handle> std::vector<int> watchedDescriptors(Handle) const {
		static_assert(isLoopHandle<Handle> && LoopsOf<Spec>::template has<Handle>,
		              "the harness takes only its spec's loops");
		return node_->watchedDescriptors(Handle::name);
	}

	void runIo() { node_->runIo(); }
	/// Store `Handle`'s file on the harness's in-memory disk (NodeHarness's store calls, by the spec's handle).
	template <class Handle> void presetStore(Handle, std::string text, std::uint32_t mode = 0600) {
		static_assert(ownStore<Handle>, "the harness takes only its spec's stores");
		node_->presetStore(Handle::suffix, std::move(text), mode);
	}
	template <class Handle> std::optional<std::string> storeText(Handle) const {
		static_assert(ownStore<Handle>, "the harness takes only its spec's stores");
		return node_->storeText(Handle::suffix);
	}
	template <class Handle> std::uint32_t storeMode(Handle) const {
		static_assert(ownStore<Handle>, "the harness takes only its spec's stores");
		return node_->storeMode(Handle::suffix);
	}
	template <class Handle> std::string storeFile(Handle) const {
		static_assert(ownStore<Handle>, "the harness takes only its spec's stores");
		return node_->storeFile(Handle::suffix);
	}
	template <class Handle> void failStoreOpen(Handle, std::string why) {
		static_assert(ownStore<Handle>, "the harness takes only its spec's stores");
		node_->failStoreOpen(Handle::suffix, std::move(why));
	}
	template <class Handle> void failNextWrite(Handle, std::string why) {
		static_assert(ownStore<Handle>, "the harness takes only its spec's stores");
		node_->failNextWrite(Handle::suffix, std::move(why));
	}

	template <class Bank> void ready(Bank) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		node_->ready(Bank::name);
	}
	template <class Bank> void lose(Bank, std::string detail) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		node_->lose(Bank::name, std::move(detail));
	}
	template <class Bank> std::span<float> input(Bank, std::size_t member) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::input, "input() takes an input bank");
		return node_->input(node_->ports().bank(Bank::name).ports.at(member));
	}
	template <class Bank> std::span<const float> output(Bank, std::size_t member) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "output() takes an output bank");
		return node_->output(node_->ports().bank(Bank::name).ports.at(member));
	}
	template <class Bank> CompanionFake& companion(Bank) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		return node_->companion(Bank::name);
	}
	/// A source's bank (an inside input bank): what its writer takes, what it played, its shortfall.
	template <class Bank> std::uint32_t feed(Bank, std::span<const float> interleaved) {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::reach == PortBank::Reach::inside && Bank::direction == Direction::input,
		              "a source plays into an inside input bank");
		return node_->feed(Bank::name, interleaved);
	}
	/// What the last process() did with the source's ring: exactly what its clock was told (SourcePlayed).
	template <class Bank> SourcePlayed lastPlayed(Bank) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		return node_->lastPlayed(Bank::name);
	}
	template <class Bank> bool paused(Bank) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		return node_->paused(Bank::name);
	}
	template <class Bank> std::uint64_t pausedFrames(Bank) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		return node_->pausedFrames(Bank::name);
	}
	template <class Bank> std::uint64_t shortfall(Bank) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		return node_->shortfall(Bank::name);
	}
	template <class Bank> float lastFadeGain(Bank, std::size_t member = 0) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "a fade is on an output bank");
		return node_->lastFadeGain(node_->ports().bank(Bank::name).ports.at(member));
	}
	/// Whether a member of output bank `Bank` is held, and its hold's gain now (NodeHarness::held, lastHoldGain).
	template <class Bank> bool held(Bank, std::size_t member = 0) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "a hold is on an output bank");
		return node_->held(node_->ports().bank(Bank::name).ports.at(member));
	}
	template <class Bank> float lastHoldGain(Bank, std::size_t member = 0) const {
		static_assert(ownBank<Bank>, "the harness takes only its spec's banks");
		static_assert(Bank::direction == Direction::output, "a hold is on an output bank");
		return node_->lastHoldGain(node_->ports().bank(Bank::name).ports.at(member));
	}
	void acknowledgeHolds() { node_->acknowledgeHolds(); }
	std::size_t swaps() const noexcept { return node_->swaps(); }

private:
	template <const auto& Row> static std::uint64_t bitsOf(typename std::remove_cvref_t<decltype(Row)>::Value value) {
		using Value = typename std::remove_cvref_t<decltype(Row)>::Value;
		const SettingRow& row = Row.row();
		const auto outside = [&] {
			throw std::logic_error("live(): " + std::string(row.key) + " cannot hold that value");
		};
		if constexpr (std::same_as<Value, double>) {
			const auto& range = std::get<DecimalRule>(row.rule);
			if (!(value >= range.low && value <= range.high)) outside();
		} else if constexpr (std::same_as<Value, std::uint32_t>) {
			const auto& range = std::get<WholeNumberRule>(row.rule);
			if (value < range.low || value > range.high) outside();
		} else if constexpr (std::is_enum_v<Value>) {
			const auto& names = std::get<ChoiceRule>(row.rule);
			for (std::uint32_t at = 0; at < names.count; ++at)
				if (names.values[at] == static_cast<std::int64_t>(std::to_underlying(value))) return at;
			outside();
		}
		return setting::liveBits(value);
	}
	std::unique_ptr<const NodeType> type_;
	std::unique_ptr<NodeHarness> node_; // after the type it runs
};

} // namespace ear::testing
