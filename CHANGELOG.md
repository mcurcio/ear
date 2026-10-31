# Changelog

## [0.1.0](https://github.com/mcurcio/ear/compare/v0.0.1...v0.1.0) (2026-10-31)


### Features

* add milestone 1 packages with their frozen interfaces (slice 0) ([70d30b3](https://github.com/mcurcio/ear/commit/70d30b331eab7f60063f1e4ab1355fc80051c8df))
* batch 2 interfaces as compiling stubs (slice s0) ([859459b](https://github.com/mcurcio/ear/commit/859459b2c8d38f5f8c5d0ce6233624146e713324))
* batch 2, run image, control port, graph UI and sound card ([6f36d74](https://github.com/mcurcio/ear/commit/6f36d746026e5d68b06af66c0fe2d8fa75d323fd))
* **control:** boot ear from the graph file, print status lines, and add the owner's white-to-file system tests (slice S6) ([15c329a](https://github.com/mcurcio/ear/commit/15c329a665224cc112c93e33452102903ac708bb))
* **control:** graph JSON, JSON-RPC envelope, commands, events, main-loop gateway and sessions (slice R1) ([68fe260](https://github.com/mcurcio/ear/commit/68fe260163c99821d80ffb24fb60865e82eb6c91))
* **core:** desired graph, node catalogue, node lifecycle and node host (slice S2) ([20cddf6](https://github.com/mcurcio/ear/commit/20cddf62d94d356d8a7468347485c6dbc4f99684))
* **core:** graph edits, observers first, close(), block-first node framework with restart, removal and sound cards, graph read and card ids (slice C1) ([9883c77](https://github.com/mcurcio/ear/commit/9883c77c64be1212a818d64136e87d50d17fbc94))
* **core:** reconcile route links from live ports and link events (slice S3) ([f74d005](https://github.com/mcurcio/ear/commit/f74d005dd6923a0964aeb37b2f5a29f533352e3e))
* **join:** main.cpp wiring and flags, system tests through out/ear, image smoke; stop waits for every close handshake; the level bell rings once per window (slice J) ([39e44fb](https://github.com/mcurcio/ear/commit/39e44fb951e730a6e5e6e275f7e4d51873e16a2d))
* milestone 1, white noise to a file sink through PipeWire ([32006df](https://github.com/mcurcio/ear/commit/32006df4769506a305a06179678ae6a57bbc095d))
* **nodes:** add the white and file node types (slice S5) ([a7cd986](https://github.com/mcurcio/ear/commit/a7cd9861e6ddf15579cef454aadd289d41b759cb))
* **nodes:** the card node type and builtinNodeTypes white, file and card (slice E2b) ([fc864f3](https://github.com/mcurcio/ear/commit/fc864f33c79d508de99ece220f97356975c069ba))
* **pipewire:** ALSA sound cards with their own links, loss teardown and the test ALSA card (slice E2a) ([43b3260](https://github.com/mcurcio/ear/commit/43b32600e5ff2189d543f1bd186f81c1f767ccd3))
* **pipewire:** drive barrier, graph group, clock-aligned levels, main-loop executor and scheduling report (slice E1) ([3d76e9b](https://github.com/mcurcio/ear/commit/3d76e9bb81aecb605f6533686027f2a076e809fd))
* **pipewire:** hand realtime frames to an ordinary thread through a ring (slice S4) ([31b5009](https://github.com/mcurcio/ear/commit/31b500941d38ccfe1f22067e0f4d5b65d9cf01c5))
* **pipewire:** run PipeWire in-process with port blocks, live ports and links (slice S1) ([61e37fa](https://github.com/mcurcio/ear/commit/61e37fac67e870526988bf88b5c44b73cf86e7f0))
* **server:** control thread, port 7777 with files, Host and Origin checks, /rpc, bounded queues, level bell and stop (slice R2) ([d60b2cf](https://github.com/mcurcio/ear/commit/d60b2cfc20580323ef6cfc275053cbd0e28a6d37))
* **skills:** add JUCE API reference generator and JUCE skills ([3405873](https://github.com/mcurcio/ear/commit/3405873d8fd5ddcfcdabf05fb22373399c955dde))
* **web:** graph editor with live levels, reconnecting rpc client and fixture-tested reducer (slice W1) ([7a25a55](https://github.com/mcurcio/ear/commit/7a25a551fd2ed4e2ecf1fa520aaa30cfd5e04ef4))


### Bug Fixes

* **nodes:** file paths stay inside the recordings folder ([5350308](https://github.com/mcurcio/ear/commit/53503080a4d9967a07c12924c5d4163b4f8feb13))
* **server:** a full descriptor table no longer spins the control port; steady J(5) and E2(3) tests; block-first lifecycle in AGENTS.md; review errata ([889ccc5](https://github.com/mcurcio/ear/commit/889ccc50e57de70178fb3b93708a1cb7010bb696))
