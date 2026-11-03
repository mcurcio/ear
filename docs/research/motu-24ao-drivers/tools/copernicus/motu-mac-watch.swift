// motu-mac-watch: event-driven arrival/departure log for the MOTU 24Ao on macOS (copernicus).
// Listens to CoreAudio: the device list, and on the MOTU device "is alive", "is running somewhere"
// and nominal sample rate. Logs every change with a UTC timestamp to ~/motu-test/watch.log. Once the
// MOTU has appeared, its disappearance or "is alive" = 0 writes ~/motu-test/DEAD and exits 1.
// DEAD is never cleared here: a person removes it after power-cycling the MOTU.
import CoreAudio
import Foundation

let dir = (CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : NSHomeDirectory() + "/motu-test")
try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
let logPath = dir + "/watch.log", deadPath = dir + "/DEAD"
let fmt = ISO8601DateFormatter(); fmt.formatOptions = [.withInternetDateTime, .withFractionalSeconds]

func log(_ s: String) {
    let line = "\(fmt.string(from: Date())) \(s)\n"
    FileHandle.standardOutput.write(line.data(using: .utf8)!)
    if let h = FileHandle(forWritingAtPath: logPath) { h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); h.closeFile() }
    else { FileManager.default.createFile(atPath: logPath, contents: line.data(using: .utf8)) }
}
func dead(_ why: String) -> Never {
    log("DEAD \(why)")
    FileManager.default.createFile(atPath: deadPath, contents: "\(fmt.string(from: Date())) \(why)\n".data(using: .utf8))
    exit(1)
}

func addr(_ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
}
func u32(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> UInt32? {
    var a = addr(sel); var v: UInt32 = 0; var n = UInt32(MemoryLayout<UInt32>.size)
    return AudioObjectGetPropertyData(obj, &a, 0, nil, &n, &v) == noErr ? v : nil
}
func f64(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> Double? {
    var a = addr(sel); var v: Float64 = 0; var n = UInt32(MemoryLayout<Float64>.size)
    return AudioObjectGetPropertyData(obj, &a, 0, nil, &n, &v) == noErr ? v : nil
}
func str(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
    var a = addr(sel); var v: Unmanaged<CFString>? = nil; var n = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(obj, &a, 0, nil, &n, &v) == noErr, let s = v else { return nil }
    return s.takeRetainedValue() as String
}
func devices() -> [AudioObjectID] {
    var a = addr(kAudioHardwarePropertyDevices); var n: UInt32 = 0
    let sys = AudioObjectID(kAudioObjectSystemObject)
    guard AudioObjectGetPropertyDataSize(sys, &a, 0, nil, &n) == noErr else { return [] }
    var ids = [AudioObjectID](repeating: 0, count: Int(n) / MemoryLayout<AudioObjectID>.size)
    guard AudioObjectGetPropertyData(sys, &a, 0, nil, &n, &ids) == noErr else { return [] }
    return ids
}
func findMotu() -> AudioObjectID? {
    devices().first { id in
        let name = str(id, kAudioObjectPropertyName) ?? "", maker = str(id, kAudioObjectPropertyManufacturer) ?? ""
        return name.contains("24Ao") || maker.uppercased().contains("MOTU")
    }
}

var motu: AudioObjectID? = nil
let q = DispatchQueue(label: "motu-watch")

func describe(_ id: AudioObjectID) -> String {
    "id=\(id) name=\(str(id, kAudioObjectPropertyName) ?? "?") uid=\(str(id, kAudioDevicePropertyDeviceUID) ?? "?") alive=\(u32(id, kAudioDevicePropertyDeviceIsAlive).map(String.init) ?? "?") running=\(u32(id, kAudioDevicePropertyDeviceIsRunningSomewhere).map(String.init) ?? "?") rate=\(f64(id, kAudioDevicePropertyNominalSampleRate).map { String($0) } ?? "?")"
}

func attach(_ id: AudioObjectID) {
    motu = id
    log("MOTU present \(describe(id))")
    for sel in [kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioDevicePropertyNominalSampleRate] {
        var a = addr(sel)
        AudioObjectAddPropertyListenerBlock(id, &a, q) { _, _ in
            log("MOTU changed \(describe(id))")
            if sel == kAudioDevicePropertyDeviceIsAlive, u32(id, sel) == 0 { log("MOTU not alive (CoreAudio)") }
        }
    }
}

// A log of arrivals and departures only: power-cycles are deliberate, and MOTU's driver keeps a hung
// device "alive", so the heartbeat (motu-mac-heartbeat) is what judges death and writes DEAD.
q.sync {
    if let id = findMotu() { attach(id) } else { log("MOTU absent at start; waiting for it to appear") }
}
var sysAddr = addr(kAudioHardwarePropertyDevices)
AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &sysAddr, q) { _, _ in
    let now = findMotu()
    if let had = motu, now != had { log("MOTU left CoreAudio's device list (was \(had))"); motu = nil }
    if motu == nil, let id = now { attach(id) }
}
log("watching (pid \(getpid()))")
dispatchMain()
