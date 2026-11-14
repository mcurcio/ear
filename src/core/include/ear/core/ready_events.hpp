// How a resource the framework asked for tells it became ready, or never will (docs/engine.md#readiness-deadlines): a
// loop's start (docs/writing-a-node-type.md#loops), and later a store's open and a claim's grant. NodeHost implements it
// for each readiness item of the kind; an adapter calls it. Pure: nothing but the standard library.
#pragma once
#include <string>
namespace ear {
/// Main thread only, at most one of the two, never inside the adapter call that was handed it (an adapter posts it to
/// the main loop). Valid until the resource it was handed for is destroyed.
class ReadyEvents {
public:
	/// The resource is ready.
	virtual void ready() = 0;
	/// It never will be: the node fails start-failed, its detail the item's with `detail` (a loop's "cannot start loop
	/// <name>: <detail>").
	virtual void failed(std::string detail) = 0;

protected:
	~ReadyEvents() = default;
};
} // namespace ear
