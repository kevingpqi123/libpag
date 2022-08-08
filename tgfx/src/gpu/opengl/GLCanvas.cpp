/////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Tencent is pleased to support the open source community by making libpag available.
//
//  Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  unless required by applicable law or agreed to in writing, software distributed under the
//  license is distributed on an "as is" basis, without warranties or conditions of any kind,
//  either express or implied. see the license for the specific language governing permissions
//  and limitations under the license.
//
/////////////////////////////////////////////////////////////////////////////////////////////////

#include "GLCanvas.h"
#include "GLFillRectOp.h"
#include "GLRRectOp.h"
#include "GLSurface.h"
#include "core/utils/MathExtra.h"
#include "gpu/AARectEffect.h"
#include "gpu/ConstColorProcessor.h"
#include "gpu/DeviceSpaceTextureEffect.h"
#include "gpu/RGBAAATextureEffect.h"
#include "gpu/opengl/GLTriangulatingPathOp.h"
#include "tgfx/core/Mask.h"
#include "tgfx/core/PathEffect.h"
#include "tgfx/core/TextBlob.h"

namespace tgfx {
static bool PaintToGLPaint(const Context* context, const Paint& paint, float alpha,
                           std::unique_ptr<FragmentProcessor>* shaderProcessor, GLPaint* glPaint) {
  FPArgs args(context);
  auto color = paint.getColor().makeOpaque();
  glPaint->colorFragmentProcessors.emplace_back(
      ConstColorProcessor::Make(color, InputMode::Ignore));
  std::unique_ptr<FragmentProcessor> shaderFP;
  if (shaderProcessor) {
    shaderFP = std::move(*shaderProcessor);
  } else if (auto shader = paint.getShader()) {
    shaderFP = shader->asFragmentProcessor(args);
    if (shaderFP == nullptr) {
      return false;
    }
  }
  if (shaderFP) {
    glPaint->colorFragmentProcessors.emplace_back(std::move(shaderFP));
  }
  if (auto colorFilter = paint.getColorFilter()) {
    if (auto processor = colorFilter->asFragmentProcessor()) {
      glPaint->colorFragmentProcessors.emplace_back(std::move(processor));
    } else {
      return false;
    }
  }
  if (auto maskFilter = paint.getMaskFilter()) {
    if (auto processor = maskFilter->asFragmentProcessor(args)) {
      glPaint->coverageFragmentProcessors.emplace_back(std::move(processor));
    }
  }
  alpha *= paint.getAlpha();
  if (alpha != 1.0f) {
    glPaint->colorFragmentProcessors.emplace_back(
        ConstColorProcessor::Make(Color{alpha, alpha, alpha, alpha}, InputMode::ModulateRGBA));
  }
  return true;
}

static bool PaintToGLPaintWithTexture(const Context* context, const Paint& paint, float alpha,
                                      std::unique_ptr<FragmentProcessor> fp,
                                      bool textureIsAlphaOnly, GLPaint* glPaint) {
  std::unique_ptr<FragmentProcessor> shaderFP;
  if (textureIsAlphaOnly) {
    if (auto shader = paint.getShader()) {
      shaderFP = shader->asFragmentProcessor(FPArgs(context));
      if (!shaderFP) {
        return false;
      }
      std::unique_ptr<FragmentProcessor> fpSeries[] = {std::move(shaderFP), std::move(fp)};
      shaderFP = FragmentProcessor::RunInSeries(fpSeries, 2);
    } else {
      shaderFP = std::move(fp);
    }
  } else {
    shaderFP = FragmentProcessor::MulChildByInputAlpha(std::move(fp));
  }
  return PaintToGLPaint(context, paint, alpha, &shaderFP, glPaint);
}

GLCanvas::GLCanvas(Surface* surface) : Canvas(surface) {
  drawContext = new GLSurfaceDrawContext(surface);
}

GLCanvas::~GLCanvas() {
  delete drawContext;
}

void GLCanvas::clear() {
  auto renderTarget = std::static_pointer_cast<GLRenderTarget>(surface->getRenderTarget());
  renderTarget->clear();
}

Texture* GLCanvas::getClipTexture() {
  if (_clipSurface == nullptr) {
    _clipSurface = Surface::Make(getContext(), surface->width(), surface->height(), true);
    if (_clipSurface == nullptr) {
      _clipSurface = Surface::Make(getContext(), surface->width(), surface->height());
    }
  }
  if (_clipSurface == nullptr) {
    return nullptr;
  }
  if (clipID != state->clipID) {
    auto clipCanvas = _clipSurface->getCanvas();
    clipCanvas->clear();
    Paint paint = {};
    paint.setColor(Color::Black());
    clipCanvas->drawPath(state->clip, paint);
    clipID = state->clipID;
  }
  return _clipSurface->getTexture().get();
}

static constexpr float BOUNDS_TO_LERANCE = 1e-3f;

/**
 * Returns true if the given rect counts as aligned with pixel boundaries.
 */
static bool IsPixelAligned(const Rect& rect) {
  return fabsf(roundf(rect.left) - rect.left) <= BOUNDS_TO_LERANCE &&
         fabsf(roundf(rect.top) - rect.top) <= BOUNDS_TO_LERANCE &&
         fabsf(roundf(rect.right) - rect.right) <= BOUNDS_TO_LERANCE &&
         fabsf(roundf(rect.bottom) - rect.bottom) <= BOUNDS_TO_LERANCE;
}

std::unique_ptr<FragmentProcessor> GLCanvas::getClipMask(const Rect& deviceBounds,
                                                         Rect* scissorRect) {
  const auto& clipPath = state->clip;
  if (clipPath.contains(deviceBounds)) {
    return nullptr;
  }
  auto rect = Rect::MakeEmpty();
  if (clipPath.asRect(&rect)) {
    if (surface->origin() == ImageOrigin::BottomLeft) {
      auto height = rect.height();
      rect.top = static_cast<float>(surface->height()) - rect.bottom;
      rect.bottom = rect.top + height;
    }
    if (IsPixelAligned(rect) && scissorRect) {
      *scissorRect = rect;
      scissorRect->round();
      return nullptr;
    } else {
      return AARectEffect::Make(rect);
    }
  } else {
    return FragmentProcessor::MulInputByChildAlpha(
        DeviceSpaceTextureEffect::Make(getClipTexture(), surface->origin()));
  }
}

Rect GLCanvas::clipLocalBounds(Rect localBounds) {
  auto deviceBounds = state->matrix.mapRect(localBounds);
  auto clipBounds = state->clip.getBounds();
  clipBounds.roundOut();
  auto clippedDeviceBounds = deviceBounds;
  if (!clippedDeviceBounds.intersect(clipBounds)) {
    return Rect::MakeEmpty();
  }
  auto clippedLocalBounds = localBounds;
  if (state->matrix.getSkewX() == 0 && state->matrix.getSkewY() == 0 &&
      clippedDeviceBounds != deviceBounds) {
    Matrix inverse = Matrix::I();
    state->matrix.invert(&inverse);
    clippedLocalBounds = inverse.mapRect(clippedDeviceBounds);
  }
  return clippedLocalBounds;
}

void GLCanvas::drawTexture(const Texture* texture, const RGBAAALayout* layout, const Paint& paint) {
  if (texture == nullptr) {
    return;
  }
  auto width = static_cast<float>(layout ? layout->width : texture->width());
  auto height = static_cast<float>(layout ? layout->height : texture->height());
  auto localBounds = clipLocalBounds(Rect::MakeWH(width, height));
  if (localBounds.isEmpty()) {
    return;
  }
  auto processor = RGBAAATextureEffect::Make(texture, Matrix::I(), layout);
  if (processor == nullptr) {
    return;
  }
  GLPaint glPaint;
  if (!PaintToGLPaintWithTexture(getContext(), paint, state->alpha, std::move(processor),
                                 texture->getSampler()->format == PixelFormat::ALPHA_8, &glPaint)) {
    return;
  }
  auto localMatrix = Matrix::I();
  localMatrix.postScale(localBounds.width(), localBounds.height());
  localMatrix.postTranslate(localBounds.x(), localBounds.y());
  draw(GLFillRectOp::Make(localBounds, state->matrix, localMatrix), std::move(glPaint), true);
}

void GLCanvas::drawPath(const Path& path, const Paint& paint) {
  if (paint.getStyle() == PaintStyle::Fill) {
    fillPath(path, paint);
    return;
  }
  auto strokePath = path;
  auto strokeEffect = PathEffect::MakeStroke(*paint.getStroke());
  if (strokeEffect) {
    strokeEffect->applyTo(&strokePath);
  }
  fillPath(strokePath, paint);
}

static std::unique_ptr<GLDrawOp> MakeSimplePathOp(const Path& path, const Matrix& viewMatrix) {
  auto rect = Rect::MakeEmpty();
  if (path.asRect(&rect)) {
    auto localMatrix = Matrix::MakeScale(rect.width(), rect.height());
    localMatrix.postTranslate(rect.x(), rect.y());
    return GLFillRectOp::Make(rect, viewMatrix, localMatrix);
  }
  RRect rRect;
  if (path.asRRect(&rRect)) {
    auto localMatrix = Matrix::MakeScale(rRect.rect.width(), rRect.rect.height());
    localMatrix.postTranslate(rRect.rect.x(), rRect.rect.y());
    return GLRRectOp::Make(rRect, viewMatrix, localMatrix);
  }
  return nullptr;
}

void GLCanvas::fillPath(const Path& path, const Paint& paint) {
  if (path.isEmpty()) {
    return;
  }
  auto bounds = path.getBounds();
  auto localBounds = clipLocalBounds(bounds);
  if (localBounds.isEmpty()) {
    return;
  }
  auto op = MakeSimplePathOp(path, state->matrix);
  if (op) {
    GLPaint glPaint;
    if (!PaintToGLPaint(getContext(), paint, state->alpha, nullptr, &glPaint)) {
      return;
    }
    draw(std::move(op), std::move(glPaint));
    return;
  }
  auto localMatrix = Matrix::I();
  if (!state->matrix.invert(&localMatrix)) {
    return;
  }
  auto tempPath = path;
  tempPath.transform(state->matrix);
  op = GLTriangulatingPathOp::Make(tempPath, state->clip.getBounds(), localMatrix);
  if (op) {
    save();
    resetMatrix();
    GLPaint glPaint;
    if (!PaintToGLPaint(getContext(), paint, state->alpha, nullptr, &glPaint)) {
      return;
    }
    draw(std::move(op), std::move(glPaint));
    restore();
    return;
  }
  auto deviceBounds = state->matrix.mapRect(localBounds);
  auto width = ceilf(deviceBounds.width());
  auto height = ceilf(deviceBounds.height());
  auto mask = Mask::Make(static_cast<int>(width), static_cast<int>(height));
  if (!mask) {
    return;
  }
  auto totalMatrix = state->matrix;
  auto matrix = Matrix::MakeTrans(-deviceBounds.x(), -deviceBounds.y());
  matrix.postScale(width / deviceBounds.width(), height / deviceBounds.height());
  totalMatrix.postConcat(matrix);
  mask->setMatrix(totalMatrix);
  mask->fillPath(path);
  auto maskTexture = mask->makeTexture(getContext());
  drawMask(deviceBounds, maskTexture.get(), paint);
}

void GLCanvas::drawMask(const Rect& bounds, const Texture* mask, const Paint& paint) {
  if (mask == nullptr) {
    return;
  }
  auto localMatrix = Matrix::I();
  localMatrix.postScale(bounds.width(), bounds.height());
  localMatrix.postTranslate(bounds.x(), bounds.y());
  auto maskLocalMatrix = Matrix::I();
  auto invert = Matrix::I();
  if (!state->matrix.invert(&invert)) {
    return;
  }
  localMatrix.postConcat(invert);
  if (!localMatrix.invert(&invert)) {
    return;
  }
  maskLocalMatrix.postConcat(invert);
  maskLocalMatrix.postScale(static_cast<float>(mask->width()), static_cast<float>(mask->height()));
  auto oldMatrix = state->matrix;
  resetMatrix();
  GLPaint glPaint;
  if (!PaintToGLPaint(getContext(), paint, state->alpha, nullptr, &glPaint)) {
    return;
  }
  glPaint.coverageFragmentProcessors.emplace_back(
      FragmentProcessor::MulInputByChildAlpha(RGBAAATextureEffect::Make(mask, maskLocalMatrix)));
  draw(GLFillRectOp::Make(bounds, state->matrix, localMatrix), std::move(glPaint));
  setMatrix(oldMatrix);
}

void GLCanvas::drawGlyphs(const GlyphID glyphIDs[], const Point positions[], size_t glyphCount,
                          const Font& font, const Paint& paint) {
  auto scaleX = state->matrix.getScaleX();
  auto skewY = state->matrix.getSkewY();
  auto scale = std::sqrt(scaleX * scaleX + skewY * skewY);
  auto scaledFont = font.makeWithSize(font.getSize() * scale);
  auto scaledPaint = paint;
  scaledPaint.setStrokeWidth(paint.getStrokeWidth() * scale);
  std::vector<Point> scaledPositions;
  for (size_t i = 0; i < glyphCount; ++i) {
    scaledPositions.push_back(Point::Make(positions[i].x * scale, positions[i].y * scale));
  }
  save();
  concat(Matrix::MakeScale(1.f / scale));
  if (scaledFont.getTypeface()->hasColor()) {
    drawColorGlyphs(glyphIDs, &scaledPositions[0], glyphCount, scaledFont, scaledPaint);
    restore();
    return;
  }
  auto textBlob = TextBlob::MakeFrom(glyphIDs, &scaledPositions[0], glyphCount, scaledFont);
  if (textBlob == nullptr) {
    restore();
    return;
  }
  Path path = {};
  auto stroke = scaledPaint.getStyle() == PaintStyle::Stroke ? scaledPaint.getStroke() : nullptr;
  if (textBlob->getPath(&path, stroke)) {
    fillPath(path, scaledPaint);
    restore();
    return;
  }
  drawMaskGlyphs(textBlob.get(), scaledPaint);
  restore();
}

void GLCanvas::drawColorGlyphs(const GlyphID glyphIDs[], const Point positions[], size_t glyphCount,
                               const Font& font, const Paint& paint) {
  for (size_t i = 0; i < glyphCount; ++i) {
    const auto& glyphID = glyphIDs[i];
    const auto& position = positions[i];

    auto glyphMatrix = Matrix::I();
    auto glyphBuffer = font.getGlyphImage(glyphID, &glyphMatrix);
    if (glyphBuffer == nullptr) {
      continue;
    }
    glyphMatrix.postTranslate(position.x, position.y);
    save();
    concat(glyphMatrix);
    auto texture = glyphBuffer->makeTexture(getContext());
    Canvas::drawTexture(texture.get(), &paint);
    restore();
  }
}

void GLCanvas::drawMaskGlyphs(TextBlob* textBlob, const Paint& paint) {
  if (textBlob == nullptr) {
    return;
  }
  auto stroke = paint.getStyle() == PaintStyle::Stroke ? paint.getStroke() : nullptr;
  auto localBounds = clipLocalBounds(textBlob->getBounds(stroke));
  if (localBounds.isEmpty()) {
    return;
  }
  auto deviceBounds = state->matrix.mapRect(localBounds);
  auto width = ceilf(deviceBounds.width());
  auto height = ceilf(deviceBounds.height());
  auto mask = Mask::Make(static_cast<int>(width), static_cast<int>(height));
  if (mask == nullptr) {
    return;
  }
  auto totalMatrix = state->matrix;
  auto matrix = Matrix::I();
  matrix.postTranslate(-deviceBounds.x(), -deviceBounds.y());
  matrix.postScale(width / deviceBounds.width(), height / deviceBounds.height());
  totalMatrix.postConcat(matrix);
  mask->setMatrix(totalMatrix);
  if (paint.getStyle() == PaintStyle::Stroke) {
    if (stroke) {
      mask->strokeText(textBlob, *stroke);
    }
  } else {
    mask->fillText(textBlob);
  }
  auto texture = mask->makeTexture(getContext());
  drawMask(deviceBounds, texture.get(), paint);
}

void GLCanvas::drawAtlas(const Texture* atlas, const Matrix matrix[], const Rect tex[],
                         const Color colors[], size_t count) {
  if (atlas == nullptr || count == 0) {
    return;
  }
  auto totalMatrix = getMatrix();
  std::vector<Rect> rects;
  std::vector<Matrix> matrices;
  std::vector<Matrix> localMatrices;
  std::vector<Color> colorVector;
  for (size_t i = 0; i < count; ++i) {
    concat(matrix[i]);
    auto width = static_cast<float>(tex[i].width());
    auto height = static_cast<float>(tex[i].height());
    auto localBounds = clipLocalBounds(Rect::MakeWH(width, height));
    if (localBounds.isEmpty()) {
      setMatrix(totalMatrix);
      continue;
    }
    rects.push_back(localBounds);
    matrices.push_back(state->matrix);
    auto localMatrix = Matrix::I();
    localMatrix.postScale(localBounds.width(), localBounds.height());
    localMatrix.postTranslate(tex[i].x() + localBounds.x(), tex[i].y() + localBounds.y());
    localMatrices.push_back(localMatrix);
    if (colors) {
      colorVector.push_back(colors[i]);
    }
    setMatrix(totalMatrix);
  }
  if (rects.empty()) {
    return;
  }
  GLPaint glPaint;
  if (colors) {
    glPaint.coverageFragmentProcessors.emplace_back(
        FragmentProcessor::MulInputByChildAlpha(RGBAAATextureEffect::Make(atlas)));
  } else {
    glPaint.colorFragmentProcessors.emplace_back(RGBAAATextureEffect::Make(atlas));
  }
  draw(GLFillRectOp::Make(rects, matrices, localMatrices, colorVector), std::move(glPaint), false);
}

void GLCanvas::drawMesh(const Mesh* mesh, const Paint& paint) {
  if (mesh == nullptr) {
    return;
  }
  auto bounds = mesh->bounds();
  if (!state->matrix.isIdentity()) {
    state->matrix.mapRect(&bounds);
  }
  auto clipBounds = state->clip.getBounds();
  clipBounds.roundOut();
  if (!clipBounds.intersect(bounds)) {
    return;
  }
  auto op = mesh->getOp(state->matrix);
  if (op == nullptr) {
    return;
  }
  auto oldMatrix = state->matrix;
  resetMatrix();
  GLPaint glPaint;
  if (!PaintToGLPaint(getContext(), paint, state->alpha, nullptr, &glPaint)) {
    return;
  }
  draw(std::move(op), std::move(glPaint));
  setMatrix(oldMatrix);
}

void GLCanvas::draw(std::unique_ptr<GLDrawOp> op, GLPaint paint, bool aa) {
  if (drawContext == nullptr) {
    return;
  }
  auto renderTarget = surface->getRenderTarget();
  auto aaType = AAType::None;
  if (renderTarget->sampleCount() > 1) {
    aaType = AAType::MSAA;
  } else if (aa && !IsPixelAligned(op->bounds())) {
    aaType = AAType::Coverage;
  } else {
    const auto& matrix = state->matrix;
    auto rotation = std::round(RadiansToDegrees(atan2f(matrix.getSkewX(), matrix.getScaleX())));
    if (static_cast<int>(rotation) % 90 != 0) {
      aaType = AAType::Coverage;
    }
  }
  DrawArgs args;
  args.colors = std::move(paint.colorFragmentProcessors);
  args.masks = std::move(paint.coverageFragmentProcessors);
  auto clipMask = getClipMask(op->bounds(), &args.scissorRect);
  if (clipMask) {
    args.masks.push_back(std::move(clipMask));
  }
  args.context = surface->getContext();
  args.blendMode = state->blendMode;
  args.renderTarget = renderTarget.get();
  args.renderTargetTexture = surface->getTexture();
  args.aa = aaType;
  drawContext->draw(std::move(args), std::move(op));
}
}  // namespace tgfx
