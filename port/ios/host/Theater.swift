/* Theater mode (display.immersive, visionOS 26 and later): an immersive space
whose Compositor Services layer shows the game's picture on a screen placed in
the room (host_theater.m draws it). SDL's window scene stays as it is: the
space is a second scene, opened through a SwiftUI hosting delegate, and its
frames are drawn from the game's thread in gpu_present, as the Phase 3
prototype showed works with SDL's run-loop slices
(halovision/step-tools/phase3-prototype). */
import CompositorServices
import SwiftUI
import UIKit

@available(visionOS 26.0, *)
struct TheaterLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        // the picture is flat, so foveation would only blur its edges
        configuration.isFoveationEnabled = false
    }
}

@available(visionOS 26.0, *)
final class TheaterSceneDelegate: UIResponder, UIHostingSceneDelegate {
    static let environment: any ImmersionStyle = host_theater_dark() != 0 ? .full : .mixed

    static var rootScene: some Scene {
        ImmersiveSpace(id: "theater") {
            CompositorLayer(configuration: TheaterLayerConfiguration()) { renderer in
                host_theater_attach(Unmanaged.passUnretained(renderer).toOpaque())
            }
        }
        // display.theater_environment: the room around the screen, or the dark
        .immersionStyle(selection: .constant(Self.environment), in: .mixed, .full)
    }
}

/* opens the space; host_theater.m calls it once SDL's window is up */
@_cdecl("host_theater_open")
public func host_theater_open() {
    guard #available(visionOS 26.0, *) else {
        host_theater_log("theater: display.immersive needs visionOS 26 or later; the game stays in its window")
        return
    }
    guard let request = UISceneSessionActivationRequest(hostingDelegateClass: TheaterSceneDelegate.self, id: "theater") else {
        host_theater_log("theater: the immersive space can't be requested")
        return
    }
    UIApplication.shared.activateSceneSession(for: request) { error in
        host_theater_log("theater: the immersive space didn't open: \(error.localizedDescription)")
    }
}

private func host_theater_log(_ message: String) {
    message.withCString { host_theater_log_c($0) }
}
