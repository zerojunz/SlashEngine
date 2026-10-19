// Sways the sprite's top like a flag in the wind, and tints it.
void vertex() {
    // UV.y is 0 at the top, so the top corners move and the bottom ones stay.
    VERTEX.x += sin(TIME * speed) * amplitude * (1.0 - UV.y);
}

void fragment() {
    COLOR *= tint;
}
