// Port blocks as pw_filter nodes (docs/design/m1-white-to-file.md section 2, port_blocks.hpp).
#include "ear/pipewire/port_blocks.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

namespace ear::pipewire {
namespace {

class FilterPortBlock final : public PortBlock {
public:
  FilterPortBlock(Engine& engine, const NodeName& node, const Ports& ports,
                  const AudioFormat& format, AudioCode& code, PortBlockEvents& events)
      : code_(code), events_(events), loop_(engine.mainLoop()), maxBlock_(format.maxBlock),
        zeros_(format.maxBlock, 0.0f), inputs_(ports.inputs.size()),
        outputs_(ports.outputs.size()) {
    for (std::size_t i = 0; i < ports.outputs.size(); ++i) {
      scratch_.emplace_back(format.maxBlock, 0.0f);
    }
    refusal_ = pw_loop_add_event(loop_, &FilterPortBlock::onRefusal, this);
    // pw_filter_new's name becomes media.name; node.name (how the registry, and so the live
    // graph, names the node) defaults to the program's name unless set here.
    filter_ = pw_filter_new(engine.core(), node.c_str(),
                            pw_properties_new(PW_KEY_NODE_NAME, node.c_str(),
                                              PW_KEY_MEDIA_TYPE, "Audio",
                                              PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
    if (filter_ == nullptr) {
      fail(std::string("cannot make the engine node: ") + spa_strerror(-errno));
      return;
    }
    pw_filter_add_listener(filter_, &listener_, &filterEvents(), this);
    addPorts(PW_DIRECTION_INPUT, ports.inputs, inputPorts_);
    addPorts(PW_DIRECTION_OUTPUT, ports.outputs, outputPorts_);
    if (const int res = pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
        res < 0) {
      fail(std::string("cannot connect the engine node: ") + spa_strerror(res));
    }
  }

  // pw_filter_destroy removes the node from the data loop and clears the process callback
  // there (a blocking invoke), so process() is never called after it returns.
  ~FilterPortBlock() override {
    if (filter_ != nullptr) pw_filter_destroy(filter_);
    if (refusal_ != nullptr) pw_loop_destroy_source(loop_, refusal_);
  }

  FilterPortBlock(const FilterPortBlock&) = delete;
  FilterPortBlock& operator=(const FilterPortBlock&) = delete;

private:
  static const pw_filter_events& filterEvents() {
    static const pw_filter_events table = [] {
      pw_filter_events events{};
      events.version = PW_VERSION_FILTER_EVENTS;
      events.state_changed = &FilterPortBlock::onStateChanged;
      events.process = &FilterPortBlock::onProcess;
      return events;
    }();
    return table;
  }

  void addPorts(pw_direction direction, const std::vector<PortName>& names,
                std::vector<void*>& handles) {
    // The handle is the port's (empty) user data, freed with the filter; a refused port (null)
    // leaves its span on zeros or scratch.
    for (const PortName& name : names) {
      handles.push_back(pw_filter_add_port(
          filter_, direction, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
                            name.c_str(), nullptr),
          nullptr, 0));
    }
  }

  /// A refusal while making the block is told on the main loop, never inside make().
  void fail(std::string detail) {
    refusalDetail_ = std::move(detail);
    if (refusal_ != nullptr) pw_loop_signal_event(loop_, refusal_);
  }

  static void onRefusal(void* data, std::uint64_t /*count*/) {
    auto* self = static_cast<FilterPortBlock*>(data);
    if (self->lostTold_) return;
    self->lostTold_ = true;
    self->events_.lost(self->refusalDetail_);
  }

  static void onStateChanged(void* data, pw_filter_state /*old*/, pw_filter_state state,
                             const char* error) {
    auto* self = static_cast<FilterPortBlock*>(data);
    if (state == PW_FILTER_STATE_STREAMING && !self->readyTold_) {
      self->readyTold_ = true;
      self->events_.ready();
    } else if (state == PW_FILTER_STATE_ERROR && !self->lostTold_) {
      self->lostTold_ = true;
      self->events_.lost(error != nullptr ? error : "engine error");
    }
  }

  /// Realtime thread.
  static void onProcess(void* data, spa_io_position* position) {
    auto* self = static_cast<FilterPortBlock*>(data);
    const auto frames = static_cast<std::uint32_t>(position->clock.duration);
    if (frames > self->maxBlock_) {
      for (void* port : self->outputPorts_) {
        if (port == nullptr) continue;
        if (auto* out = static_cast<float*>(pw_filter_get_dsp_buffer(port, frames))) {
          std::fill_n(out, frames, 0.0f);
        }
      }
      return;
    }
    for (std::size_t i = 0; i < self->inputPorts_.size(); ++i) {
      const auto* in = self->inputPorts_[i] == nullptr
                           ? nullptr
                           : static_cast<const float*>(
                                 pw_filter_get_dsp_buffer(self->inputPorts_[i], frames));
      self->inputs_[i] = std::span<const float>(in != nullptr ? in : self->zeros_.data(), frames);
    }
    for (std::size_t i = 0; i < self->outputPorts_.size(); ++i) {
      auto* out = self->outputPorts_[i] == nullptr
                      ? nullptr
                      : static_cast<float*>(pw_filter_get_dsp_buffer(self->outputPorts_[i], frames));
      self->outputs_[i] = std::span<float>(out != nullptr ? out : self->scratch_[i].data(), frames);
    }
    self->code_.process(self->inputs_, self->outputs_, frames);
  }

  AudioCode& code_;
  PortBlockEvents& events_;
  pw_loop* loop_;
  std::uint32_t maxBlock_;
  std::vector<float> zeros_;                  // an unlinked input reads these
  std::vector<std::vector<float>> scratch_;   // an output with no buffer this cycle writes here
  std::vector<std::span<const float>> inputs_;
  std::vector<std::span<float>> outputs_;
  std::vector<void*> inputPorts_;
  std::vector<void*> outputPorts_;
  pw_filter* filter_ = nullptr;
  spa_hook listener_{};
  bool readyTold_ = false;
  bool lostTold_ = false;
  spa_source* refusal_ = nullptr;
  std::string refusalDetail_;
};

}  // namespace

FilterPortBlocks::FilterPortBlocks(Engine& engine) : engine_(engine) {}

std::unique_ptr<PortBlock> FilterPortBlocks::make(const NodeName& node, const Ports& ports,
                                                  const AudioFormat& format, AudioCode& code,
                                                  PortBlockEvents& events) {
  return std::make_unique<FilterPortBlock>(engine_, node, ports, format, code, events);
}

}  // namespace ear::pipewire
