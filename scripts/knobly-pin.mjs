#!/usr/bin/env node
// scripts/knobly-pin.mjs --check | <version>
//
// knobly's one pin, cmake/knobly.json (AGENTS.md#build-run-and-test): its version; the URL templates of the C++
// release asset, the npm tarball and the release's SHA256SUMS ({version} replaced by the version: knobly's release
// naming, whose one home is the pin); the C++ asset's SHA-256; and the npm tarball's SHA-512 integrity. CMakeLists.txt
// reads it; npm cannot, since it wants the tarball's URL written in web/package.json and its integrity in
// web/package-lock.json. This script keeps those two in step with the pin. Node.js 24, no packages.
//
//   node scripts/knobly-pin.mjs --check   fails, naming each mismatch, when web/package.json's knobly URL, or
//                                         web/package-lock.json's resolved URL, version or integrity, is not the pin's
//                                         (make web-test runs it; configure checks the same, CMakeLists.txt)
//   node scripts/knobly-pin.mjs 1.1.0     moves everything to knobly 1.1.0, all or nothing: downloads both assets and
//                                         the release's SHA256SUMS and fails unless both assets' SHA-256s are listed
//                                         there; runs npm install of the tarball in web/ (which rewrites package.json
//                                         and the lock); only then writes the pin; then --check. Any failure puts the
//                                         pin, package.json and the lock back as they were.
//
// Run it in the image (scripts/dev.sh node scripts/knobly-pin.mjs <version>), whose Node and npm are web/'s.
//
// Exit codes: 0 in step (or moved), 1 a mismatch or a failed step (printed to standard error), 2 bad arguments.

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const pinPath = join(root, "cmake", "knobly.json");
const packagePath = join(root, "web", "package.json");
const lockPath = join(root, "web", "package-lock.json");

const readJson = (path) => JSON.parse(readFileSync(path, "utf8"));
const urlOf = (pin, key, version = pin.version) => pin[key].replaceAll("{version}", version);

function check() {
	const pin = readJson(pinPath);
	const url = urlOf(pin, "jsUrl");
	const pkg = readJson(packagePath);
	const lock = readJson(lockPath);
	const entry = lock.packages?.["node_modules/knobly"] ?? {};
	const found = [
		["web/package.json dependencies.knobly", pkg.dependencies?.knobly, url],
		['web/package-lock.json packages."".dependencies.knobly', lock.packages?.[""]?.dependencies?.knobly, url],
		['web/package-lock.json packages."node_modules/knobly".resolved', entry.resolved, url],
		['web/package-lock.json packages."node_modules/knobly".version', entry.version, pin.version],
		['web/package-lock.json packages."node_modules/knobly".integrity', entry.integrity, pin.jsIntegrity],
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

// SHA256SUMS is `sha256sum` output: "<hex>  <file name>" per line.
function listedSha256(sums, sumsUrl, name) {
	for (const line of sums.toString("utf8").split("\n")) {
		const match = /^([0-9a-f]{64}) [ *](.+)$/.exec(line.trim());
		if (match && match[2] === name) {
			return match[1];
		}
	}
	throw new Error(`${sumsUrl} lists no ${name}`);
}

async function move(version) {
	const pin = readJson(pinPath);
	const cppUrl = urlOf(pin, "cppUrl", version);
	const jsUrl = urlOf(pin, "jsUrl", version);
	const sumsUrl = urlOf(pin, "sumsUrl", version);
	const [cpp, js, sums] = [await download(cppUrl), await download(jsUrl), await download(sumsUrl)];
	const cppSha256 = createHash("sha256").update(cpp).digest("hex");
	for (const [url, sha256] of [
		[cppUrl, cppSha256],
		[jsUrl, createHash("sha256").update(js).digest("hex")],
	]) {
		const listed = listedSha256(sums, sumsUrl, basename(new URL(url).pathname));
		if (listed !== sha256) {
			throw new Error(`${url} has SHA-256 ${sha256}, but ${sumsUrl} lists ${listed}`);
		}
	}
	console.log(`knobly-pin: both assets match ${sumsUrl}`);

	const saved = [pinPath, packagePath, lockPath].map((path) => [path, readFileSync(path)]);
	try {
		// npm writes the URL into package.json and the tarball's integrity and dependencies into the lock; --check then
		// proves npm's integrity is the one computed here from the same file.
		execFileSync("npm", ["install", "--save-exact", "--no-audit", "--no-fund", jsUrl], {
			cwd: join(root, "web"),
			stdio: "inherit",
		});
		pin.version = version;
		pin.cppSha256 = cppSha256;
		pin.jsIntegrity = `sha512-${createHash("sha512").update(js).digest("base64")}`;
		writeFileSync(pinPath, `${JSON.stringify(pin, null, "\t")}\n`);
		if (check() !== 0) {
			throw new Error("npm's lock disagrees with the pin");
		}
	} catch (error) {
		for (const [path, bytes] of saved) {
			writeFileSync(path, bytes);
		}
		throw new Error(`${error.message}; cmake/knobly.json, web/package.json and web/package-lock.json are as they were`);
	}
	console.log(`knobly-pin: knobly ${version}: ${cppUrl} sha256 ${pin.cppSha256}`);
	return 0;
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
