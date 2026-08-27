#version 450

layout(location = 0) in vec2 corner;
layout(location = 1) in vec4 bounds;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 params;
layout(location = 4) in vec4 uvRect;
layout(location = 5) in vec4 clip;

layout(push_constant) uniform Push {
    vec2 viewport;
} push;

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vParams;
layout(location = 3) out vec2 vUv;
layout(location = 4) out vec4 vClip;
layout(location = 5) out vec2 vSize;

void main()
{
    vec2 position = bounds.xy + corner * bounds.zw;
    vec2 normalized = position / push.viewport * 2.0 - 1.0;
    gl_Position = vec4(normalized, 0.0, 1.0);
    vLocal = corner * bounds.zw;
    vSize = bounds.zw;
    vColor = color;
    vParams = params;
    vUv = mix(uvRect.xy, uvRect.zw, corner);
    vClip = clip;
}
