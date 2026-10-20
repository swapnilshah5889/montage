// GPU effects with Core Image on Metal. Compiled with ARC.

#include "Effects.h"

#import <Vision/Vision.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <QFile>
#include <QHash>
#include <QTextStream>
#include <cmath>
#include <functional>

namespace mtg {

namespace {

// Deterministic pseudo-random in [0, 1) for time-based effects (glitch, grain).
double hash01(int a, int b = 0)
{
    quint32 h = quint32(a) * 2654435761u ^ quint32(b) * 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / double(0x1000000);
}

CIImage *ciFilter(NSString *name, CIImage *input, NSDictionary *params = @{})
{
    CIFilter *f = [CIFilter filterWithName:name];
    if (!f) return input;
    if (input) [f setValue:input forKey:kCIInputImageKey];
    for (NSString *k in params) [f setValue:params[k] forKey:k];
    CIImage *out = f.outputImage;
    return out ?: input;
}

CIImage *colorControls(CIImage *img, double brightness, double contrast, double saturation)
{
    return ciFilter(@"CIColorControls", img, @{@"inputBrightness": @(brightness), @"inputContrast": @(contrast),
                                            @"inputSaturation": @(saturation)});
}

// a > 0 warms, a < 0 cools.
CIImage *temperature(CIImage *img, double a)
{
    return ciFilter(@"CITemperatureAndTint", img, @{@"inputNeutral": [CIVector vectorWithX:6500 Y:0],
                                                 @"inputTargetNeutral": [CIVector vectorWithX:6500 - 3000 * a Y:0]});
}

CIImage *crop(CIImage *img, CGRect r)
{
    return [img imageByCroppingToRect:r];
}

CIImage *blur(CIImage *img, double radius, CGRect extent)
{
    if (radius < 0.3) return img;
    return crop(ciFilter(@"CIGaussianBlur", [img imageByClampingToExtent], @{@"inputRadius": @(radius)}), extent);
}

CIImage *solid(CGRect r, double red, double green, double blue, double alpha)
{
    CIImage *c = [CIImage imageWithColor:[CIColor colorWithRed:red green:green blue:blue alpha:alpha]];
    return crop(c, r);
}

CIImage *over(CIImage *top, CIImage *bottom)
{
    return ciFilter(@"CISourceOverCompositing", top, @{kCIInputBackgroundImageKey: bottom});
}

CIImage *mix(CIImage *a, CIImage *b, double amount)  // 0 = a, 1 = b
{
    if (amount >= 0.999) return b;
    if (amount <= 0.001) return a;
    return ciFilter(@"CIDissolveTransition", a, @{kCIInputTargetImageKey: b, kCIInputTimeKey: @(amount)});
}

CIImage *channel(CIImage *img, int which, bool keepAlpha)
{
    CIVector *zero = [CIVector vectorWithX:0 Y:0 Z:0 W:0];
    CIVector *r = which == 0 ? [CIVector vectorWithX:1 Y:0 Z:0 W:0] : zero;
    CIVector *g = which == 1 ? [CIVector vectorWithX:0 Y:1 Z:0 W:0] : zero;
    CIVector *b = which == 2 ? [CIVector vectorWithX:0 Y:0 Z:1 W:0] : zero;
    CIVector *a = keepAlpha ? [CIVector vectorWithX:0 Y:0 Z:0 W:1] : zero;
    return ciFilter(@"CIColorMatrix", img, @{@"inputRVector": r, @"inputGVector": g, @"inputBVector": b, @"inputAVector": a});
}

CIImage *rgbSplit(CIImage *img, double d, CGRect extent)
{
    if (d < 0.3) return img;
    // Opaque single-channel layers combined with max(): alpha stays 1 and each
    // channel comes from its own shifted copy.
    CIImage *r = [channel(img, 0, true) imageByApplyingTransform:CGAffineTransformMakeTranslation(d, 0)];
    CIImage *g = channel(img, 1, true);
    CIImage *b = [channel(img, 2, true) imageByApplyingTransform:CGAffineTransformMakeTranslation(-d, 0)];
    CIImage *rg = ciFilter(@"CIMaximumCompositing", r, @{kCIInputBackgroundImageKey: g});
    return crop(ciFilter(@"CIMaximumCompositing", b, @{kCIInputBackgroundImageKey: rg}), extent);
}

CIImage *grain(CIImage *img, double amount, double t, CGRect extent)
{
    CIImage *noise = [CIFilter filterWithName:@"CIRandomGenerator"].outputImage;
    const int frame = int(std::floor(t * 24));
    noise = [noise imageByApplyingTransform:CGAffineTransformMakeTranslation(hash01(frame) * 512, hash01(frame, 7) * 512)];
    CIVector *g = [CIVector vectorWithX:0 Y:1 Z:0 W:0];
    noise = ciFilter(@"CIColorMatrix", noise, @{@"inputRVector": g, @"inputGVector": g, @"inputBVector": g,
                                            @"inputAVector": [CIVector vectorWithX:0 Y:0 Z:0 W:0],
                                            @"inputBiasVector": [CIVector vectorWithX:0 Y:0 Z:0 W:1]});
    CIImage *grained = ciFilter(@"CISoftLightBlendMode", crop(noise, extent), @{kCIInputBackgroundImageKey: img});
    return mix(img, grained, std::clamp(amount * 0.55, 0.0, 1.0));
}

CIImage *vignette(CIImage *img, double amount, CGRect e)
{
    return ciFilter(@"CIVignetteEffect", img, @{kCIInputCenterKey: [CIVector vectorWithX:CGRectGetMidX(e) Y:CGRectGetMidY(e)],
                                             @"inputRadius": @(std::hypot(e.size.width, e.size.height) * 0.42),
                                             @"inputIntensity": @(0.9 * amount), @"inputFalloff": @(0.6)});
}

CIImage *glow(CIImage *img, double radius, double intensity, CGRect e)
{
    return crop(ciFilter(@"CIBloom", [img imageByClampingToExtent], @{@"inputRadius": @(radius), @"inputIntensity": @(intensity)}), e);
}

// Teal shadows, warm highlights.
CIImage *splitTone(CIImage *img, double amount)
{
    CIImage *teal = ciFilter(@"CIColorMatrix", img, @{@"inputBiasVector": [CIVector vectorWithX:-0.04 * amount Y:0.015 * amount Z:0.06 * amount W:0]});
    CIImage *warm = ciFilter(@"CIColorMatrix", img, @{@"inputBiasVector": [CIVector vectorWithX:0.06 * amount Y:0.02 * amount Z:-0.04 * amount W:0]});
    CIVector *l = [CIVector vectorWithX:0.299 Y:0.587 Z:0.114 W:0];
    CIImage *luma = ciFilter(@"CIColorMatrix", img, @{@"inputRVector": l, @"inputGVector": l, @"inputBVector": l});
    return ciFilter(@"CIBlendWithMask", warm, @{kCIInputBackgroundImageKey: teal, kCIInputMaskImageKey: luma});
}

CIImage *glitch(CIImage *img, double amount, double t, CGRect e)
{
    const int step = int(std::floor(t * 12));
    CIImage *out = rgbSplit(img, (6 + 10 * hash01(step, 1)) * amount * e.size.height / 1080.0, e);
    const int bands = 3 + int(hash01(step, 2) * 4);
    for (int i = 0; i < bands; ++i) {
        const double y = hash01(step, 10 + i) * e.size.height;
        const double h = (0.02 + 0.06 * hash01(step, 20 + i)) * e.size.height;
        const double dx = (hash01(step, 30 + i) - 0.5) * 0.08 * e.size.width * amount;
        CIImage *band = [crop(out, CGRectMake(0, y, e.size.width, h)) imageByApplyingTransform:CGAffineTransformMakeTranslation(dx, 0)];
        out = crop(over(band, out), e);
    }
    return out;
}

CIImage *letterbox(CIImage *img, CGRect e)
{
    const double bar = (e.size.height - e.size.width / 2.39) / 2;
    if (bar <= 0) return img;
    CIImage *top = solid(CGRectMake(0, e.size.height - bar, e.size.width, bar), 0, 0, 0, 1);
    CIImage *bottom = solid(CGRectMake(0, 0, e.size.width, bar), 0, 0, 0, 1);
    return over(bottom, over(top, img));
}

CIImage *lightLeak(CIImage *img, double amount, double t, CGRect e)
{
    const double cx = e.size.width * (0.5 + 0.45 * std::sin(t * 0.7));
    CIImage *g = ciFilter(@"CIRadialGradient", nil, @{kCIInputCenterKey: [CIVector vectorWithX:cx Y:e.size.height * 0.75],
                                                   @"inputRadius0": @(0), @"inputRadius1": @(e.size.width * 0.6),
                                                   @"inputColor0": [CIColor colorWithRed:1 green:0.45 blue:0.1 alpha:std::clamp(0.6 * amount, 0.0, 1.0)],
                                                   @"inputColor1": [CIColor colorWithRed:0 green:0 blue:0 alpha:0]});
    return ciFilter(@"CIScreenBlendMode", crop(g, e), @{kCIInputBackgroundImageKey: img});
}

CIImage *scanlines(CIImage *img, double s, CGRect e)
{
    CIImage *stripes = ciFilter(@"CIStripesGenerator", nil, @{@"inputColor0": [CIColor colorWithRed:0 green:0 blue:0 alpha:0.18],
                                                          @"inputColor1": [CIColor colorWithRed:0 green:0 blue:0 alpha:0],
                                                          @"inputWidth": @(std::max(1.0, 2 * s)), @"inputSharpness": @(1)});
    stripes = [stripes imageByApplyingTransform:CGAffineTransformMakeRotation(M_PI / 2)];
    return over(crop(stripes, e), img);
}

// Keeps the subject (people, products, animals: Vision's foreground
// instances) and makes everything else transparent, with softened edges.
// Returns the input unchanged when nothing stands out.
CIImage *cutout(CIImage *img, CGRect e)
{
    if (@available(macOS 14.0, *)) {
        VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCIImage:img options:@{}];
        VNGenerateForegroundInstanceMaskRequest *req = [[VNGenerateForegroundInstanceMaskRequest alloc] init];
        NSError *error = nil;
        if (![handler performRequests:@[req] error:&error]) return img;
        VNInstanceMaskObservation *obs = req.results.firstObject;
        if (!obs || obs.allInstances.count == 0) return img;
        CVPixelBufferRef maskBuffer = [obs generateScaledMaskForImageForInstances:obs.allInstances fromRequestHandler:handler error:&error];
        if (!maskBuffer) return img;
        CIImage *mask = [CIImage imageWithCVPixelBuffer:maskBuffer];
        CVPixelBufferRelease(maskBuffer);
        // Line the mask up with the frame, then soften its edge by about a pixel.
        mask = [mask imageByApplyingTransform:CGAffineTransformMakeScale(e.size.width / mask.extent.size.width,
                                                                            e.size.height / mask.extent.size.height)];
        mask = blur(mask, std::max(0.6, e.size.height / 1080.0), e);
        return ciFilter(@"CIBlendWithMask", img, @{kCIInputBackgroundImageKey: [CIImage emptyImage], kCIInputMaskImageKey: mask});
    }
    return img;
}

CIImage *affineAround(CIImage *img, CGRect e, double scale, double dx, double dy)
{
    CGAffineTransform t = CGAffineTransformMakeTranslation(CGRectGetMidX(e) + dx, CGRectGetMidY(e) + dy);
    t = CGAffineTransformScale(t, scale, scale);
    t = CGAffineTransformTranslate(t, -CGRectGetMidX(e), -CGRectGetMidY(e));
    return crop([[img imageByClampingToExtent] imageByApplyingTransform:t], e);
}

}  // namespace

