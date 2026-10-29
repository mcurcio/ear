// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in .claude/errata.md and a re-cut, never a local edit.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "ear/core/desired_graph.hpp"
namespace ear {
using LinkId = std::uint64_t;          // issued by the Reconciler, unique per process
using EnginePortId = std::uint32_t;    // the engine's global id of a port
struct LivePort { PortRef port; Direction direction; EnginePortId id; };
class LivePortListener {
public:
  virtual void portAppeared(const LivePort& port) = 0;
  virtual void portGone(EnginePortId id) = 0;
protected:
  ~LivePortListener() = default;
};
class LinkListener {
public:
  virtual void linkActive(LinkId link) = 0;                      // audio flows
  virtual void linkFailed(LinkId link, std::string detail) = 0;  // refused; no link exists
  virtual void linkGone(LinkId link) = 0;                        // existed, now gone
protected:
  ~LinkListener() = default;
};
class LinkMaker {
public:
  /// Main thread. Exactly one of linkActive/linkFailed follows later, never inside this call.
  virtual void createLink(LinkId link, EnginePortId output, EnginePortId input) = 0;
  /// Main thread. Removes it if present; no further events for `link`.
  virtual void destroyLink(LinkId link) = 0;
protected:
  ~LinkMaker() = default;
};
enum class RouteMatch { pending, linked, refused };   // pending: an end is not live yet
std::string_view matchName(RouteMatch match) noexcept;
struct RouteStatus { Route route; RouteMatch match; std::string detail; };   // detail when refused
class RouteStatusListener {
public:
  virtual void routeChanged(const RouteStatus& status) = 0;
protected:
  ~RouteStatusListener() = default;
};
/// Invariants: at most one link per wanted route; links only for wanted routes; a route is
/// pending until both ends are live, then linked on linkActive, refused on linkFailed; an end
/// going (or linkGone) makes it pending and it is linked again when both ends are live.
class Reconciler final : public DesiredRouteListener, public LivePortListener, public LinkListener {
public:
  explicit Reconciler(LinkMaker& links);
  ~Reconciler();
  void subscribe(RouteStatusListener& listener);
  void routeAdded(const Route& route) override;
  void portAppeared(const LivePort& port) override;
  void portGone(EnginePortId id) override;
  void linkActive(LinkId link) override;
  void linkFailed(LinkId link, std::string detail) override;
  void linkGone(LinkId link) override;
  std::vector<RouteStatus> status() const;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
