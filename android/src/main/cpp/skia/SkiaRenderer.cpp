//
// Created by Marc Rousavy on 10.08.23.
//

#if VISION_CAMERA_ENABLE_SKIA

#include "SkiaRenderer.h"
#include <android/log.h>
#include "OpenGLError.h"

#include <GLES2/gl2ext.h>

#include <core/SkColorSpace.h>
#include <core/SkCanvas.h>
#include <core/SkYUVAPixmaps.h>

#include <gpu/gl/GrGLInterface.h>
#include <gpu/GrDirectContext.h>
#include <gpu/GrBackendSurface.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <gpu/ganesh/SkImageGanesh.h>

#include <android/native_window_jni.h>
#include <android/surface_texture_jni.h>

// from <gpu/ganesh/gl/GrGLDefines.h>
#define GR_GL_RGBA8 0x8058
#define DEFAULT_FBO 0

namespace vision {

jni::local_ref<SkiaRenderer::jhybriddata> SkiaRenderer::initHybrid(jni::alias_ref<jhybridobject> javaPart) {
  return makeCxxInstance(javaPart);
}

SkiaRenderer::SkiaRenderer(const jni::alias_ref<jhybridobject>& javaPart) {
  _javaPart = jni::make_global(javaPart);
}

SkiaRenderer::~SkiaRenderer() {
  _offscreenSurface = nullptr;
  _offscreenSurfaceTextureId = NO_TEXTURE;

  // 3. Delete the Skia context
  if (_skiaContext != nullptr) {
    _skiaContext->abandonContext();
    _skiaContext = nullptr;
  }
}

sk_sp<GrDirectContext> SkiaRenderer::getSkiaContext() {
  if (_skiaContext == nullptr) {
    GrContextOptions options;
    // TODO: Set this to true or not? idk
    options.fDisableGpuYUVConversion = false;
    _skiaContext = GrDirectContext::MakeGL(options);
  }
  return _skiaContext;
}

OpenGLTexture SkiaRenderer::renderFrame(OpenGLContext& glContext, OpenGLTexture& texture) {
  // 1. Activate the OpenGL context (eglMakeCurrent)
  glContext.use();

  // 2. Initialize Skia
  auto skiaContext = getSkiaContext();
  // TODO: use this later kRenderTarget_GrGLBackendState | kTextureBinding_GrGLBackendState
  skiaContext->resetContext();

  // 3. Create the offscreen Skia Surface
  if (_offscreenSurface == nullptr) {
    GrBackendTexture skiaTex = skiaContext->createBackendTexture(texture.width,
                                                                 texture.height,
                                                                 SkColorType::kN32_SkColorType,
                                                                 GrMipMapped::kNo,
                                                                 GrRenderable::kYes);
    GrGLTextureInfo info;
    skiaTex.getGLTextureInfo(&info);
    _offscreenSurfaceTextureId = info.fID;

    SkSurfaceProps props(0, kUnknown_SkPixelGeometry);
    _offscreenSurface = SkSurfaces::WrapBackendTexture(skiaContext.get(),
                                                       skiaTex,
                                                       kBottomLeft_GrSurfaceOrigin,
                                                       0,
                                                       SkColorType::kN32_SkColorType,
                                                       nullptr,
                                                       &props,
                                                       // TODO: Delete texture!
                                                       nullptr);
  }

  GrGLTextureInfo textureInfo {
      // OpenGL will automatically convert YUV -> RGB because it's an EXTERNAL texture
      .fTarget = texture.target,
      .fID = texture.id,
      .fFormat = GR_GL_RGBA8,
      .fProtected = skgpu::Protected::kNo,
  };
  GrBackendTexture skiaTexture(texture.width,
                               texture.height,
                               GrMipMapped::kNo,
                               textureInfo);
  sk_sp<SkImage> frame = SkImages::BorrowTextureFrom(skiaContext.get(),
                                                     skiaTexture,
                                                     kBottomLeft_GrSurfaceOrigin,
                                                     kN32_SkColorType,
                                                     kOpaque_SkAlphaType,
                                                     nullptr,
                                                     nullptr);


  SkCanvas* canvas = _offscreenSurface->getCanvas();

  canvas->clear(SkColors::kCyan);

  auto duration = std::chrono::system_clock::now().time_since_epoch();
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

  canvas->drawImage(frame, 0, 0);

  // TODO: Run Skia Frame Processor
  SkRect rect = SkRect::MakeXYWH(150, 250, millis % 3000 / 10, millis % 3000 / 10);
  SkPaint paint;
  paint.setColor(SkColors::kGreen);
  canvas->drawRect(rect, paint);

  _offscreenSurface->flushAndSubmit();

  return OpenGLTexture {
      .id = _offscreenSurfaceTextureId,
      .target = GL_TEXTURE_2D,
      .width = texture.width,
      .height = texture.height,
  };
}

void SkiaRenderer::renderTextureToSurface(OpenGLContext &glContext, OpenGLTexture &texture, EGLSurface surface) {
  // 1. Activate the OpenGL context (eglMakeCurrent)
  glContext.use(surface);

  // 2. Initialize Skia
  auto skiaContext = getSkiaContext();

  // 3. Wrap the output EGLSurface in a Skia SkSurface
  GLint samples;
  glGetIntegerv(GL_SAMPLES, &samples);
  GLint stencil;
  glGetIntegerv(GL_STENCIL_BITS, &stencil);
  GrGLFramebufferInfo fboInfo {
      .fFBOID = DEFAULT_FBO,
      .fFormat = GR_GL_RGBA8
  };
  GrBackendRenderTarget renderTarget(texture.width,
                                     texture.height,
                                     samples,
                                     stencil,
                                     fboInfo);
  SkSurfaceProps props(0, kUnknown_SkPixelGeometry);
  sk_sp<SkSurface> skSurface = SkSurfaces::WrapBackendRenderTarget(_skiaContext.get(),
                                                                   renderTarget,
                                                                   kBottomLeft_GrSurfaceOrigin,
                                                                   kN32_SkColorType,
                                                                   nullptr,
                                                                   &props,
                                                                   nullptr,
                                                                   nullptr);
  if (skSurface == nullptr) {
    [[unlikely]];
    throw std::runtime_error("Failed to create Skia Surface! Cannot wrap EGLSurface/FBO0 using Skia.");
  }

  // 4. Wrap the input texture in a Skia SkImage
  GrGLTextureInfo textureInfo {
      // OpenGL will automatically convert YUV -> RGB - if it's an EXTERNAL texture
      .fTarget = texture.target,
      .fID = texture.id,
      .fFormat = GR_GL_RGBA8,
  };
  GrBackendTexture skiaTexture(texture.width,
                               texture.height,
                               GrMipMapped::kNo,
                               textureInfo);
  sk_sp<SkImage> frame = SkImages::BorrowTextureFrom(_skiaContext.get(),
                                                     skiaTexture,
                                                     kBottomLeft_GrSurfaceOrigin,
                                                     kN32_SkColorType,
                                                     kOpaque_SkAlphaType,
                                                     nullptr,
                                                     nullptr);
  if (frame == nullptr) {
    [[unlikely]];
    throw std::runtime_error("Failed to create Skia Image! Cannot wrap input texture (frame) using Skia.");
  }

  // 5. Prepare the Canvas!
  SkCanvas* canvas = skSurface->getCanvas();
  if (canvas == nullptr) {
    [[unlikely]];
    throw std::runtime_error("Failed to get Skia Canvas!");
  }

  // 6. Render it!
  canvas->clear(SkColors::kBlack);
  canvas->drawImage(frame, 0, 0);

  // 7. Flush all Skia operations to OpenGL
  skSurface->flushAndSubmit();

  // 8. Swap the buffers so the onscreen surface gets updated.
  glContext.flush();
}

void SkiaRenderer::registerNatives() {
  registerHybrid({
     makeNativeMethod("initHybrid", SkiaRenderer::initHybrid),
  });
}

} // namespace vision

#endif