struct EffectProcessor::Impl {
    CIContext *ctx = nil;
    CGColorSpaceRef space = nullptr;
    QHash<QString, NSData *> lutData;
    QHash<QString, int> lutSize;

    // Reads an Adobe/Resolve .cube 3D LUT into Core Image's RGBA float layout.
    bool loadLut(const QString &path)
    {
        if (lutData.contains(path)) return lutData.value(path) != nil;
        QFile f(path);
        int n = 0;
        std::vector<float> rgba;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            while (!in.atEnd()) {
                const QString line = in.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#')) continue;
                if (line.startsWith(QLatin1String("LUT_3D_SIZE"))) {
                    n = line.section(' ', 1, 1).toInt();
                    rgba.reserve(size_t(n) * n * n * 4);
                    continue;
                }
                if (!line.at(0).isDigit() && line.at(0) != '-' && line.at(0) != '.') continue;
                const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
                if (parts.size() < 3) continue;
                rgba.insert(rgba.end(), {parts[0].toFloat(), parts[1].toFloat(), parts[2].toFloat(), 1.0f});
            }
        }
        const bool ok = n >= 2 && rgba.size() == size_t(n) * n * n * 4;
        lutData.insert(path, ok ? [NSData dataWithBytes:rgba.data() length:rgba.size() * sizeof(float)] : nil);
        lutSize.insert(path, n);
        return ok;
    }

    CIImage *one(CIImage *img, const Effect &fx, double t, CGRect e, bool layer = false)
    {
        const double a = fx.amount;
        const double s = e.size.height / 1080.0;
        const QString &n = fx.name;
        if (layer && (n == QLatin1String("glow") || n == QLatin1String("neon"))) {
            // Text on a transparent layer: blurred, brightened copy underneath.
            CIImage *halo = blur(img, (n == QLatin1String("neon") ? 16 : 10) * s, e);
            const double k = 1 + 1.6 * a;
            halo = ciFilter(@"CIColorMatrix", halo, @{@"inputRVector": [CIVector vectorWithX:k Y:0 Z:0 W:0],
                                                     @"inputGVector": [CIVector vectorWithX:0 Y:k Z:0 W:0],
                                                     @"inputBVector": [CIVector vectorWithX:0 Y:0 Z:k W:0],
                                                     @"inputAVector": [CIVector vectorWithX:0 Y:0 Z:0 W:k]});
            return over(img, over(halo, halo));
        }
        if (n == QLatin1String("brightness")) return colorControls(img, 0.25 * a, 1, 1);
        if (n == QLatin1String("contrast")) return colorControls(img, 0, 1 + 0.5 * a, 1);
        if (n == QLatin1String("saturation")) return colorControls(img, 0, 1, std::max(0.0, 1 + a));
        if (n == QLatin1String("exposure")) return ciFilter(@"CIExposureAdjust", img, @{@"inputEV": @(1.5 * a)});
        if (n == QLatin1String("temperature")) return temperature(img, 0.6 * a);
        if (n == QLatin1String("warm")) return colorControls(temperature(img, 0.5 * a), 0, 1, 1 + 0.08 * a);
        if (n == QLatin1String("cool")) return temperature(img, -0.5 * a);
        if (n == QLatin1String("vivid")) return colorControls(ciFilter(@"CIVibrance", img, @{@"inputAmount": @(0.8 * a)}), 0, 1 + 0.08 * a, 1);
        if (n == QLatin1String("cinematic"))
            return vignette(splitTone(colorControls(img, -0.02 * a, 1 + 0.15 * a, 1 - 0.1 * a), a), 0.5 * a, e);
        if (n == QLatin1String("vintage")) {
            CIImage *v = ciFilter(@"CISepiaTone", img, @{@"inputIntensity": @(0.35 * a)});
            v = ciFilter(@"CIColorMatrix", colorControls(v, 0, 1 - 0.1 * a, 0.9),
                      @{@"inputBiasVector": [CIVector vectorWithX:0.05 * a Y:0.04 * a Z:0.03 * a W:0]});
            return grain(vignette(v, 0.6 * a, e), 0.5 * a, t, e);
        }
        if (n == QLatin1String("black-white")) return mix(img, ciFilter(@"CIPhotoEffectMono", img), a);
        if (n == QLatin1String("noir")) return mix(img, ciFilter(@"CIPhotoEffectNoir", img), a);
        if (n == QLatin1String("faded")) return mix(img, ciFilter(@"CIPhotoEffectFade", img), a);
        if (n == QLatin1String("dramatic")) return vignette(colorControls(img, -0.03 * a, 1 + 0.3 * a, 1 + 0.1 * a), 0.8 * a, e);
        if (n == QLatin1String("golden-hour"))
            return glow(colorControls(temperature(img, 0.8 * a), 0.02 * a, 1.05, 1 + 0.15 * a), 8 * s, 0.3 * a, e);
        if (n == QLatin1String("neon")) return glow(colorControls(img, 0, 1 + 0.15 * a, 1 + 0.5 * a), 12 * s, 0.8 * a, e);
        if (n == QLatin1String("blur")) return blur(img, 14 * a * s, e);
        if (n == QLatin1String("glow")) return glow(img, 12 * s, 0.9 * a, e);
        if (n == QLatin1String("vignette")) return vignette(img, a, e);
        if (n == QLatin1String("grain")) return grain(img, a, t, e);
        if (n == QLatin1String("sharpen")) return ciFilter(@"CISharpenLuminance", img, @{@"inputSharpness": @(0.8 * a)});
        if (n == QLatin1String("rgb-split")) return rgbSplit(img, 6 * a * s, e);
        if (n == QLatin1String("glitch")) return glitch(img, a, t, e);
        if (n == QLatin1String("vhs"))
            return grain(scanlines(blur(colorControls(rgbSplit(img, 3 * s * a, e), 0, 1.05, 1 + 0.25 * a), 1.2 * s, e), s, e), 0.5 * a, t, e);
        if (n == QLatin1String("pixelate"))
            return crop(ciFilter(@"CIPixellate", img, @{kCIInputCenterKey: [CIVector vectorWithX:0 Y:0], @"inputScale": @(std::max(2.0, 24 * a * s))}), e);
        if (n == QLatin1String("posterize")) return ciFilter(@"CIColorPosterize", img, @{@"inputLevels": @(std::max(2.0, 8 - 4 * a))});
        if (n == QLatin1String("letterbox")) return letterbox(img, e);
        if (n == QLatin1String("light-leak")) return lightLeak(img, a, t, e);
        if (n == QLatin1String("motion-blur"))
            return crop(ciFilter(@"CIMotionBlur", [img imageByClampingToExtent], @{@"inputRadius": @(30 * a * s), @"inputAngle": @(0)}), e);
        if (n == QLatin1String("zoom-blur"))
            return crop(ciFilter(@"CIZoomBlur", [img imageByClampingToExtent], @{kCIInputCenterKey: [CIVector vectorWithX:CGRectGetMidX(e) Y:CGRectGetMidY(e)],
                                                                             @"inputAmount": @(18 * a * s)}), e);
        if (n == QLatin1String("cutout")) return a >= 0.999 ? cutout(img, e) : mix(img, cutout(img, e), std::clamp(a, 0.0, 1.0));
        if (n == QLatin1String("lut") && !fx.path.isEmpty() && loadLut(fx.path)) {
            CIImage *graded = ciFilter(@"CIColorCubeWithColorSpace", img, @{@"inputCubeDimension": @(lutSize.value(fx.path)),
                                                                         @"inputCubeData": lutData.value(fx.path),
                                                                         @"inputColorSpace": (__bridge id)space});
            return mix(img, graded, std::clamp(a, 0.0, 1.0));
        }
        return img;
    }

    CIImage *moment(CIImage *img, const Effect &fx, double t, double u, CGRect e)
    {
        const double a = fx.amount;
        const double s = e.size.height / 1080.0;
        const QString &n = fx.name;
        if (n == QLatin1String("flash")) return over(solid(e, 1, 1, 1, std::clamp(a * std::pow(std::max(0.0, 1 - u * 1.5), 2), 0.0, 1.0)), img);
        if (n == QLatin1String("white-out")) return over(solid(e, 1, 1, 1, std::clamp(a * std::sin(M_PI * u), 0.0, 1.0)), img);
        if (n == QLatin1String("fade-black")) return over(solid(e, 0, 0, 0, std::clamp(a * std::sin(M_PI * u), 0.0, 1.0)), img);
        if (n == QLatin1String("shake")) {
            const double amp = 0.02 * e.size.height * a * (1 - u);
            return affineAround(img, e, 1.04, amp * std::sin(t * 55), amp * std::cos(t * 47));
        }
        if (n == QLatin1String("zoom-punch")) return affineAround(img, e, 1 + 0.12 * a * std::pow(1 - u, 3), 0, 0);
        if (n == QLatin1String("rgb-split")) return rgbSplit(img, 14 * a * s * (1 - u), e);
        if (n == QLatin1String("blur-in")) return blur(img, 20 * a * s * (1 - u), e);
        if (n == QLatin1String("blur-out")) return blur(img, 20 * a * s * u, e);
        return one(img, fx, t, e);
    }

    QImage run(const QImage &input, const std::function<CIImage *(CIImage *, CGRect)> &chain)
    {
        @autoreleasepool {
            QImage src = input.format() == QImage::Format_ARGB32_Premultiplied
                             ? input : input.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            NSData *bytes = [NSData dataWithBytes:src.constBits() length:size_t(src.sizeInBytes())];
            CIImage *img = [CIImage imageWithBitmapData:bytes bytesPerRow:size_t(src.bytesPerLine())
                                                   size:CGSizeMake(src.width(), src.height())
                                                 format:kCIFormatBGRA8 colorSpace:space];
            const CGRect e = CGRectMake(0, 0, src.width(), src.height());
            CIImage *result = chain(img, e);
            QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
            out.fill(Qt::transparent);
            [ctx render:result toBitmap:out.bits() rowBytes:ptrdiff_t(out.bytesPerLine()) bounds:e
                 format:kCIFormatBGRA8 colorSpace:space];
            return out;
        }
    }
};

