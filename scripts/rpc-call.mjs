#!/usr/bin/env node
// scripts/rpc-call.mjs <ws-url> [--until '<node> <state>'] [--timeout <s>] <method> [params-json]
//
// One JSON-RPC 2.0 call to ear's control port (docs/design/batch2.md section 6; from protocol 1,
// docs/design/control-plane.md 2.2 and 4), for scripts: connects to <ws-url> (for example
// ws://127.0.0.1:7777/rpc), sends <method> with the named params given as JSON, prints the response's
// result as one line of JSON and exits 0. An error response is printed to standard error and exits 1.
// Node.js 24's own WebSocket; no packages.
//
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc node.remove '{"name":"noise"}'
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc --until 'porch running' node.start '{"name":"porch"}'
//
// --until '<node> <state>' waits, after the reply, for the node to reach the state: it subscribes to the
// graph before the request, prints the reply, then exits 0 on the first node.state of that node with
// that state received after the reply (a session's replies and notifications share one ordered queue,
// so nothing older arrives after it), 1 on a node.state of that node that says failed (or an error
// reply), and 2 once --timeout seconds (default 60) pass, printing the last node.state seen. A reply
// whose applied is unchanged or retune, for a node already in that state, causes no node.state, so it
// exits 0 at once.
//
// Without --until it waits for the response, the socket's close or an error; the deadline below only
// fails a call that never ends.

const args = process.argv.slice(2);
const options = { until: undefined, timeoutS: 60 };
const positional = [];
for (let i = 0; i < args.length; ++i) {
	if (args[i] === '--until') options.until = args[++i];
	else if (args[i] === '--timeout') options.timeoutS = Number(args[++i]);
	else positional.push(args[i]);
}
const [url, method, params] = positional;
const usage = "usage: rpc-call.mjs <ws-url> [--until '<node> <state>'] [--timeout <s>] <method> [params-json]";
let untilNode;
let untilState;
if (options.until !== undefined) {
	[untilNode, untilState] = options.until.trim().split(/\s+/);
	if (!untilNode || !untilState) {
		console.error(usage);
		process.exit(2);
	}
}
if (!url || !method || !(options.timeoutS > 0)) {
	console.error(usage);
	process.exit(2);
}

const fail = (message, code = 1) => {
	console.error(`rpc-call: ${message}`);
	process.exit(code);
};

const request = { jsonrpc: '2.0', id: 2, method };
if (params !== undefined) request.params = JSON.parse(params);
const socket = new WebSocket(url);
socket.addEventListener('error', () => fail(`cannot talk to ${url}`));

if (untilNode === undefined) {
	const deadlineMs = 10_000;
	setTimeout(() => fail(`no answer from ${url} within ${deadlineMs} ms`), deadlineMs).unref();
	socket.addEventListener('open', () => socket.send(JSON.stringify(request)));
	socket.addEventListener('message', (event) => {
		const message = JSON.parse(event.data);
		if (message.id !== request.id) return; // a notification
		if (message.error) {
			console.error(JSON.stringify(message.error));
			process.exitCode = 1;
		} else {
			console.log(JSON.stringify(message.result));
			process.exitCode = 0;
		}
		socket.close();
	});
	socket.addEventListener('close', (event) => {
		if (process.exitCode === undefined) fail(`${url} closed with ${event.code} before answering`);
	});
} else {
	let state; // the node's state as last seen: the snapshot's, then each node.state's
	let lastHealth; // the last node.state of the node, for the timeout's message
	let replied = false;
	const finish = (code) => {
		process.exitCode = code;
		socket.close();
	};
	setTimeout(() => {
		console.error(`rpc-call: ${untilNode} did not reach ${untilState} within ${options.timeoutS} s`);
		if (lastHealth !== undefined) console.error(JSON.stringify(lastHealth));
		process.exit(2);
	}, options.timeoutS * 1000).unref();
	socket.addEventListener('open', () =>
		socket.send(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'graph.subscribe' })),
	);
	socket.addEventListener('message', (event) => {
		const message = JSON.parse(event.data);
		if (message.id === 1) {
			if (message.error) {
				console.error(JSON.stringify(message.error));
				return finish(1);
			}
			state = message.result.nodes?.find((node) => node.name === untilNode)?.state;
			socket.send(JSON.stringify(request)); // after the snapshot, so no node.state is missed
			return;
		}
		if (message.id === request.id) {
			replied = true;
			if (message.error) {
				console.error(JSON.stringify(message.error));
				return finish(1);
			}
			console.log(JSON.stringify(message.result));
			const applied = message.result?.applied;
			if ((applied === 'unchanged' || applied === 'retune') && state === untilState) finish(0);
			return;
		}
		if (message.method !== 'node.state' || message.params?.name !== untilNode) return;
		state = message.params.state;
		lastHealth = message.params;
		if (!replied) return;
		if (state === untilState) finish(0);
		else if (state === 'failed') {
			console.error(JSON.stringify(message.params));
			finish(1);
		}
	});
	socket.addEventListener('close', (event) => {
		if (process.exitCode === undefined) fail(`${url} closed with ${event.code} before ${untilNode} was ${untilState}`);
	});
}
