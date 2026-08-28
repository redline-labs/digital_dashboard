// SPDX-License-Identifier: GPL-3.0-or-later
//
// One placed character, on its way to the GPU.
//
// A header of its own because it is the seam between two halves that otherwise
// know nothing about each other: map/labels.h decides where text goes and owns
// the atlas it is rasterised into, and map/gpu_renderer.h draws quads and has
// never heard of a font. Neither has to include the other.
#ifndef MAP_TEXT_QUAD_H
#define MAP_TEXT_QUAD_H

#include <QPointF>
#include <QRectF>

namespace map_render
{

struct TextQuad
{
    // Clockwise from the character's top left, in DEVICE pixels, already
    // turned to lie along whatever it is labelling. The rotation is applied
    // here rather than in the shader because the label pass has to know the
    // corners anyway -- that is what it collides.
    QPointF corners[4];
    // Where this character sits in the atlas page, in texture coordinates.
    QRectF uv;

    friend bool operator==(const TextQuad&, const TextQuad&) = default;
};

} // namespace map_render

#endif // MAP_TEXT_QUAD_H
