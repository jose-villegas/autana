/* Clamp a pixel coordinate to its upper bound. */
static int
clamp_px(int value, int max_px) {
    if (value > max_px) {
        return max_px;
    }
    return value;
}
