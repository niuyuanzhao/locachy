// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import CoreGraphics
import CoreMedia
import Foundation
import ScreenCaptureKit
import VideoToolbox

enum CompanionLog {
    static func info(_ text: String) { fputs("locachy-macos: \(text)\n", stderr) }
}

@available(macOS 14.0, *)
final class CaptureSession: NSObject, SCStreamOutput, SCStreamDelegate {
    private let transport: SecureTransport
    private let panel: Panel
    private let sampleQueue = DispatchQueue(label: "locachy.macos.capture")
    private let lock = NSLock()
    private var display: VirtualDisplay?
    private var stream: SCStream?
    private var encoder: VTCompressionSession?
    private var input: RemoteInput?
    private var stopped = false
    private var needsKeyframe = true
    private var encoding = false
    private var lastSubmit = 0.0
    private var scrollScale = 1.0

    init(transport: SecureTransport, panel: Panel) {
        self.transport = transport
        self.panel = panel
    }

    // Main actor: CGVirtualDisplay installs its WindowServer callbacks on main.
    @MainActor
    func start() async throws {
        guard CGPreflightScreenCaptureAccess() else {
            throw CompanionError("Screen Recording is not granted; grant it in System Settings and relaunch")
        }
        guard AXIsProcessTrusted() else {
            throw CompanionError("Accessibility is not granted; grant it in System Settings and relaunch")
        }
        guard let plan = VirtualCanvasSizing.plan(pixelsWide: panel.width, pixelsHigh: panel.height, scale: panel.scale)
        else { throw CompanionError("Receiver panel dimensions are unsupported") }
        let first = plan.bootstrap
        let millimeters = first.pointsWide >= first.pointsHigh ? CGSize(width: 300, height: 190) : CGSize(width: 190, height: 300)
        guard let created = VirtualDisplay(
            name: "LOCachy", pointsWide: first.pointsWide, pointsHigh: first.pointsHigh,
            scale: first.scale, descriptorMaxPixelsPerAxis: plan.descriptorMaxPixelsPerAxis,
            sizeInMillimeters: millimeters, serialNum: 0x4C4F4301, productID: 0x4C4F
        ) else { throw CompanionError("CGVirtualDisplay is unavailable or rejected this mode") }
        display = created
        do {
            if plan.requested != first {
                guard await created.resize(pointsWide: plan.requested.pointsWide, pointsHigh: plan.requested.pointsHigh,
                                           scale: plan.requested.scale, movingTo: nil) else {
                    throw CompanionError("macOS rejected the receiver resolution")
                }
            }
            let id = created.displayID
            guard CGDisplayIsInMirrorSet(id) != 1 else { throw CompanionError("macOS placed the LOCachy display in a mirror set") }
            let width = CGDisplayPixelsWide(id), height = CGDisplayPixelsHigh(id)
            guard width == plan.requested.pixelsWide, height == plan.requested.pixelsHigh else {
                throw CompanionError("Virtual display did not enter the requested mode")
            }
            let target = try await shareable(id)
            try configureEncoder(width: width, height: height)
            let bounds = CGDisplayBounds(id)
            scrollScale = bounds.width > 0 ? bounds.width / Double(width) : 1
            input = RemoteInput(displayID: id)
            let filter = SCContentFilter(display: target, excludingWindows: [])
            let configuration = SCStreamConfiguration()
            configuration.width = width
            configuration.height = height
            // Upstream measured that exactly 1/60 makes SCK skip early frames
            // (~51 fps); ask for 1/120 and rate-limit admission ourselves.
            configuration.minimumFrameInterval = CMTime(value: 1, timescale: 120)
            configuration.queueDepth = 4
            configuration.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            configuration.showsCursor = true
            let capture = SCStream(filter: filter, configuration: configuration, delegate: self)
            try capture.addStreamOutput(self, type: .screen, sampleHandlerQueue: sampleQueue)
            stream = capture
            transport.sendObject(["type": "streamConfig", "codec": "h264", "width": width,
                                  "height": height, "framesPerSecond": panel.fps])
            try await capture.startCapture()
            CompanionLog.info("Extended display \(id) is active at \(width)x\(height)")
        } catch {
            stop("Capture setup failed")
            throw error
        }
    }

    @MainActor
    private func shareable(_ id: CGDirectDisplayID) async throws -> SCDisplay {
        for _ in 0..<40 {
            if let match = try await SCShareableContent.current.displays.first(where: { $0.displayID == id }) { return match }
            try await Task.sleep(for: .milliseconds(250))
        }
        throw CompanionError("The virtual display did not appear in ScreenCaptureKit")
    }

