// A tuft of grass that leans with the wind. WIND is the wind at the mesh's origin, in meters per
// second in the world, so the tips move further the harder it blows and the bases stay put.
void vertex() {
    float height = saturate(VERTEX.y + 0.6);
    VERTEX.xz += WIND.xz * bend * height * height;
}

void fragment() {
    ALBEDO = lerp(float3(0.12, 0.3, 0.08), float3(0.55, 0.75, 0.3), UV.y < 0.5 ? 1.0 : 0.3);
    ROUGHNESS = 0.8;
}
