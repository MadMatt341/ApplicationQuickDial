#include "IconLoader.h"

#include <windows.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace quickdial {
namespace {

using Microsoft::WRL::ComPtr;

struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

bool SupportedDimensions(UINT width, UINT height) {
  return width != 0 && height != 0 && width <= 8192 && height <= 8192 &&
         static_cast<std::uint64_t>(width) * height <= 16'777'216;
}

ComPtr<IWICBitmapFrameDecode> SelectFrame(IWICBitmapDecoder* decoder, UINT size) {
  GUID format{};
  UINT count = 0;
  if (FAILED(decoder->GetContainerFormat(&format)) || FAILED(decoder->GetFrameCount(&count))) {
    return {};
  }

  // Only ICO frames are alternative resolutions. Other formats retain their
  // first image rather than selecting a different animation frame or page.
  if (!IsEqualGUID(format, GUID_ContainerFormatIco)) count = std::min(count, 1U);
  ComPtr<IWICBitmapFrameDecode> best;
  UINT bestSize = 0;
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0, height = 0;
    if (FAILED(decoder->GetFrame(index, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        !SupportedDimensions(width, height)) {
      continue;
    }
    const UINT frameSize = std::max(width, height);
    // Prefer the smallest image that needs no enlargement; if none is large
    // enough, retain the largest available image. File order is irrelevant.
    if (!best || (bestSize < size && frameSize > bestSize) ||
        (frameSize >= size && frameSize < bestSize)) {
      best = std::move(frame);
      bestSize = frameSize;
    }
  }
  return best;
}

IconImage ReadPixels(IWICImagingFactory* factory, IWICBitmapSource* source, UINT size) {
  UINT width = 0, height = 0;
  if (FAILED(source->GetSize(&width, &height)) || !SupportedDimensions(width, height)) {
    return {};
  }
  // Filter premultiplied pixels so transparent colors cannot bleed into edges.
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    return {};
  }
  IWICBitmapSource* pixels = converter.Get();
  ComPtr<IWICBitmapScaler> scaler;
  if (std::max(width, height) > size) {
    const double ratio = static_cast<double>(size) / std::max(width, height);
    width = std::max(1U, static_cast<UINT>(std::lround(width * ratio)));
    height = std::max(1U, static_cast<UINT>(std::lround(height * ratio)));
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(converter.Get(), width, height, WICBitmapInterpolationModeFant))) {
      return {};
    }
    pixels = scaler.Get();
  }
  IconImage result{width, height, std::vector<std::uint8_t>(width * height * 4)};
  if (FAILED(pixels->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.pixels.size()),
                                result.pixels.data()))) {
    return {};
  }
  return result;
}

}  // namespace

IconImage LoadApplicationIcon(
    const std::wstring& target, const std::optional<std::wstring>& icon, unsigned int size) {
  Apartment apartment;
  if (FAILED(apartment.result)) return {};
  size = std::clamp(size, 1U, 96U);
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&factory)))) {
    return {};
  }
  if (icon) {
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(icon->c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder))) {
      if (const auto frame = SelectFrame(decoder.Get(), size)) {
        IconImage image = ReadPixels(factory.Get(), frame.Get(), size);
        if (!image.pixels.empty()) return image;
      }
    }
  }
  if (target.empty()) return {};
  ComPtr<IShellItem> shellItem;
  ComPtr<IShellItemImageFactory> imageFactory;
  if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&shellItem))) ||
      FAILED(shellItem.As(&imageFactory))) {
    return {};
  }
  HBITMAP bitmap = nullptr;
  const SIZE requested{static_cast<LONG>(size), static_cast<LONG>(size)};
  // Keep the Shell's native larger bitmap and downsample with WIC ourselves;
  // the default Shell resize uses a lower-quality GDI stretch.
  if (FAILED(imageFactory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) ||
      bitmap == nullptr) {
    return {};
  }
  ComPtr<IWICBitmap> source;
  // This option describes the input; it does not premultiply it. Shell images
  // contain straight alpha, which ReadPixels converts before filtering/drawing.
  const HRESULT result = factory->CreateBitmapFromHBITMAP(
      bitmap, nullptr, WICBitmapUseAlpha, &source);
  DeleteObject(bitmap);
  return SUCCEEDED(result) ? ReadPixels(factory.Get(), source.Get(), size) : IconImage{};
}

}  // namespace quickdial
