// The graph editor (docs/design/batch2.md section 8): a React Flow canvas of ear's graph. Nodes
// and wires are exactly what ear announced; dragging a wire between ports, the panels and the
// delete key send requests, and the canvas changes when ear's events arrive. Node positions
// and selection are the browser's own.
import {
  Background,
  Controls,
  ReactFlow,
  applyNodeChanges,
  type Connection,
  type EdgeChange,
  type NodeChange,
  type OnBeforeDelete,
} from "@xyflow/react";
import { useCallback, useEffect, useMemo, useState } from "react";
import { edgeId, handleId, isValidConnection, routeFromConnection } from "./connect";
import type { GraphState } from "./graph";
import { placeNodes, type Position } from "./layout";
import { RpcError } from "./rpc";
import { EarNode, type EarFlowNode } from "./components/EarNode";
import { useConnection, useEditor, useGraph } from "./components/hooks";
import { LevelEdge, type LevelFlowEdge } from "./components/LevelEdge";
import { AddNodePanel, EngineFacts, NodePanel } from "./components/Panels";

const nodeTypes = { ear: EarNode };
const edgeTypes = { level: LevelEdge };

/** The canvas's nodes for a new graph: kept nodes keep their place, new ones are laid out. */
function syncNodes(previous: EarFlowNode[], graph: GraphState): EarFlowNode[] {
  if (graph === null) return [];
  const kept = new Map(previous.map((node) => [node.id, node]));
  const placed = new Map<string, Position>(previous.map((node) => [node.id, node.position]));
  const positions = placeNodes(graph, placed);
  return graph.nodes.map((node) => {
    const old = kept.get(node.name);
    if (old !== undefined) return { ...old, data: { node } };
    return { id: node.name, type: "ear", position: positions.get(node.name)!, data: { node } };
  });
}

function describe(error: unknown): string {
  if (error instanceof RpcError) return error.data !== undefined ? `${error.data.code}: ${error.message}` : error.message;
  return error instanceof Error ? error.message : String(error);
}

export function App() {
  const { client } = useEditor();
  const graph = useGraph();
  const connection = useConnection();
  const [nodes, setNodes] = useState<EarFlowNode[]>([]);
  const [selectedEdges, setSelectedEdges] = useState<ReadonlySet<string>>(new Set());
  const [error, setError] = useState<string | null>(null);
  const showError = useCallback((caught: unknown) => setError(describe(caught)), []);

  useEffect(() => setNodes((previous) => syncNodes(previous, graph)), [graph]);

  const edges = useMemo<LevelFlowEdge[]>(
    () =>
      (graph?.routes ?? []).map((route) => {
        const id = edgeId(route);
        return {
          id,
          type: "level",
          source: route.from.node,
          sourceHandle: handleId("out", route.from.port),
          target: route.to.node,
          targetHandle: handleId("in", route.to.port),
          selected: selectedEdges.has(id),
          data: { route },
        };
      }),
    [graph, selectedEdges],
  );

  // Moves and selection are the browser's; adding and removing wait for ear's events.
  const onNodesChange = useCallback(
    (changes: NodeChange<EarFlowNode>[]) =>
      setNodes((current) => applyNodeChanges(changes.filter((c) => c.type !== "remove" && c.type !== "add"), current)),
    [],
  );
  const onEdgesChange = useCallback((changes: EdgeChange<LevelFlowEdge>[]) => {
    setSelectedEdges((current) => {
      const next = new Set(current);
      for (const change of changes) {
        if (change.type !== "select") continue;
        if (change.selected) next.add(change.id);
        else next.delete(change.id);
      }
      return next;
    });
  }, []);

  const onConnect = useCallback(
    (connectionMade: Connection) => {
      const route = routeFromConnection(connectionMade);
      if (route !== null) client.call("route.add", route).catch(showError);
    },
    [client, showError],
  );

  // The delete key asks ear to remove the selection and deletes nothing itself.
  const onBeforeDelete = useCallback<OnBeforeDelete<EarFlowNode, LevelFlowEdge>>(
    async ({ nodes: doomedNodes, edges: doomedEdges }) => {
      const names = new Set(doomedNodes.map((node) => node.id));
      for (const name of names) client.call("node.remove", { name }).catch(showError);
      for (const edge of doomedEdges) {
        const route = edge.data!.route;
        // Removing a node removes its routes; only wires between kept nodes are asked for.
        if (names.has(route.from.node) || names.has(route.to.node)) continue;
        client.call("route.remove", { from: route.from, to: route.to }).catch(showError);
      }
      return false;
    },
    [client, showError],
  );

  const selectedNode = nodes.find((node) => node.selected)?.data.node;

  return (
    <div className="app">
      <ReactFlow<EarFlowNode, LevelFlowEdge>
        nodes={nodes}
        edges={edges}
        nodeTypes={nodeTypes}
        edgeTypes={edgeTypes}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onConnect={onConnect}
        isValidConnection={isValidConnection}
        onBeforeDelete={onBeforeDelete}
        deleteKeyCode={["Delete", "Backspace"]}
        fitView
      >
        <Background />
        <Controls />
      </ReactFlow>
      <aside className="panel">
        <h1>EAR</h1>
        {connection !== "open" && (
          <div className="banner banner-connection">
            {connection === "connecting" ? "Connecting to ear…" : "Lost the connection to ear; retrying."}
          </div>
        )}
        {error !== null && (
          <div className="banner banner-error" role="alert">
            {error}{" "}
            <button type="button" onClick={() => setError(null)}>
              Dismiss
            </button>
          </div>
        )}
        {graph !== null && <EngineFacts graph={graph} />}
        {graph !== null && <AddNodePanel types={graph.types} onError={showError} />}
        {selectedNode !== undefined && <NodePanel node={selectedNode} onError={showError} />}
      </aside>
    </div>
  );
}
