#!/usr/bin/env node
// scripts/rpc-call.mjs <ws-url> <method> [params-json]
//
// One JSON-RPC 2.0 call to ear's control port (docs/design/batch2.md section 6), for scripts:
// connects to <ws-url> (for example ws://127.0.0.1:7777/rpc), sends <method> with the named
// params given as JSON, prints the response's result as one line of JSON and exits 0. An error
// response is printed to standard error and exits 1. Node.js 24's own WebSocket; no packages.
//
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get
//   node scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc node.remove '{"name":"noise"}'
//
// It waits for the response, the socket's close or an error; the deadline below only fails a
// call that never ends.

const [url, method, params] = process.argv.slice(2);
if (!url || !method) {
	console.error('usage: rpc-call.mjs <ws-url> <method> [params-json]');
	process.exit(2);
}

const deadlineMs = 10_000;
const fail = (message) => {
	console.error(`rpc-call: ${message}`);
	process.exit(1);
};
setTimeout(() => fail(`no answer from ${url} within ${deadlineMs} ms`), deadlineMs).unref();

const request = { jsonrpc: '2.0', id: 1, method };
if (params !== undefined) request.params = JSON.parse(params);

const socket = new WebSocket(url);
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
socket.addEventListener('error', () => fail(`cannot talk to ${url}`));
socket.addEventListener('close', (event) => {
	if (process.exitCode === undefined) fail(`${url} closed with ${event.code} before answering`);
});
