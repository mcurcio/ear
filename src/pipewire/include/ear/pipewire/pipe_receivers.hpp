#pragma once
#include "ear/core/companions.hpp"
#include "ear/pipewire/module_converters.hpp"
namespace ear::pipewire {
/// Pipe receivers, the second module-run converter (docs/engine.md#pipe-receivers): one receiver = PipeWire's
/// libpipewire-module-pipe-tunnel loaded into the engine's context in source mode with the pipe receiver's and pipe
/// stream's rows of the property table as its arguments (docs/engine.md#properties-ear-sets), every one built in
/// pipe_receivers.cpp (the one home of that form) from the shape and the target: `pipe.filename` is
/// CompanionTarget::fifo, which the module makes (mkfifo) at load and removes when it is destroyed; `tunnel.may-pause`
/// false, so a writer that stops leaves the stream running silent. Its node (Audio/Source, a pw_stream on the module's
/// own internal connection) never drives, and runs alone on the graph's driver until joined. Its ports:
/// "capture_AUX<i>". The module keeps the ring (pipeReceiverBufferFrames ahead, resynced past three times that), the
/// tracking of the writer's pace and the resampler.
/// Lifecycle: ModuleConverters'. RUNNING once its stream runs (always-process), which a FIFO the module could make and
/// open always reaches; a path it cannot make fails the load, told later as every module's is.
using PipeReceivers = ModuleConverters<PipeReceiverShape>;
extern template class ModuleConverters<PipeReceiverShape>;
} // namespace ear::pipewire
