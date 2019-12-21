uniform float opacity;
uniform bool invert_color;
uniform sampler2D tex;
uniform vec4 margin;

float min3(vec3 v) { return min(min(v.x, v.y), v.z); }
float max3(vec3 v) { return max(max(v.x, v.y), v.z); }

void main() {
    vec2 coord = vec2(gl_TexCoord[0]);
    vec4 c = texture2D(tex, coord);

    // If current window should be inverted and the pixel is inside client area
    if (invert_color
        && all(greaterThan(coord.xy, margin.st))
        && all(lessThan(coord.xy, margin.pq))
    ) {
        // Fast luminance inversion, while preserving hue
        c.rgb += 1.0 - max3(c.rgb) - min3(c.rgb);
    }

    c *= opacity;
    gl_FragColor = c;
}