EffectProcessor::EffectProcessor()
    : d(new Impl)
{
    d->space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSDictionary *opts = @{kCIContextWorkingColorSpace: (__bridge id)d->space, kCIContextCacheIntermediates: @NO};
    d->ctx = device ? [CIContext contextWithMTLDevice:device options:opts] : [CIContext contextWithOptions:opts];
}

EffectProcessor::~EffectProcessor()
{
    CGColorSpaceRelease(d->space);
    delete d;
}

QImage EffectProcessor::apply(const QImage &frame, const QList<Effect> &effects, double local, bool transparentLayer)
{
    if (effects.isEmpty() || frame.isNull()) return frame;
    return d->run(frame, [&](CIImage *img, CGRect e) {
        for (const Effect &fx : effects) img = d->one(img, fx, local, e, transparentLayer);
        return img;
    });
}

QImage EffectProcessor::applyMoment(const QImage &canvas, const QList<Effect> &effects, double local, double progress)
{
    if (effects.isEmpty() || canvas.isNull()) return canvas;
    return d->run(canvas, [&](CIImage *img, CGRect e) {
        for (const Effect &fx : effects) img = d->moment(img, fx, local, std::clamp(progress, 0.0, 1.0), e);
        return img;
    });
}

QStringList effectNames()
{
    return {"cinematic", "vintage", "noir", "black-white", "golden-hour", "vivid", "warm", "cool", "faded",
            "dramatic", "neon", "brightness", "contrast", "saturation", "exposure", "temperature", "blur",
            "glow", "vignette", "grain", "sharpen", "rgb-split", "glitch", "vhs", "pixelate", "posterize",
            "letterbox", "light-leak", "zoom-blur", "motion-blur", "cutout", "lut"};
}

QStringList momentNames()
{
    return {"flash", "white-out", "fade-black", "shake", "zoom-punch", "rgb-split", "glitch", "blur-in", "blur-out"};
}

}  // namespace mtg
