#version 450

// PROTOTYPE: text as textured quads out of a glyph atlas.
//
// Positions arrive in SCREEN PIXELS, already rotated -- the CPU knows where
// every character goes and which way it faces (it has to, for collision), so
// the four corners are cheap to build there and the shader stays a passthrough.
// That also keeps the atlas coordinates and the placement in the same space
// QPainter used, which is what makes the output identical to the CPU blit.
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;

layout(location = 0) out vec2 vuv;

layout(std140, binding = 0) uniform buf {
    // Screen pixels -> clip. Built by OffscreenRenderer::screenToClip(), which is
    // also where the map pass starts, so the two cannot disagree about which
    // way up the frame is. Hand-rolling this from the viewport size and a sign
    // is what put the labels upside down on Vulkan: the backend's Y convention
    // is two questions, not one, and only this matrix answers both.
    mat4 clip;
};

void main() {
    vuv = uv;
    gl_Position = clip * vec4(pos, 0.0, 1.0);
}
