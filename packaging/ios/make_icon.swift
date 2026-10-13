// make_icon.swift
// Regenerates the iOS app icon and launch-screen badge in packaging/ios/Assets.xcassets.
//
// Usage (from the repo root):   swift packaging/ios/make_icon.swift [nearest|smooth] [outDir]
//
// Icon = MightyMike512.png badge centered on the red/white fabric from the title page
// (Data/Images/titlepage.tga). The fabric is a clean, logo-free column of stripes cropped
// from the right side of the title page, scaled with nearest-neighbor and mirror-tiled
// horizontally to fill 1024x1024. Output is opaque (no alpha), as App Store icons require.
//
// Badge scaling modes:
//   nearest (default): nearest-neighbor 2x to 1024, then a high-quality downsample to the target size
//   smooth:            a single high-quality resample from 512 to the target size

import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let mode = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "nearest"
let outDir = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : "packaging/ios/Assets.xcassets"

let kIconSize = 1024
let kBadgeFraction: CGFloat = 0.84									// badge size relative to the canvas
let kFabricCrop = CGRect(x: 470, y: 40, width: 120, height: 400)	// clean stripes in titlepage.tga (640x480, top-left origin)

func loadImage(_ path: String) -> CGImage {
	let url = URL(fileURLWithPath: path) as CFURL
	guard let src = CGImageSourceCreateWithURL(url, nil),
		  let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
		fatalError("Can't load \(path)")
	}
	return img
}

func writePNG(_ img: CGImage, _ path: String) {
	let url = URL(fileURLWithPath: path) as CFURL
	guard let dst = CGImageDestinationCreateWithURL(url, UTType.png.identifier as CFString, 1, nil) else {
		fatalError("Can't write \(path)")
	}
	CGImageDestinationAddImage(dst, img, nil)
	if !CGImageDestinationFinalize(dst) { fatalError("Can't finalize \(path)") }
	print("wrote \(path) (\(img.width)x\(img.height))")
}

func makeContext(_ w: Int, _ h: Int, opaque: Bool) -> CGContext {
	let info = opaque ? CGImageAlphaInfo.noneSkipLast.rawValue : CGImageAlphaInfo.premultipliedLast.rawValue
	return CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
					 space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: info)!
}

// Nearest-neighbor 2x of the badge (keeps alpha)
func badgeNearest2x(_ badge: CGImage) -> CGImage {
	let ctx = makeContext(badge.width * 2, badge.height * 2, opaque: false)
	ctx.interpolationQuality = .none
	ctx.draw(badge, in: CGRect(x: 0, y: 0, width: badge.width * 2, height: badge.height * 2))
	return ctx.makeImage()!
}

let badge512 = loadImage("packaging/MightyMike512.png")
let title = loadImage("Data/Images/titlepage.tga")
guard let fabric = title.cropping(to: kFabricCrop) else { fatalError("bad fabric crop") }

let badgeSource = (mode == "smooth") ? badge512 : badgeNearest2x(badge512)

//
// App icon (opaque)
//

let ctx = makeContext(kIconSize, kIconSize, opaque: true)
ctx.setFillColor(CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 1))
ctx.fill(CGRect(x: 0, y: 0, width: kIconSize, height: kIconSize))

// Fabric: scale the column to full height, mirror-tile it horizontally around the center
ctx.interpolationQuality = .none
let scale = CGFloat(kIconSize) / kFabricCrop.height
let tileW = kFabricCrop.width * scale
let center = CGFloat(kIconSize) / 2
var i = -6
while i <= 6 {
	let x = center + CGFloat(i) * tileW
	if x < CGFloat(kIconSize) && x + tileW > 0 {
		ctx.saveGState()
		if i % 2 != 0 {							// mirror every other tile so stripes join seamlessly
			ctx.translateBy(x: x + tileW, y: 0)
			ctx.scaleBy(x: -1, y: 1)
			ctx.draw(fabric, in: CGRect(x: 0, y: 0, width: tileW, height: CGFloat(kIconSize)))
		} else {
			ctx.draw(fabric, in: CGRect(x: x, y: 0, width: tileW, height: CGFloat(kIconSize)))
		}
		ctx.restoreGState()
	}
	i += 1
}

// Badge with a soft drop shadow
let badgeSize = (CGFloat(kIconSize) * kBadgeFraction).rounded()
let badgeRect = CGRect(x: (CGFloat(kIconSize) - badgeSize) / 2, y: (CGFloat(kIconSize) - badgeSize) / 2,
					   width: badgeSize, height: badgeSize)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 36, color: CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 0.75))
ctx.interpolationQuality = .high
ctx.draw(badgeSource, in: badgeRect)
ctx.restoreGState()

writePNG(ctx.makeImage()!, "\(outDir)/AppIcon.appiconset/AppIcon-1024.png")

//
// Launch-screen badge (transparent, 1024x1024)
//

let lctx = makeContext(kIconSize, kIconSize, opaque: false)
lctx.interpolationQuality = .high
lctx.draw(badgeSource, in: CGRect(x: 0, y: 0, width: kIconSize, height: kIconSize))
writePNG(lctx.makeImage()!, "\(outDir)/LaunchBadge.imageset/LaunchBadge.png")
