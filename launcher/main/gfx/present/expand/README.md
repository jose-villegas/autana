# Expand

The half picture doubled to the panel during the send: nearest, or blended
within a surface through the seam mask. The frame overlay is replayed over
each expanded strip.

On the device, the blend between two half rows runs on the S3's PIE vector
unit (`gfx_expand_pie.c`, eight pixels a step). Host builds keep the scalar
blend. The board-only `suite_gfx_expand_pie` checks the two byte for byte.
