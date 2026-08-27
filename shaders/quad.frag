#version 450

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 3) in vec2 vUv;
layout(location = 4) in vec4 vClip;
layout(location = 5) in vec2 vSize;

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D atlas;

float roundedAlpha(vec2 point, vec2 halfSize, float radius)
{
    vec2 q = abs(point) - (halfSize - vec2(radius));
    float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    return 1.0 - smoothstep(-1.0, 0.5, distance);
}

void main()
{
    if (gl_FragCoord.x < vClip.x || gl_FragCoord.x > vClip.z
        || gl_FragCoord.y < vClip.y || gl_FragCoord.y > vClip.w) {
        discard;
    }

    if (vParams.x < 0.5) {
        float radius = vParams.y;
        float alpha = radius > 0.0
            ? roundedAlpha(vLocal - vSize * 0.5, vSize * 0.5, radius)
            : 1.0;
        fragColor = vec4(vColor.rgb, vColor.a * alpha);
        return;
    }

    float coverage = texture(atlas, vUv).r;
    fragColor = vec4(vColor.rgb, vColor.a * coverage);
}
