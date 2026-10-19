// Burns the sprite away where its noise is under amount, with a glowing edge of edge_color.
// Helper functions are plain HLSL. The noise is integer math, so it is the same on every GPU.
float cell_noise(float2 uv) {
    uint2 cell = uint2(uv * 8.0);
    uint h = (cell.x * 73856093u) ^ (cell.y * 19349663u);
    h = (h ^ (h >> 13u)) * 1274126177u;
    return float(h & 1023u) / 1023.0;
}

void fragment() {
    float n = cell_noise(UV);
    if (n < amount) {
        discard;
    }
    float edge = amount > 0.0 ? smoothstep(amount, amount + 0.15, n) : 1.0;
    COLOR.rgb = lerp(edge_color.rgb, COLOR.rgb, edge);
}
