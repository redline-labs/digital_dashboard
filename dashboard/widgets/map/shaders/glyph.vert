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
    // Viewport in device pixels. Screen pixels -> clip is the whole transform.
    vec2 viewport;
    // 1.0 when the framebuffer's Y runs down the screen, -1.0 when it runs up.
    // Same question the map pass answers with its ortho box.
    float yFlip;
    float pad;
};

void main() {
    vuv = uv;
    vec2 ndc = vec2(pos.x / viewport.x * 2.0 - 1.0,
                    (pos.y / viewport.y * 2.0 - 1.0) * yFlip);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
