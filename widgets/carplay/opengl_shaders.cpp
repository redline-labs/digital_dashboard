#include "carplay/opengl_shaders.h"

// Simplified shaders
const char* vertexShaderSource = R"(
#version 120
attribute vec2 aPos;
attribute vec2 aTexCoord;
varying vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

const char* fragmentShaderSource = R"(
#version 120
varying vec2 TexCoord;
uniform sampler2D textureY;
uniform sampler2D textureU;
uniform sampler2D textureV;
void main() {
    float y = texture2D(textureY, TexCoord).r;
    float u = texture2D(textureU, TexCoord).r - 0.5;
    float v = texture2D(textureV, TexCoord).r - 0.5;
    vec3 rgb;
    rgb.r = y + 1.402 * v;
    rgb.g = y - 0.344136 * u - 0.714136 * v;
    rgb.b = y + 1.772 * u;
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";