#!/usr/bin/env node
// scripts/knobly-pin.mjs --check | <version>
//
// knobly's one pin, cmake/knobly.json (AGENTS.md#build-run-and-test): its version, the SHA-256 of the C++
// release asset and the SHA-512 integrity of the npm tarball. CMakeLists.txt reads it; npm cannot, since it wants the
// tarball's URL written in web/package.json and its integrity in web/package-lock.json. This script keeps those two
// in step with the pin. Node.js 24, no packages.
//
//   node scripts/knobly-pin.mjs --check   fails, naming each mismatch, when web/package.json's knobly URL, or
//                                         web/package-lock.json's resolved URL, version or integrity, is not the pin's
//                                         (make web-test runs it; configure checks the same, CMakeLists.txt)
//   node scripts/knobly-pin.mjs 1.1.0     moves everything to knobly 1.1.0: downloads both release assets, writes
//                                         their hashes and the version into the pin, runs npm install of the tarball
//                                         in web/ (which rewrites package.json and the lock), then --check
//
// The URLs come from the version by knobly's release naming (below; CMakeLists.txt derives the same two). Run it in
// the image (scripts/dev.sh node scripts/knobly-pin.mjs <version>), whose Node and npm are web/'s.
//
// Exit codes: 0 in step (or moved), 1 a mismatch or a failed step (printed to standard error), 2 bad arguments.

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const pinPath = join(root, "cmake", "knobly.json");
const packagePath = join(root, "web", "package.json");
const lockPath = join(root, "web", "package-lock.json");

// knobly's release naming: one GitHub Release per version, tag v<version>, with these assets.
const releaseBase = (version) => `https://github.com/mcurcio/knobly/releases/download/v${version}`;
const cppUrl = (version) => `${releaseBase(version)}/knobly-cpp-${version}.tar.gz`;
const jsUrl = (version) => `${releaseBase(version)}/knobly-${version}.tgz`;

const readJson = (path) => JSON.parse(readFileSync(path, "utf8"));

function check() {
	const pin = readJson(pinPath);
	const url = jsUrl(pin.version);
	const pkg = readJson(packagePath);
	const lock = readJson(lockPath);
	const entry = lock.packages?.["node_modules/knobly"] ?? {};
	const found = [
		["web/package.json dependencies.knobly", pkg.dependencies?.knobly, url],
		['web/package-lock.json packages[""].dependencies.knobly', lock.packages?.[""]?.dependencies?.knobly, url],
		['web/package-lock.json packages["node_modules/knobly"].resolved', entry.resolved, url],
		['web/package-lock.json packages["node_modules/knobly"].version', entry.version, pin.version],
		['web/package-lock.json packages["node_modules/knobly"].integrity', entry.integrity, pin.jsIntegrity],
	];
	const mismatches = found.filter(([, actual, wanted]) => actual !== wanted);
	for (const [where, actual, wanted] of mismatches) {
		console.error(`knobly-pin: ${where} is ${JSON.stringify(actual)}, but cmake/knobly.json pins ${JSON.stringify(wanted)}`);
	}
	if (mismatches.length > 0) {
		console.error(`knobly-pin: web/ is not on knobly ${pin.version}; move it with: node scripts/knobly-pin.mjs ${pin.version}`);
		return 1;
	}
	console.log(`knobly-pin: web/ and cmake/knobly.json agree on knobly ${pin.version}`);
	return 0;
}

async function download(url) {
	const response = await fetch(url);
	if (!response.ok) {
		throw new Error(`${url}: HTTP ${response.status}`);
	}
	return Buffer.from(await response.arrayBuffer());
}

async function move(version) {
	const cpp = await download(cppUrl(version));
	const js = await download(jsUrl(version));
	const pin = readJson(pinPath);
	pin.version = version;
	pin.cppSha256 = createHash("sha256").update(cpp).digest("hex");
	pin.jsIntegrity = `sha512-${createHash("sha512").update(js).digest("base64")}`;
	writeFileSync(pinPath, `${JSON.stringify(pin, null, "\t")}\n`);
	console.log(`knobly-pin: cmake/knobly.json is knobly ${version}: ${cppUrl(version)} sha256 ${pin.cppSha256}`);
	// npm writes the URL into package.json and the tarball's integrity and dependencies into the lock; --check then
	// proves npm's integrity is the one computed here from the same URL.
	execFileSync("npm", ["install", "--save-exact", "--no-audit", "--no-fund", jsUrl(version)], {
		cwd: join(root, "web"),
		stdio: "inherit",
	});
	return check();
}

const args = process.argv.slice(2);
if (args.length !== 1) {
	console.error("usage: knobly-pin.mjs --check | <version>");
	process.exit(2);
}
if (args[0] === "--check") {
	process.exit(check());
}
if (!/^\d+\.\d+\.\d+$/.test(args[0])) {
	console.error(`knobly-pin: not a version: ${args[0]}`);
	console.error("usage: knobly-pin.mjs --check | <version>");
	process.exit(2);
}
try {
	process.exit(await move(args[0]));
} catch (error) {
	console.error(`knobly-pin: ${error.message}`);
	process.exit(1);
}
