// Slats with gaps between them, cut out where ALPHA is under the threshold.
void fragment() {
    ALBEDO = float3(0.55, 0.35, 0.2) * (0.8 + 0.2 * UV.y);
    ROUGHNESS = 0.9;
    ALPHA = step(0.4, frac(UV.x * slats));
    ALPHA_SCISSOR_THRESHOLD = 0.5;
}
