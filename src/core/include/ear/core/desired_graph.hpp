// Frozen interface (docs/design/m1-white-to-file.md section 2). Change it only through an
// erratum in that page's Errata section and a re-cut, never a local edit.
#pragma once
#include <expected>
#include <memory>
#include "ear/core/node_catalogue.hpp"
namespace ear {
class DesiredNodeListener {
public:
  /// After the node is recorded; `config` lives as long as the node is wanted.
  virtual void nodeAdded(const NodeName& node, const NodeConfig& config) = 0;
protected:
  ~DesiredNodeListener() = default;
};
class DesiredRouteListener {
public:
  virtual void routeAdded(const Route& route) = 0;
protected:
  ~DesiredRouteListener() = default;
};
/// The control calls. M1: these two; the control port adds the rest later.
class GraphControl {
public:
  virtual ~GraphControl() = default;
  /// Same spec again: succeeds, no event. Errors: UnknownNodeType, InvalidSettings, DuplicateNode.
  virtual std::expected<void, CommandError> addNode(const NodeSpec& node) = 0;
  /// Existing route: succeeds, no event. Errors: UnknownNode, UnknownPort, WrongDirection.
  virtual std::expected<void, CommandError> addRoute(const Route& route) = 0;
};
class DesiredGraph final : public GraphControl {
public:
  explicit DesiredGraph(const NodeCatalogue& catalogue);   // catalogue outlives this
  ~DesiredGraph() override;
  /// Told in subscription order after the change is recorded; subscribe before the first call.
  void subscribe(DesiredNodeListener& listener);
  void subscribe(DesiredRouteListener& listener);
  std::expected<void, CommandError> addNode(const NodeSpec& node) override;
  std::expected<void, CommandError> addRoute(const Route& route) override;
private:
  struct Impl; std::unique_ptr<Impl> impl_;
};
}
