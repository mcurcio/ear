#!/usr/bin/env node
// scripts/rpc-call.mjs <ws-url> [--until '<node> <state>'] [--timeout <s>] <method> [params-json]
//
// One JSON-RPC 2.0 call to ear's control port (docs/control-port.md; from protocol 1,
// docs/control-port.md#talking-to-it), for scripts: connects to <ws-url> (for example
// ws://127.0.0.1:7777/rpc), sends <method> with the named params given as JSON and prints the response's
// result as one line of JSON. Node.js 24's own WebSocket; no packages.
//
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc node.remove '{"name":"noise"}'
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc --until 'porch running' node.start '{"name":"porch"}'
//
// --until '<node> <state>' waits, after the reply, for the node to reach the state: it subscribes to the
// graph before the request, prints the reply, then exits on the first node.state of that node with that
// state received after the reply (a session's replies and notifications share one ordered queue, so
// nothing older arrives after it). A reply whose applied is unchanged or retune, for a node already in
// that state, causes no node.state, so it exits at once.
//
// --timeout <s> bounds the whole call: 60 s by default with --until, 10 s without.
//
// Exit codes:
//   0  the result came (and, with --until, the node reached the state)
//   1  an error response (printed to standard error), a node.state that says failed while waiting with
//      --until, or a socket that failed or closed first
//   2  --until's state did not come within --timeout (the last node.state of the node is printed)
//   3  bad arguments (the usage is printed)
//   4  no answer within --timeout, without --until

const usage = "usage: rpc-call.mjs <ws-url> [--until '<node> <state>'] [--timeout <s>] <method> [params-json]";
const badArguments = (why) => {
	console.error(`rpc-call: ${why}`);
	console.error(usage);
	process.exit(3);
};

const args = process.argv.slice(2);
let until;
let timeoutS;
const positional = [];
for (let i = 0; i < args.length; ++i) {
	if (args[i] === '--until' || args[i] === '--timeout') {
		const value = args[i + 1];
		if (value === undefined || value.startsWith('--')) badArguments(`${args[i]} needs a value`);
		if (args[i] === '--until') until = value;
		else timeoutS = Number(value);
		++i;
	} else positional.push(args[i]);
}
const [url, method, params, ...extra] = positional;
if (!url || !method) badArguments('a URL and a method are needed');
if (extra.length > 0) badArguments(`unexpected ${extra.join(' ')}`);
if (timeoutS !== undefined && !(timeoutS > 0)) badArguments('--timeout must be a number of seconds above 0');
let untilNode;
let untilState;
if (until !== undefined) {
	[untilNode, untilState] = until.trim().split(/\s+/);
	if (!untilNode || !untilState || until.trim().split(/\s+/).length !== 2)
		badArguments("--until takes '<node> <state>'");
}
const request = { jsonrpc: '2.0', id: 2, method };
if (params !== undefined) {
	try {
		request.params = JSON.parse(params);
	} catch (error) {
		badArguments(`params are not JSON: ${error.message}`);
	}
}

// The first outcome decides the exit code; nothing after it changes it.
let done = false;
const finish = (code) => {
	if (done) return;
	done = true;
	process.exitCode = code;
	socket.close();
};
const fail = (message, code = 1) => {
	if (done) return;
	console.error(`rpc-call: ${message}`);
	finish(code);
};

const socket = new WebSocket(url);
socket.addEventListener('error', () => fail(`cannot talk to ${url}`));

if (untilNode === undefined) {
	const deadlineS = timeoutS ?? 10;
	setTimeout(() => {
		fail(`no answer from ${url} within ${deadlineS} s`, 4);
		process.exit(4);
	}, deadlineS * 1000).unref();
	socket.addEventListener('open', () => socket.send(JSON.stringify(request)));
	socket.addEventListener('message', (event) => {
		if (done) return;
		const message = JSON.parse(event.data);
		if (message.id !== request.id) return; // a notification
		if (message.error) {
			console.error(JSON.stringify(message.error));
			finish(1);
		} else {
			console.log(JSON.stringify(message.result));
			finish(0);
		}
	});
	socket.addEventListener('close', (event) => fail(`${url} closed with ${event.code} before answering`));
} else {
	const deadlineS = timeoutS ?? 60;
	let state; // the node's state as last seen: the snapshot's, then each node.state's
	let lastHealth; // the last node.state of the node, for the timeout's message
	let replied = false;
	setTimeout(() => {
		if (done) return;
		console.error(`rpc-call: ${untilNode} did not reach ${untilState} within ${deadlineS} s`);
		if (lastHealth !== undefined) console.error(JSON.stringify(lastHealth));
		process.exit(2);
	}, deadlineS * 1000).unref();
	socket.addEventListener('open', () =>
		socket.send(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'graph.subscribe' })),
	);
	socket.addEventListener('message', (event) => {
		if (done) return;
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
	socket.addEventListener('close', (event) =>
		fail(`${url} closed with ${event.code} before ${untilNode} was ${untilState}`),
	);
}
