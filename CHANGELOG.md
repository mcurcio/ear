## [4.2.0](https://github.com/mcurcio/ear/compare/v4.1.1...v4.2.0) (2026-11-25)

### Features

* **web:** colour tokens and a dark theme ([6521653](https://github.com/mcurcio/ear/commit/65216530086e95015839f9f484294ec71a57a695))
* **web:** command palette, shortcuts, canvas adding and kept places ([f4f6636](https://github.com/mcurcio/ear/commit/f4f6636c25c57adfdc3c26c059f1b68e43e8ebda))
* **web:** fit phones and touch screens, a foldable's two screens included ([b3a7f1c](https://github.com/mcurcio/ear/commit/b3a7f1cf98677fed80879bc6c64f18bfd71d69e5))
* **web:** in-flight feedback, toasts and the ear.conf gutter ([53160d6](https://github.com/mcurcio/ear/commit/53160d61633430d578c0b1895f351c394f24f9a2))
* **web:** restyle node boxes, wires and meters ([98de119](https://github.com/mcurcio/ear/commit/98de11953cf1912002adac2cdbeb3095156b6bb1))
* **web:** top bar, inspector and ear.conf drawer ([69b0541](https://github.com/mcurcio/ear/commit/69b054111442c95c0413d0cdda0ebe7b308b5f63))

### Bug Fixes

* **web:** fit the view once laid out, a segmented lifecycle bar, the palette's long hints and the phone-width top bar ([a983322](https://github.com/mcurcio/ear/commit/a9833225cb4eb923ae72a1cf99a0b08a246cf7c8))

## [4.1.1](https://github.com/mcurcio/ear/compare/v4.1.0...v4.1.1) (2026-11-22)

### Bug Fixes

* **core:** a close that completed is never judged overdue ([e35a683](https://github.com/mcurcio/ear/commit/e35a6836516c884f9d47ade8af923144e10ddbd9))
* **core:** a configuration freed with its node is freed in teardown's order ([c9adf16](https://github.com/mcurcio/ear/commit/c9adf16d9a499a08dfe71e9ef4d03e149bb8e853))
* **core:** ear.close-overdue no longer counts a loop that never started as open ([d99d143](https://github.com/mcurcio/ear/commit/d99d143b4e35c8eab967d55a387766eb80eafab3))
* **core:** ear.close-overdue says its close was under way when nothing is left open (review) ([623ccc9](https://github.com/mcurcio/ear/commit/623ccc927fab7b8b33dddd80854c727e489f1898))
* **sendspin:** what is sent to the server is dated by the capture latency upstream (slice 3c) ([b0c4ddd](https://github.com/mcurcio/ear/commit/b0c4dddb4ebf44c2fe48485385107862e1565f0e))

## [4.1.0](https://github.com/mcurcio/ear/compare/v4.0.0...v4.1.0) (2026-11-15)

### Features

* **airplay:** an airplay node type on AirPlay 1, with shairport-sync in the images (slice 9) ([da29b42](https://github.com/mcurcio/ear/commit/da29b429770dab7c7d25d2a90aa25f657b97a97f))
* **airplay:** the airplay2 setting, nqptp run by the operator, a listing per service, nqptp in the dev image for tests (slice 9) ([7f86408](https://github.com/mcurcio/ear/commit/7f864089df56780cd623d17752673433cef8fd96))
* **core:** pipe receivers, programs and any-port claims in the node framework (slice 9) ([ed9093e](https://github.com/mcurcio/ear/commit/ed9093e5a75c785b2dbce951bdfd633422180840))
* **core:** programs that need shared memory, ready once it exists and ended when it goes, and boolean setting rows (slice 9) ([a7f3145](https://github.com/mcurcio/ear/commit/a7f3145f8e5a4bc27b9092430a95bcfcab671c29))
* **mdns:** listings probed and renamed on a clash, two airplay nodes of one name told apart (slice 9) ([2290f42](https://github.com/mcurcio/ear/commit/2290f421d7606ae0b0416f53138761e2f950da24))
* **runtime:** programs run by ear and pipe receivers on PipeWire's pipe module (slice 9) ([ee971ce](https://github.com/mcurcio/ear/commit/ee971ce171675bd4f98a80847db8151dc97225c4))

### Bug Fixes

* **airplay:** ear's own runtime folder every boot, a program's whole group ended, host labels that are DNS labels (slice 9 review) ([57aa06a](https://github.com/mcurcio/ear/commit/57aa06a34c723e4cc65e45753f9d5547abfa1120))

## [4.0.0](https://github.com/mcurcio/ear/compare/v3.0.0...v4.0.0) (2026-11-15)

### ⚠ BREAKING CHANGES

* **config:** ear.conf's top-level graph_frequency and block_size are
refused (ear/unknown-directive) with what to write instead, from the file's
own values: "graph_frequency is no longer a directive: graph settings moved:
write graph { rate 44.1khz; block_size 512; }". A bare-number rate, the graph
rate's or an rtp node's, is refused (ear/bad-rate: "48000 has no unit: write
48000hz or 48khz"). --graph-frequency is refused, saying to write --rate
with a unit. No compatibility is kept: rewrite ear.conf with a graph { }
block and units on every rate.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MtEdSSMnWMDCszQdw5TZut

### Features

* **config:** rates are written with units, in a graph { } block ([39c0f45](https://github.com/mcurcio/ear/commit/39c0f45c99dc07471138acc80541978d07306891))
* **control:** a node's own knobly text, read and replaced (node-text) ([3e9a2c6](https://github.com/mcurcio/ear/commit/3e9a2c657e7c641707d9e9fa05c3688103884f5b))
* **control:** ear.sink-dropped and ear.source-shortfall on standard output ([371c4e5](https://github.com/mcurcio/ear/commit/371c4e5f6a944808a13615fef68914450f5a24c5))
* **control:** node.addText appends a new node's knobly statement (node-text) ([7e852f1](https://github.com/mcurcio/ear/commit/7e852f1059d07595991249e2891df316e44b90fc))
* **control:** the companions feature: each node's companion list on the wire and node.companionsChanged ([4230a3c](https://github.com/mcurcio/ear/commit/4230a3cd591654321796b1e60f7714e6ed5f3914))
* **control:** the counters feature and counters.changed, and the two counter records (cycle facts D2) ([ced7c08](https://github.com/mcurcio/ear/commit/ced7c08d18cd39e1797872434fb9f51a4110fb1b))
* **core:** a node's companion list, told by NodeHost and its counters paced by CounterWatch ([84026c3](https://github.com/mcurcio/ear/commit/84026c375de3cc74be3a68d5c7979f79c711583f))
* **core:** claims, so a second rtp node on one UDP port is refused held by node <first> (slice 3c) ([7b5a1ff](https://github.com/mcurcio/ear/commit/7b5a1ff6331045b93c57413f15d7d66abfb4ad87)), closes [writing-a-node-type.md#claims](https://github.com/mcurcio/writing-a-node-type.md/issues/claims) [engine.md#rtp-receivers](https://github.com/mcurcio/engine.md/issues/rtp-receivers) [#readiness-deadlines](https://github.com/mcurcio/ear/issues/readiness-deadlines) [node-types.md#rtp](https://github.com/mcurcio/node-types.md/issues/rtp)
* **core:** cycle counters per graph and per node (cycle facts D1) ([b24bfbf](https://github.com/mcurcio/ear/commit/b24bfbf87498d23db3d9bbba5049a95d918dd2db))
* **core:** cycle facts and the one rule that states them (cycle facts C1) ([6874285](https://github.com/mcurcio/ear/commit/68742852bebdb09f63822a02d6617c7e515dd850))
* **core:** GraphCycle::ranRawNs, when the block really began a cycle ([910279b](https://github.com/mcurcio/ear/commit/910279bb02c162b36b69aa8e866418c1888d48e3))
* **core:** one latency model, series and parallel kept apart (root classes R1) ([8d11cd7](https://github.com/mcurcio/ear/commit/8d11cd7aab7fffed97d00dee03b4f6d4f0d28a5a))
* **core:** readiness deadlines for companions and the quantum guard (slice 3b.4) ([55601e2](https://github.com/mcurcio/ear/commit/55601e274837d5415f7669223cfa04613089973d))
* **core:** recordings, so a second file node on one path is refused held by node <first> (slice 3c) ([5a3396d](https://github.com/mcurcio/ear/commit/5a3396d67c9abbd7259a16a898a3c4e61d4afb20)), closes [writing-a-node-type.md#recordings](https://github.com/mcurcio/writing-a-node-type.md/issues/recordings) [#claims](https://github.com/mcurcio/ear/issues/claims) [#file](https://github.com/mcurcio/ear/issues/file) [engine.md#readiness-deadlines](https://github.com/mcurcio/engine.md/issues/readiness-deadlines)
* **core:** SampleRate and FrameClock(SampleRate) (cycle facts C6a) ([0d16b07](https://github.com/mcurcio/ear/commit/0d16b07e06ef2e08754c87caf3b65f2871f7d687))
* **core:** sinks stamp their upstream latency and count their drops (slice 3b.1) ([51410af](https://github.com/mcurcio/ear/commit/51410af26b88316af6df6127740fbafc2bc261c2)), closes [docs/engine.md#sinks](https://github.com/docs/engine.md/issues/sinks) [docs/engine.md#latency-per-bank-pair](https://github.com/docs/engine.md/issues/latency-per-bank-pair) [docs/writing-a-node-type.md#sinks](https://github.com/docs/writing-a-node-type.md/issues/sinks)
* **core:** sources keep in step with the clock on cycle facts, and sinks' chunks carry them (cycle facts C4b) ([75a64c1](https://github.com/mcurcio/ear/commit/75a64c1f7c34e814134719abd905be5a272aef26))
* **core:** sources, played by their block before its code (slice 3b.2) ([cbe6cf7](https://github.com/mcurcio/ear/commit/cbe6cf7f261ea49fbdb9395d17155853a01556d3))
* faults split by owner, an adapter's bug reported as ear.internal-error (root classes R4b) ([49a3047](https://github.com/mcurcio/ear/commit/49a3047fbe38315b659b3130594003e218ccc607)), closes [docs/architecture.md#faults-by-owner](https://github.com/docs/architecture.md/issues/faults-by-owner)
* **pipewire:** the engine states every cycle's facts (cycle facts C2) ([5e214ba](https://github.com/mcurcio/ear/commit/5e214ba0acfacf7bccc00a2e7bdb4b352b0fe048))
* **sendspin:** one owner of the played timeline, the source's clock (slice 3c) ([31285ed](https://github.com/mcurcio/ear/commit/31285edb700b9e61b219c798f383f2cfe82be445))
* **sendspin:** sendspin on the type's class, and a stopped node stays connected (slice 3c) ([9ad571a](https://github.com/mcurcio/ear/commit/9ad571a98e021d47cb89ae89bca2e0f03d4015bc))
* **sendspin:** skip every catch-up the cycle facts state, however long (cycle facts C4a) ([339149b](https://github.com/mcurcio/ear/commit/339149bf53c1212d63db30c16e6eab75a90f2874))
* **web:** companion badges on each node ([ddedf1f](https://github.com/mcurcio/ear/commit/ddedf1f6886fee0a61b038ca4212a43febd63f09))
* **web:** knobly syntax highlighting from knobly v1.0.0 ([feb45dc](https://github.com/mcurcio/ear/commit/feb45dc770f95fb4de9df86d9fa2c2b44f17223c))
* **web:** the add-node form takes knobly ([fece90f](https://github.com/mcurcio/ear/commit/fece90f84098977bb4459f55f5d7b49157fa5f82))
* **web:** the counters panel and recent marks (cycle facts D3) ([f3d5971](https://github.com/mcurcio/ear/commit/f3d5971ab1a72c9d0649568816801f138911499e))
* **web:** the engine facts show the graph's rate in kHz ([d9c8c9e](https://github.com/mcurcio/ear/commit/d9c8c9e51bbad3b1492654292a6d79b07218d1c7))
* **web:** the node box edits knobly ([d4a7fac](https://github.com/mcurcio/ear/commit/d4a7faceb6f770a54f068efbd7407dce862a693b))

### Bug Fixes

* a stop ends with ear: stopped, and the keeper's watch goes as it begins ([d5b08d0](https://github.com/mcurcio/ear/commit/d5b08d0ef85457957d7e4144e9d619e340b1c151)), closes [docs/node-types.md#failures](https://github.com/docs/node-types.md/issues/failures)
* an idle writer owes nothing, and a stall past the feed is proven back in sync (cycle facts C4 review) ([1a6bad8](https://github.com/mcurcio/ear/commit/1a6bad80b22bf09fb3bea1bec41ccbc0bf849793)), closes [docs/node-types.md#clock](https://github.com/docs/node-types.md/issues/clock)
* **control:** a node's text ends where it ends, in the file's line ending ([7f7aeff](https://github.com/mcurcio/ear/commit/7f7aeff9804c48ff0a8ebbfbc4b391d8c53d192a))
* **control:** companion lists only for nodes a client holds ([70c621a](https://github.com/mcurcio/ear/commit/70c621a61da45a5c4743f204a7d1d2f72fd6b27c))
* **core:** a declared delay is a latency path, a misfit plan fails configure (root classes R2 review) ([884f8c9](https://github.com/mcurcio/ear/commit/884f8c9a6ca68908e64c4a482e47bf76983df8f3))
* **core:** a declared delay names its path, and the path must exist (root classes R2) ([26e75ff](https://github.com/mcurcio/ear/commit/26e75fffdedb8bc60708eb53af767b6af4028f4b))
* **core:** a leaked configuration's store writes dropped, its record freed after its late close, checked on the real engine (slice 3c) ([7879852](https://github.com/mcurcio/ear/commit/78798528fcd5d2cc455a3b659f827fa9eb48e7ae))
* **core:** a left source never runs on a state that is gone, replaced strand workers joined (slice 3c) ([e302d49](https://github.com/mcurcio/ear/commit/e302d496e7b1a0a572fe3abd5548f7cf43459b21)), closes [writing-a-node-type.md#loops](https://github.com/mcurcio/writing-a-node-type.md/issues/loops)
* **core:** a multicast rtp node shares its port with a program that binds it by SO_REUSEADDR, and a wedged leak's refusal says so (slice 3c) ([4edb09d](https://github.com/mcurcio/ear/commit/4edb09dec3742cbf87009c2962f7998870acbec8)), closes [writing-a-node-type.md#claims](https://github.com/mcurcio/writing-a-node-type.md/issues/claims) [engine.md#rtp-receivers](https://github.com/mcurcio/engine.md/issues/rtp-receivers) [node-types.md#rtp](https://github.com/mcurcio/node-types.md/issues/rtp)
* **core:** a node whose only outputs are sinks' banks is silenced with no fade (slice 3c) ([874dce9](https://github.com/mcurcio/ear/commit/874dce905fe7732378997b9042f87ebe0695ff87)), closes [architecture.md#fades](https://github.com/mcurcio/architecture.md/issues/fades) [writing-a-node-type.md#sinks](https://github.com/mcurcio/writing-a-node-type.md/issues/sinks)
* **core:** a stall lasts while the graph is behind, not while cuts are adjacent ([7a1b4fe](https://github.com/mcurcio/ear/commit/7a1b4fe58f60049b59bd280674981237f5ab1114))
* **core:** a superseded node's counts reach the records, its span open until it is gone ([c82e5be](https://github.com/mcurcio/ear/commit/c82e5be3f63d8af846c2f1511ea97baa4a7aa0d6)), closes [docs/engine.md#counters](https://github.com/docs/engine.md/issues/counters)
* **core:** a type's throw at parse refuses the edit, and none goes unrecorded (root classes R4c review) ([d98ed47](https://github.com/mcurcio/ear/commit/d98ed476533c12ef539b677a403528e4f6c16d88))
* **core:** a type's throw is a fact about its node (root classes R4c) ([e598251](https://github.com/mcurcio/ear/commit/e598251e6233a28baf5569eb3834ebe1d01329ea))
* **core:** add a companion's latency in series beyond its bank's peers, and cover the companion rules (slice 3b.3 review) ([40be60a](https://github.com/mcurcio/ear/commit/40be60af040988a9c12899dd27e26d2f630acc2c))
* **core:** cuts counted for any span, not only the ring's (cycle facts C1 review) ([c27c109](https://github.com/mcurcio/ear/commit/c27c109460225b165f139a1c4d641c74362c4175))
* **core:** date a source by its paired outputs' declared latency, keep time exactly, and harden its waits (slice 3b.2 review) ([6c1299b](https://github.com/mcurcio/ear/commit/6c1299b5567f580e3258fedf87f4f3bf2e84eba9))
* **core:** date a split chunk's second call by its own first frame, and harden the block's latency (slice 3b.1 review) ([e4b72cf](https://github.com/mcurcio/ear/commit/e4b72cf66190e972e428e27613a6b45f715f9f05))
* **core:** keep a card's ALSA buffer, pin the quantum floor, and judge late or unmade deadlines fairly (slice 3b.4 review) ([f7a4295](https://github.com/mcurcio/ear/commit/f7a4295c3d029e5b0401bf25ae2b0944c4689456))
* **core:** stores refuse a blocking write on the main loop, tell an unconfirmed write apart, read and narrow on one descriptor (slice 3c) ([2e3888e](https://github.com/mcurcio/ear/commit/2e3888e86fa87a4092675d05ff81a3098e2a3976))
* **core:** teardown step 5 destroys a never-started loop's state before the instance (slice 3c) ([5fe3bf4](https://github.com/mcurcio/ear/commit/5fe3bf4a91955eae3308e677f83a6f907895ff01)), closes [docs/architecture.md#teardown-by-dependency](https://github.com/docs/architecture.md/issues/teardown-by-dependency) [writing-a-node-type.md#loops](https://github.com/mcurcio/writing-a-node-type.md/issues/loops)
* **core:** teardown step 5 frees the sinks before the instance (slice 3c) ([97d1c5e](https://github.com/mcurcio/ear/commit/97d1c5e8cbcaf6edb0eafb35255ad6ebb3d4d8ff)), closes [docs/architecture.md#teardown-by-dependency](https://github.com/docs/architecture.md/issues/teardown-by-dependency) [writing-a-node-type.md#sinks](https://github.com/mcurcio/writing-a-node-type.md/issues/sinks)
* **core:** the node machine says what it drops, and every dropped type fault is logged (root classes R4c) ([9756961](https://github.com/mcurcio/ear/commit/9756961572e76a6e39a07259eda10f8f0bd319e4))
* **core:** the recording path rule is core's, and a leaked recording closes before its path is free (slice 3c) ([ee8d4e8](https://github.com/mcurcio/ear/commit/ee8d4e83b2aed04089822f03f0bac27f662d1274)), closes [writing-a-node-type.md#recordings](https://github.com/mcurcio/writing-a-node-type.md/issues/recordings)
* no blocking invoke into a wedged data loop, from any path (root classes R4b) ([b9ab3db](https://github.com/mcurcio/ear/commit/b9ab3db61029b2cc98f7b9c00691f9b68f7f58f6))
* owned faults leave nothing registered and refusals survive nesting (root classes R4b review) ([10b4337](https://github.com/mcurcio/ear/commit/10b4337fc7d92c14e4c35a0cd3f769cba6bedbc7))
* **pipewire:** a lost cut fails, and a left listener keeps itself (cycle facts C2 review) ([a119aea](https://github.com/mcurcio/ear/commit/a119aea89a6b9fb647cfbe33f4f7e26dfe104eca))
* **pipewire:** no route around the property table (root classes R6 review) ([f7067aa](https://github.com/mcurcio/ear/commit/f7067aadfd3a5622748890bc12e18cbdc18463af))
* **sendspin:** a debt is paid from everything readable, skipping again what the writer adds while it is owed ([a3bbd37](https://github.com/mcurcio/ear/commit/a3bbd377781ccf300ce0382d2652edf685043b44))
* **sendspin:** a debt's payment skips in two passes at most, so the data thread's work is bounded ([d26e833](https://github.com/mcurcio/ear/commit/d26e833725ea973efebcc17b3046438521f5b206))
* **sendspin:** a node stopped while the server streams stays connected (slice 3c) ([66e0bb0](https://github.com/mcurcio/ear/commit/66e0bb06f42975d90dbd13c2da11aa862cda059a)), closes [node-types.md#sendspin](https://github.com/mcurcio/node-types.md/issues/sendspin)
* **sendspin:** a pending drop timeline skips a jump, the judge models it from the code's own facts, and S3c.6c owns one timeline (slice 3c) ([40a4fad](https://github.com/mcurcio/ear/commit/40a4fad7d64f144b215e7946e445c60a8e523625))
* **sendspin:** a stall's unskipped remainder is paid before the library is told (cycle facts C4) ([5affeb3](https://github.com/mcurcio/ear/commit/5affeb38c5a3751a3453917a11fc862fe0e46599))
* **sendspin:** a stop's drop is told, each owner reports in its own box, no report outlives its stream (slice 3c) ([9c45f6e](https://github.com/mcurcio/ear/commit/9c45f6e56965d4052cad311a465c62116f1e2f3c))
* **sendspin:** a stop's first tick ends what the clock kept about the ring (slice 3c) ([b561df6](https://github.com/mcurcio/ear/commit/b561df61530b4b741813931a8bd46a9b976d99c7))
* **sendspin:** count only open sockets in the transport's room, and end a connection closed under it while open ([e22f998](https://github.com/mcurcio/ear/commit/e22f99893bf76a4eaeb1872fea5381fe1b888fea))
* **sendspin:** ear owes lost time only while the library cannot realign itself (cycle facts C4) ([7632a62](https://github.com/mcurcio/ear/commit/7632a62e33ca5c2f59d24089195834b36b1933fe)), closes [docs/node-types.md#clock](https://github.com/docs/node-types.md/issues/clock)
* **sendspin:** one hand-over and one timeline between the code and the feed's writer (slice 3c) ([4514e1c](https://github.com/mcurcio/ear/commit/4514e1cb63f6d4dcae5616ac0cd166851866963c))
* **sendspin:** tell the library nothing from a cycle dated behind raw time, and pay a debt in one step with the read ([01b54dd](https://github.com/mcurcio/ear/commit/01b54dd731a45e65068ce3496bfd581368a6fbb1))
* **sendspin:** the drop's timeline skips a catch-up once, and expires after a feed's worth of cycles (slice 3c) ([5527d57](https://github.com/mcurcio/ear/commit/5527d576a195372f8b2de40129882daa0baa38b3))
* **sendspin:** the dropped feed's finish never runs ahead, and its bound is derived (slice 3c) ([73a3bf8](https://github.com/mcurcio/ear/commit/73a3bf88b9dcad84dacb79145251785335e7dd38))
* **sendspin:** the library floors its playtime only when starved, so a stall's catch-up stays in sync (cycle facts C4) ([b8e3596](https://github.com/mcurcio/ear/commit/b8e359689e25082522407a21fd20478836ce0685))
* **sendspin:** the library is told after untoldBound even under constant loss, and its underrun recovery is tested (cycle facts C4) ([a96f841](https://github.com/mcurcio/ear/commit/a96f8417506a0d5159a1e25df39df9158ce24338)), closes [docs/node-types.md#clock](https://github.com/docs/node-types.md/issues/clock)
* **sendspin:** the node keeps its own time as an honest sink with a local skip ([f599d4f](https://github.com/mcurcio/ear/commit/f599d4ff420b2c4de9a42806bf4d06dcb71aa1fd))
* the keeper's watch and descriptor go as the stop begins, and a stop is bounded by ear's own word ([582c55a](https://github.com/mcurcio/ear/commit/582c55a48b450c4a3de3abda6075808f5f8137eb))
* **web:** a node's mark lasts across a port-caused replace, and rows say how much in text ([ca5a4b5](https://github.com/mcurcio/ear/commit/ca5a4b570b046c6bd2f1f9e9e6e18b545ceca545))
* **web:** a reconnect's snapshot never marks companion drops as recent ([3bc87ab](https://github.com/mcurcio/ear/commit/3bc87ab8a43c329046441ec8a69aee18b275e19a))
* **web:** the knobly boxes in forced colours, at 4.5:1, light only ([3d3f230](https://github.com/mcurcio/ear/commit/3d3f2300305a6f1535be10dbdbf41156043ad295)), closes [#f1f3f5](https://github.com/mcurcio/ear/issues/f1f3f5) [#212529](https://github.com/mcurcio/ear/issues/212529) [#1864ab](https://github.com/mcurcio/ear/issues/1864ab) [#1c7ed6](https://github.com/mcurcio/ear/issues/1c7ed6) [#a35200](https://github.com/mcurcio/ear/issues/a35200) [#8a6100](https://github.com/mcurcio/ear/issues/8a6100) [#2b7a3a](https://github.com/mcurcio/ear/issues/2b7a3a) [#2b8a3e](https://github.com/mcurcio/ear/issues/2b8a3e) [#a61e1e](https://github.com/mcurcio/ear/issues/a61e1e) [#666](https://github.com/mcurcio/ear/issues/666) [#777](https://github.com/mcurcio/ear/issues/777) [#495057](https://github.com/mcurcio/ear/issues/495057) [#c92a2a](https://github.com/mcurcio/ear/issues/c92a2a) [#e03131](https://github.com/mcurcio/ear/issues/e03131) [docs/editor.md#highlighting](https://github.com/docs/editor.md/issues/highlighting)

### Dependencies

* **deps:** knobly's pin holds its URL templates, and a bump checks SHA256SUMS and is all or nothing ([50fbae5](https://github.com/mcurcio/ear/commit/50fbae59be7576f7e9d7e7e680a7a56845bfb82e))
* **deps:** upgrade knobly to 1.0.0, one pin for C++ and JS ([53a19f7](https://github.com/mcurcio/ear/commit/53a19f7307205837a194104a3128e9baaf67f660))

## [3.0.0](https://github.com/mcurcio/ear/compare/v2.0.0...v3.0.0) (2026-11-09)

### ⚠ BREAKING CHANGES

* **core:** standard output's node records change. A removal's
line is `node <n> removed dropped=<frames>` ("state":"stopping",
"phase":"removed") in place of `node <n> stopped dropped=<frames>`.
Every node record carries "phase". New lines:
`node <n> stopping: <cause>` (stop, disabling, removing, or
`restarting (settings changed: <keys>)` / `restarting (restart
invoked)`, with a "cause" object), `node <n> starting: <cause>` for a
node configuring after a restart, retry or enable, `node <n> stopped`,
`node <n> disabled` and `node <n> still <phase> after <s>s: waiting for
<what>` (with "waitingFor"). A restart now says stopping, not starting.
A node that fails after it ran says `failed lost: <detail>`, not
start-failed. The log record's never-emitted "run" field is gone. The
wire's node.state can now carry the states stopped and disabled, and
stopping for a restart.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MtEdSSMnWMDCszQdw5TZut
* **pipewire:** a sound card no longer drives the graph's clock. ear's internal clock always does,
and every linked card follows it, resampled to it, so no card output is bit-perfect; a card's loss
no longer moves the clock.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MtEdSSMnWMDCszQdw5TZut
* **output:** standard output is one JSON record a line instead of plain
status lines; a reader matching lines matches each record's msg.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MtEdSSMnWMDCszQdw5TZut

### Features

* **config:** disabled true; as a reserved node setting (slice 3a L-5) ([f3ff27a](https://github.com/mcurcio/ear/commit/f3ff27aa570ad75415873aaf26ead58f29fdef0b))
* **control:** the lifecycle on the wire: node.start/stop/restart/disable/enable, health on every node.state, protocol 1 (slice 3a L-6) ([1757efa](https://github.com/mcurcio/ear/commit/1757efa6a0af73a56135c5404efa95c761998025))
* **core:** acquire resources through a registry, start runs in two phases, tear them down by dependency ([acd8873](https://github.com/mcurcio/ear/commit/acd88731e0619573f1c511cf8e8393070f537332))
* **core:** fades, bounds and the wedged data loop (slice 3a L-3c) ([4050e76](https://github.com/mcurcio/ear/commit/4050e768eb737b8f82a782c27eb386b740745c67))
* **core:** node lifecycle machines for slice 3a, pure and tested by table ([454cee2](https://github.com/mcurcio/ear/commit/454cee2ecdef447fab096e25fa234d521e1d55df))
* **core:** NodeHost honours disabled, tells every lifecycle phase, and stdout gets its new node records (slice 3a L-3d) ([4417171](https://github.com/mcurcio/ear/commit/44171712707123a64f5f57bad53bd544a63e9971))
* **core:** the interim sink and file on the node's class (slice 3a L-4b) ([fea3700](https://github.com/mcurcio/ear/commit/fea37002878a719e19bab51cca60ed7374d8cede))
* **core:** the node type's class, configure and its contexts (slice 3a L-4a) ([a0194f6](https://github.com/mcurcio/ear/commit/a0194f60c324172f8b4801e7400fef48602a3d36))
* **core:** write node types as a spec and a behaviour, with Io by bank and a harness ([eb661cf](https://github.com/mcurcio/ear/commit/eb661cf1bc47fddfdd37931e84eaa9128703859c))
* **output:** a main-loop stall watchdog (slice 3a L-7) ([7037dde](https://github.com/mcurcio/ear/commit/7037ddee03a028b9742435401fbaf9caf2c2bbb8))
* **output:** add the output thread, descriptor capture and ear's own pw_log ([42d05a2](https://github.com/mcurcio/ear/commit/42d05a27b38113f0e3b6e88b97fb674ee238ff47))
* **output:** write every standard output line as one JSON record ([fb91f20](https://github.com/mcurcio/ear/commit/fb91f2001f2e486ae40f32af0afe4ef64b559636))
* **pipewire:** asynchronous drive and fade on port blocks for slice 3a ([571cebf](https://github.com/mcurcio/ear/commit/571cebf247a54fd93c7dc7a2fa8ce53557e337df))
* **pipewire:** ear's internal clock always drives, cards follow, with latency per bank pair ([da0ef2e](https://github.com/mcurcio/ear/commit/da0ef2e4360defcaeee68d2bd010c9c3abe1913f))

### Bug Fixes

* **control:** pace log records per node or source, so a flooding library cannot push out states and notes ([179bff3](https://github.com/mcurcio/ear/commit/179bff3b90cf005280b580158b04dcf973df50f7))
* **control:** snapshots stamped at the read, a disabled add's first node.state, the editor's own clock (slice 3a L-6 review) ([04afc31](https://github.com/mcurcio/ear/commit/04afc314047f0f9e3415079df3672c9671da1874))
* **core:** a port-caused replace is gone only after its older node, and stdout keeps superseded nodes apart (slice 3a L-3d review) ([bc87a66](https://github.com/mcurcio/ear/commit/bc87a66e3d1e1de585098cbfb6578dba6d0dea2d))
* **core:** close the lifetime gaps around a run's teardown and leak ([c75bd3e](https://github.com/mcurcio/ear/commit/c75bd3efcc00f3571d3023b6b4ea79b85214d6e2))
* **core:** faults during a restart's drive, input queue, record order and driveNow (slice 3a L-3a review) ([6871832](https://github.com/mcurcio/ear/commit/6871832c597f3ff38d540be2aae6ce42f7110421))
* **core:** key node types on named namespaces, and test real types through TypeHarness ([54b20c2](https://github.com/mcurcio/ear/commit/54b20c2bed2c11f26613035ff31621e078f400d7))
* **core:** members found by name, broken rules recorded, nothing a type does thrown out (slice 3a L-4a review) ([07929a1](https://github.com/mcurcio/ear/commit/07929a184d1316f94cf98348047597087b4a24c4))
* **core:** the sink ramp held exact, no throw on the drive path, file's open guarded (slice 3a L-4b review) ([0c8a0ae](https://github.com/mcurcio/ear/commit/0c8a0aecd396566a84b1686981cf93f7d6f72350))
* **core:** the wedge reaches the engine's adapters, bounds get a grace (slice 3a L-3c review) ([db6dd28](https://github.com/mcurcio/ear/commit/db6dd280e78dfbdbd7d8745966216175027af628))
* **output:** keep a crash's last words, and end the capture past a surviving duplicate ([0d25b28](https://github.com/mcurcio/ear/commit/0d25b28a62201dcdacb71cd3dcdf61173c13a24a))
* **output:** the stall's length measured by the beat, honest bounds (slice 3a L-7 review) ([3013d9f](https://github.com/mcurcio/ear/commit/3013d9fa789e5b2a1ae8572ce39df48958c1e3fa))
* **pipewire:** a data-thread stall never resets the internal clock off the raw clock ([c1bb9d2](https://github.com/mcurcio/ear/commit/c1bb9d246b2f83c1858f5f487e47e09938695bf2))
* **pipewire:** flush denormals only on the data thread, and combine only routed ports' latency ([442dad9](https://github.com/mcurcio/ear/commit/442dad96e5510acebb850b70e51cb46d7cfacc4e))
* **pipewire:** reset the engine log's bell with an exchange, so no wakeup is lost ([5506d0b](https://github.com/mcurcio/ear/commit/5506d0bf01fbd386abe77a98b63596b0b8663d90))
* **sendspin:** an address's share at the transport counts only sockets its peer has not closed ([43266e9](https://github.com/mcurcio/ear/commit/43266e921640c5b75bb7f8e1d7c214f8ddd02f63))
* **sendspin:** poll each transport socket once an admission, and pin the share and the clock's limits ([f1d6eec](https://github.com/mcurcio/ear/commit/f1d6eecf30e1ba9e28e065512f43f47ec95f0366))
* **sendspin:** require the boot log, so a key file it cannot narrow is never dropped ([be66fd3](https://github.com/mcurcio/ear/commit/be66fd391b7b4ec4458c6e1519cf08c95cc31fac))

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