    private func configureEncoder(width: Int, height: Int) throws {
        var created: VTCompressionSession?
        var status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264,
            encoderSpecification: [kVTVideoEncoderSpecification_EnableLowLatencyRateControl: kCFBooleanTrue] as CFDictionary,
            imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil, refcon: nil,
            compressionSessionOut: &created)
        if created == nil {
            status = VTCompressionSessionCreate(
                allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264,
                encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        }
        guard status == noErr, let created else { throw CompanionError("VideoToolbox H.264 encoder is unavailable") }
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_MaxFrameDelayCount, value: 0 as CFNumber)
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: 60 as CFNumber)
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: panel.fps as CFNumber)
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_AverageBitRate, value: 18_000_000 as CFNumber)
        VTSessionSetProperty(created, key: kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, value: kCFBooleanTrue)
        guard VTCompressionSessionPrepareToEncodeFrames(created) == noErr else {
            VTCompressionSessionInvalidate(created)
            throw CompanionError("VideoToolbox could not prepare the encoder")
        }
        encoder = created
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, CMSampleBufferIsValid(sample), let image = CMSampleBufferGetImageBuffer(sample) else { return }
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        guard !stopped, !encoding, let encoder, now - lastSubmit >= 0.95 / Double(panel.fps) else { lock.unlock(); return }
        encoding = true
        lastSubmit = now
        let properties = needsKeyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue!] as CFDictionary : nil
        needsKeyframe = false
        lock.unlock()
        let status = VTCompressionSessionEncodeFrame(
            encoder, imageBuffer: image, presentationTimeStamp: CMSampleBufferGetPresentationTimeStamp(sample),
            duration: .invalid, frameProperties: properties, infoFlagsOut: nil
        ) { [weak self] status, _, output in self?.encoded(status: status, sample: output) }
        if status != noErr { finishEncode(failed: true) }
    }

    private func encoded(status: OSStatus, sample: CMSampleBuffer?) {
        guard status == noErr, let sample, let data = annexB(sample) else { finishEncode(failed: true); return }
        do { try transport.sendVideo(data); finishEncode(failed: false) }
        catch { finishEncode(failed: true) }
    }

    private func finishEncode(failed: Bool) {
        lock.lock(); encoding = false; let ended = stopped; lock.unlock()
        if failed && !ended { transport.stop("Video encoder failed") }
    }

    private func annexB(_ sample: CMSampleBuffer) -> Data? {
        guard let block = CMSampleBufferGetDataBuffer(sample) else { return nil }
        var total = 0
        var pointer: UnsafeMutablePointer<Int8>?
        guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &total,
                                          dataPointerOut: &pointer) == kCMBlockBufferNoErr, let pointer else { return nil }
        var sets: [Data] = []
        if isKeyframe(sample), let format = CMSampleBufferGetFormatDescription(sample) {
            for index in 0..<2 {
                var set: UnsafePointer<UInt8>?
                var size = 0
                guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format, parameterSetIndex: index, parameterSetPointerOut: &set, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let set else { return nil }
                sets.append(Data(bytes: set, count: size))
            }
        }
        return AnnexB.fromAVCC(UnsafeRawBufferPointer(start: pointer, count: total), parameterSets: sets)
    }

    private func isKeyframe(_ sample: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]],
              let first = attachments.first else { return true }
        return !(first[kCMSampleAttachmentKey_NotSync] as? Bool ?? false)
    }

    func requestKeyframe() { lock.lock(); needsKeyframe = true; lock.unlock() }
    func scrollPointScale() -> Double { scrollScale }

    @MainActor
    func handleInput(_ object: [String: Any]) throws {
        // Before the output exists there is nothing to target: drop, never
        // fail the authenticated session over an early event.
        guard let input else { return }
        try input.handle(object, scrollScale: scrollScale)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        transport.stop("Capture stopped")
    }

    @MainActor
    func stop(_ reason: String) {
        lock.lock()
        guard !stopped else { lock.unlock(); return }
        stopped = true
        lock.unlock()
        input?.cancel()
        input = nil
        stream?.stopCapture { _ in }
        stream = nil
        if let encoder { VTCompressionSessionInvalidate(encoder) }
        encoder = nil
        // Releasing the only owner removes this session's virtual output.
        display = nil
        CompanionLog.info(reason)
    }
}
