## [2.0.0](https://github.com/mcurcio/ear/compare/v1.0.0...v2.0.0) (2026-11-06)

### ⚠ BREAKING CHANGES

* **nodes:** every route in ear.conf changes. white: MONO is noise.mono.
file: FL, FR are record.left, record.right. card: playback_N is play.N and
capture_N is capture.N. rtp: channel_N is stream.N. sendspin: from_server_FL,
from_server_FR are from.left, from.right and to_server_FL, to_server_FR are
to.left, to.right. An old name is refused as unknown-port.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>

### Features

* **nodes:** a port is <bank>.<member>, declared in named banks ([dca7be3](https://github.com/mcurcio/ear/commit/dca7be31b3bd385f0dd85d468b4fa898d2dcad11))
* **sendspin:** listen and advertise over mDNS, per node ([611ac9e](https://github.com/mcurcio/ear/commit/611ac9e15501c568a3cce49320e1b7d84877d85a))
* **sendspin:** play Music Assistant's audio through a sendspin node ([bb27b57](https://github.com/mcurcio/ear/commit/bb27b577960ec5fb8f58619649bca79839dd2aef))
* **sendspin:** send routed audio to Music Assistant, paired by a code ([1083128](https://github.com/mcurcio/ear/commit/1083128e71866b027405e5f3691dd609afd1fd48))

### Bug Fixes

* **core:** a name added back waits until its old node has gone ([a2cd221](https://github.com/mcurcio/ear/commit/a2cd221a177ebfe37d0a02a66885d4fd15f6eb0a))
* **sendspin:** answer mDNS as RFC 6762 says, and reflect nothing ([25fd286](https://github.com/mcurcio/ear/commit/25fd286984b482d4b51ee83874844bacf9993693))
* **sendspin:** configure asks for the whole carried patch, and a flood's cost is stated ([f84a3e4](https://github.com/mcurcio/ear/commit/f84a3e41bfbcd95196d906b1726b1c71290f4397))
* **sendspin:** hold one format, report what still plays, and date audio at the device ([7ec1f88](https://github.com/mcurcio/ear/commit/7ec1f884965d88f2b596082e39bca71ce4b6bb37))
* **sendspin:** keep every key file at 0600, not only a live node's ([2b2511e](https://github.com/mcurcio/ear/commit/2b2511ee3e2916a7657070cd652bacd246cb71d3))
* **sendspin:** limit what a peer that proved nothing can cost the transport ([eaf717c](https://github.com/mcurcio/ear/commit/eaf717c29926a57de9976614b1e81f1446765089))
* **sendspin:** sockets that prove nothing give way, so holding them keeps no server out ([016bd10](https://github.com/mcurcio/ear/commit/016bd100f8e6b3a81b0552e7e500d5876b2cb014))
* **test:** take the last `;` out of a test name ([18edd06](https://github.com/mcurcio/ear/commit/18edd06e85b7050099a174ef12fcf471c35ce8a5))

## [1.0.0](https://github.com/mcurcio/ear/compare/v0.0.0...v1.0.0) (2026-11-01)

### ⚠ BREAKING CHANGES

* --config and ear.json are gone; ear reads ear.conf
(knobly) from --config-dir. A route to an undeclared port at boot no
longer exits 1.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>

### Features

* add milestone 1 packages with their frozen interfaces (slice 0) ([70d30b3](https://github.com/mcurcio/ear/commit/70d30b331eab7f60063f1e4ab1355fc80051c8df))
* batch 2 interfaces as compiling stubs (slice s0) ([859459b](https://github.com/mcurcio/ear/commit/859459b2c8d38f5f8c5d0ce6233624146e713324))
* **config:** edits at the source; graphFile wire vocabulary and status lines ([8e4bd41](https://github.com/mcurcio/ear/commit/8e4bd41224e22440e7317df63fd79695a11a1bd8))
* **config:** graph file keeper on one background thread ([2f729bf](https://github.com/mcurcio/ear/commit/2f729bf79d879ee9f29eba7ea373c22115b3707e))
* **config:** knobly in the build; graph file model and desired graph plans ([512d27d](https://github.com/mcurcio/ear/commit/512d27d953b5d22e59befc6127b3a2dd9cd8e733))
* **control:** boot ear from the graph file, print status lines, and add the owner's white-to-file system tests (slice S6) ([15c329a](https://github.com/mcurcio/ear/commit/15c329a665224cc112c93e33452102903ac708bb))
* **control:** graph JSON, JSON-RPC envelope, commands, events, main-loop gateway and sessions (slice R1) ([68fe260](https://github.com/mcurcio/ear/commit/68fe260163c99821d80ffb24fb60865e82eb6c91))
* **core:** desired graph, node catalogue, node lifecycle and node host (slice S2) ([20cddf6](https://github.com/mcurcio/ear/commit/20cddf62d94d356d8a7468347485c6dbc4f99684))
* **core:** graph edits, observers first, close(), block-first node framework with restart, removal and sound cards, graph read and card ids (slice C1) ([9883c77](https://github.com/mcurcio/ear/commit/9883c77c64be1212a818d64136e87d50d17fbc94))
* **core:** reconcile route links from live ports and link events (slice S3) ([f74d005](https://github.com/mcurcio/ear/commit/f74d005dd6923a0964aeb37b2f5a29f533352e3e))
* ear runs from a knobly graph file kept live by the control port ([251a5cd](https://github.com/mcurcio/ear/commit/251a5cd5186fa84744a5f0267b5781df9ca7fd20))
* **folder:** confined folder shared by recordings and the graph file ([cc15cb0](https://github.com/mcurcio/ear/commit/cc15cb0ccd86d4356e04ff018a09af439824f474))
* **join:** main.cpp wiring and flags, system tests through out/ear, image smoke; stop waits for every close handshake; the level bell rings once per window (slice J) ([39e44fb](https://github.com/mcurcio/ear/commit/39e44fb951e730a6e5e6e275f7e4d51873e16a2d))
* **nodes:** add the white and file node types (slice S5) ([a7cd986](https://github.com/mcurcio/ear/commit/a7cd9861e6ddf15579cef454aadd289d41b759cb))
* **nodes:** rtp node receives network audio (tracer) ([92900ee](https://github.com/mcurcio/ear/commit/92900ee2ae6cfbb9869186f12411d6fee37bdb10))
* **nodes:** the card node type and builtinNodeTypes white, file and card (slice E2b) ([fc864f3](https://github.com/mcurcio/ear/commit/fc864f33c79d508de99ece220f97356975c069ba))
* **pipewire:** ALSA sound cards with their own links, loss teardown and the test ALSA card (slice E2a) ([43b3260](https://github.com/mcurcio/ear/commit/43b32600e5ff2189d543f1bd186f81c1f767ccd3))
* **pipewire:** drive barrier, graph group, clock-aligned levels, main-loop executor and scheduling report (slice E1) ([3d76e9b](https://github.com/mcurcio/ear/commit/3d76e9bb81aecb605f6533686027f2a076e809fd))
* **pipewire:** hand realtime frames to an ordinary thread through a ring (slice S4) ([31b5009](https://github.com/mcurcio/ear/commit/31b500941d38ccfe1f22067e0f4d5b65d9cf01c5))
* **pipewire:** run PipeWire in-process with port blocks, live ports and links (slice S1) ([61e37fa](https://github.com/mcurcio/ear/commit/61e37fac67e870526988bf88b5c44b73cf86e7f0))
* **server:** control thread, port 7777 with files, Host and Origin checks, /rpc, bounded queues, level bell and stop (slice R2) ([d60b2cf](https://github.com/mcurcio/ear/commit/d60b2cfc20580323ef6cfc275053cbd0e28a6d37))
* **skills:** add JUCE API reference generator and JUCE skills ([3405873](https://github.com/mcurcio/ear/commit/3405873d8fd5ddcfcdabf05fb22373399c955dde))
* **web:** ear.conf text pane with diagnostics and save ([0fea379](https://github.com/mcurcio/ear/commit/0fea37991806b9f60459c5fb2bd9f87d9918c248))
* **web:** graph editor with live levels, reconnecting rpc client and fixture-tested reducer (slice W1) ([7a25a55](https://github.com/mcurcio/ear/commit/7a25a551fd2ed4e2ecf1fa520aaa30cfd5e04ef4))

### Bug Fixes

* **config:** refuse unwritable and oversized edits; run every test ([bafbb7e](https://github.com/mcurcio/ear/commit/bafbb7e5a8dbf7db61c5e58bbd6b86f2dffffa89))
* **nodes:** file paths stay inside the recordings folder ([5350308](https://github.com/mcurcio/ear/commit/53503080a4d9967a07c12924c5d4163b4f8feb13))
* **rtp:** end review: probes that bite, a failed sync fails the receiver, shared outside-node plumbing ([155702b](https://github.com/mcurcio/ear/commit/155702b2e92b20ba8560736b027aaa9ab7f45be5))
* **server:** a full descriptor table no longer spins the control port; steady J(5) and E2(3) tests; block-first lifecycle in AGENTS.md; review errata ([889ccc5](https://github.com/mcurcio/ear/commit/889ccc50e57de70178fb3b93708a1cb7010bb696))
