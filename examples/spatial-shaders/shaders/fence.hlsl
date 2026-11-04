// Slats with gaps between them, cut out where ALPHA is under the threshold. The wind leans the
// top of the fence, and its shadow with it, as vertex() moves the corners.
void vertex() {
    VERTEX.x += sin(TIME * 3.0) * wind * (VERTEX.y + 0.4);
}

void fragment() {
    ALBEDO = float3(0.55, 0.35, 0.2) * (0.8 + 0.2 * UV.y);
    ROUGHNESS = 0.9;
    ALPHA = step(0.4, frac(UV.x * slats));
    ALPHA_SCISSOR_THRESHOLD = 0.5;
}
