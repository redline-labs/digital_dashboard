#version 450

// The atlas holds what QPainter rasterised -- colour and antialiasing already
// baked in, premultiplied -- so the fragment stage is a sample and nothing
// else. That is deliberate: it is what makes GPU text the SAME text, rather
// than a second rendering of it that has to be argued about.
layout(location = 0) in vec2 vuv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D atlas;

void main() {
    fragColor = texture(atlas, vuv);
}
