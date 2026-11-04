// A crystal: tinted and glossy, its edges glowing brighter and dimmer with the time.
void fragment() {
    ALBEDO = tint.rgb;
    METALLIC = 0.2;
    ROUGHNESS = 0.15;
    // NORMAL and VIEW are in view space, as Godot's: the rim is where the surface turns away.
    float rim = pow(1.0 - saturate(dot(NORMAL, VIEW)), 2.0);
    float pulse = 0.5 + 0.5 * sin(TIME * speed);
    EMISSION = glow.rgb * rim * pulse;
}
