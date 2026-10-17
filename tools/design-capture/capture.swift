// Renders the WEFT design boards with WebKit and writes, for each view:
//   <out>/<name>.png          2x reference screenshot
//   <out>/<name>.layout.json  every element's box, text, baseline and computed style
//
// Build & run:  swiftc -O capture.swift -o capture && ./capture <design/boards> <design/reference>

import Cocoa
import WebKit

struct Job {
    let board: String      // file in the boards dir
    let name: String       // output basename
    let width: CGFloat
    let height: CGFloat
    let marker: String     // text that appears once the board has rendered
    let setup: String?     // JS run before capture (e.g. switch tab)
}

let layoutJS = #"""
(() => {
  const out = [];
  let id = 0;
  const depthOf = (el) => { let d = 0; for (let p = el; p.parentElement; p = p.parentElement) d++; return d; };
  for (const el of document.body.querySelectorAll('*')) {
    const r = el.getBoundingClientRect();
    if (r.width === 0 && r.height === 0) continue;
    const cs = getComputedStyle(el);
    if (cs.display === 'none' || cs.visibility === 'hidden') continue;
    const rec = { i: id++, tag: el.tagName.toLowerCase(), d: depthOf(el),
      x: r.x, y: r.y, w: r.width, h: r.height,
      font: cs.fontFamily, fs: parseFloat(cs.fontSize), fw: cs.fontWeight, ls: cs.letterSpacing,
      color: cs.color, bg: cs.backgroundColor, br: cs.borderRadius,
      bw: cs.borderTopWidth, bs: cs.borderTopStyle, bc: cs.borderTopColor, disp: cs.display };
    const label = el.getAttribute('aria-label'); if (label) rec.aria = label;
    let textNode = null, text = '';
    for (const n of el.childNodes) if (n.nodeType === 3 && n.textContent.trim()) { textNode = textNode || n; text += n.textContent; }
    if (textNode) {
      rec.text = text.trim();
      const range = document.createRange();
      range.selectNodeContents(textNode);
      const rects = range.getClientRects();
      const tr = range.getBoundingClientRect();
      rec.tx = tr.x; rec.ty = tr.y; rec.tw = tr.width; rec.th = tr.height; rec.lines = rects.length;
      if (!/flex|grid/.test(cs.display)) {
        const probe = document.createElement('span');
        probe.style.cssText = 'display:inline-block;width:0;height:0;vertical-align:baseline;padding:0;margin:0;border:0';
        el.insertBefore(probe, textNode);
        rec.base = probe.getBoundingClientRect().top;
        probe.remove();
      }
    }
    out.push(rec);
  }
  return JSON.stringify(out);
})()
"""#

final class Capturer: NSObject, WKNavigationDelegate {
    let boards: URL
    let outDir: URL
    var jobs: [Job]
    let window: NSWindow
    let web: WKWebView

    init(boards: URL, outDir: URL, jobs: [Job]) {
        self.boards = boards
        self.outDir = outDir
        self.jobs = jobs
        let config = WKWebViewConfiguration()
        config.preferences.setValue(true, forKey: "allowFileAccessFromFileURLs")
        web = WKWebView(frame: NSRect(x: 0, y: 0, width: 1440, height: 960), configuration: config)
        window = NSWindow(contentRect: NSRect(x: -20000, y: -20000, width: 1440, height: 960),
                          styleMask: [.borderless], backing: .buffered, defer: false)
        super.init()
        window.contentView = web
        window.orderFrontRegardless()
        web.navigationDelegate = self
    }

    func next() {
        guard let job = jobs.first else { NSApp.terminate(nil); return }
        let size = NSSize(width: job.width, height: job.height)
        window.setContentSize(size)
        web.frame = NSRect(origin: .zero, size: size)
        let url = boards.appendingPathComponent(job.board)
        web.loadFileURL(url, allowingReadAccessTo: boards)
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        poll(0)
    }

    func poll(_ attempt: Int) {
        let job = jobs[0]
        let js = "document.fonts.status === 'loaded' && document.body.innerText.indexOf(\(jsonString(job.marker))) >= 0"
        web.evaluateJavaScript(js) { result, _ in
            if (result as? Bool) == true {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { self.runSetup() }
            } else if attempt > 200 {
                print("timeout waiting for", job.name); self.jobs.removeFirst(); self.next()
            } else {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { self.poll(attempt + 1) }
            }
        }
    }

    func runSetup() {
        let job = jobs[0]
        guard let setup = job.setup else { capture(); return }
        web.evaluateJavaScript(setup) { _, err in
            if let err = err { print("setup error", job.name, err) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { self.capture() }
        }
    }

    func capture() {
        let job = jobs[0]
        let cfg = WKSnapshotConfiguration()
        cfg.rect = NSRect(x: 0, y: 0, width: job.width, height: job.height)
        cfg.afterScreenUpdates = true
        web.takeSnapshot(with: cfg) { image, err in
            guard let image = image, let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
                print("snapshot failed", job.name, err as Any); self.finishJob(); return
            }
            let rep = NSBitmapImageRep(cgImage: cg)
            let png = rep.representation(using: .png, properties: [:])!
            try! png.write(to: self.outDir.appendingPathComponent(job.name + ".png"))
            print("wrote", job.name + ".png", cg.width, "x", cg.height)
            self.web.evaluateJavaScript(layoutJS) { result, err in
                if let s = result as? String {
                    try! s.write(to: self.outDir.appendingPathComponent(job.name + ".layout.json"), atomically: true, encoding: .utf8)
                    print("wrote", job.name + ".layout.json", s.count, "bytes")
                } else { print("layout failed", job.name, err as Any) }
                self.finishJob()
            }
        }
    }

    func finishJob() { jobs.removeFirst(); next() }
}

func jsonString(_ s: String) -> String {
    let d = try! JSONSerialization.data(withJSONObject: [s])
    let a = String(data: d, encoding: .utf8)!
    return String(a.dropFirst().dropLast())
}

let args = CommandLine.arguments
guard args.count >= 3 else { print("usage: capture <boardsDir> <outDir>"); exit(2) }
let boards = URL(fileURLWithPath: args[1], isDirectory: true)
let out = URL(fileURLWithPath: args[2], isDirectory: true)
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

let clickTab = { (label: String) in
    "Array.from(document.querySelectorAll('button')).find(b => b.textContent.trim() === '\(label)').click()"
}
let jobs = [
    Job(board: "02-play-patch.html", name: "play", width: 1440, height: 960, marker: "Glass Choir", setup: nil),
    Job(board: "02-play-patch.html", name: "patch", width: 1440, height: 960, marker: "Glass Choir", setup: clickTab("PATCH")),
    Job(board: "03-live.html", name: "live", width: 1440, height: 960, marker: "MORPH", setup: nil),
    Job(board: "04-how-it-works.html", name: "how-it-works", width: 1440, height: 940, marker: "HOW IT WORKS", setup: nil),
    Job(board: "05-library.html", name: "library", width: 1440, height: 940, marker: "LIBRARY", setup: nil),
    Job(board: "01-research.html", name: "research", width: 1440, height: 2000, marker: "RESEARCH", setup: nil),
]

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let capturer = Capturer(boards: boards, outDir: out, jobs: jobs)
DispatchQueue.main.async { capturer.next() }
app.run()
