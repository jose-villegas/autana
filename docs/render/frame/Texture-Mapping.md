# Texture mapping

How the span fill draws a textured triangle: which texel each pixel takes,
from which mip level, and how bright. The texture's bytes are
[LTEX](../../assets/formats/LTEX.md); a mesh names its textures and u, v
through its [LUVS](../../assets/formats/LUVS.md) entry. The fill is
[`r3d_span_textured.c`](../../../launcher/main/render/texture/r3d_span_textured.c);
its texture type is
[`r3d_texture.h`](../../../launcher/main/render/texture/r3d_texture.h).

```mermaid
flowchart LR
    Tri["triangle<br/>with u, v"] --> Mip["mip level<br/>from u, v"]
    Mip --> Format{format}
    Format -- MASK1 --> Setup["set-up: light,<br/>u, v planes"]
    Setup -- MASK1 --> Cut["cut walk,<br/>affine"]
    Format -- INDEX4 --> Far{"level from the<br/>texture's flat<br/>level on?"}
    Far -- yes --> Plain["solid fill:<br/>corners' mean,<br/>no u, v or texel"]
    Far -- no --> Setup
    Setup -- INDEX4 --> Tests{"divide<br/>tests"}
    Tests -- "any fails" --> Affine["affine walk"]
    Tests -- "all hold" --> Divided["divided walk:<br/>u, v by depth<br/>every segment"]
    Cut --> Pixel["pixel: colormap<br/>at the dithered<br/>light level"]
    Affine --> Pixel
    Divided --> Pixel
```

## A pixel

A pixel's colour is one load from the texture's colormap: a row per light
level, a column per palette colour. The texture builds it at open, lit by
its tint, from black at level 0 up to `R3D_TEXTURE_LIGHT_LEVELS - 1`.

| | INDEX4 | MASK1 |
|---|---|---|
| Texel | 4 bits: a palette colour | 1 bit: kept or not |
| Pixel | the colormap at the texel's colour; a flat level, the corners' mean | texel 0 leaves the pixel; texel 1 draws palette colour 1 |
| Steps | affine, or dividing by depth | affine |

The light level is the vertex light's luminance, stepped across the triangle
in 8.8. A 4x4 Bayer threshold at the pixel's screen position dithers it
between two rows.

## Mip level

Level 0 is full size; each mip halves the last
<sup>[[126]](../../Citations.md#williams-1983)</sup>. A pixel should read the
level where one screen step moves about one texel: floor(log2 ρ), ρ the longer
of the level-0 texel steps per pixel along x and y, capped at the last level.

A triangle picks one level from the steps of its affine u and v planes,
averaged over the triangle. Where it recedes the true step grows
<sup>[[127]](../../Citations.md#heckbert-1989)</sup>, so far pixels read a
finer level than they need and alias.

![A Bistro cobblestone's mip chain at true size; below, a receding floor with each triangle tinted by the level it picked, and the pixels that read a finer level than their own step](../../images/render/texture-mip-levels.png)

The red pixels are the far part of each triangle. A per-span pick would
remove them.

## Flat levels

A level from the texture's flat level on reads no texel: the triangle goes
to the solid fill, `r3d_span_triangle_mean()`, in one colour, its corners'
mean, which is the surface's mean lit colour. It steps no u, v or light,
and never divides.

Each texture finds its flat level when it opens, from its own texels: the
first level from which every level's colours, lit at the tint, are within
an OKLab distance of 0.02 of the tint, one just noticeable
difference<sup>[[136]](../../Citations.md#w3c-css-color-4)</sup>. The tint is
the colour the flat fill draws, as the pattern averages to it. There is no
setting: a texture whose coarse levels still show their pattern keeps
them textured, and MASK1 has no flat level.

The pattern is gone there, the colour is the same:

![Bistro's street from above: the source, the street with every level's texels, with each texture's flat levels untextured, and each one's CIE76 against the source](../../images/render/bistro-street-far-levels.png)

The crops are where the two differ most: a flat triangle meeting a nearer one
at a hard edge.

![The places the flat levels change most, the source above every level's texels and the flat levels untextured](../../images/render/bistro-street-far-levels.crops.png)

## Perspective

Stepped affine, u and v bend across a receding triangle: floor rows near the
camera curve. An INDEX4 span divides u and v by depth every few pixels,
affine between <sup>[[127]](../../Citations.md#heckbert-1989)</sup>, when all
of these hold on the triangle's unclipped box, so every row slice agrees:

| Test | Setting ([Render-Settings.md](../Render-Settings.md)) |
|---|---|
| Dividing is on | `texture.divide_px` is not 0 |
| It is wider than one segment | its box's longer side exceeds `texture.divide_px` |
| It reads a fine level | its mip level is under `texture.affine_level` (0: every level); a coarser level's pattern shifts rather than bends |
| Affine would bend it too far | its estimated bend exceeds `texture.affine_bend` texels |

A dividing triangle starts from segments of `texture.divide_px` pixels. It
doubles them, up to `RENDER_TEXTURE_DIVIDE_PX_MAX`, while one segment would
still bend no more than `texture.affine_bend`.

The dividing fill and its row walk run from IRAM
([Internal-Memory-Map.md](../../memory/Internal-Memory-Map.md)).

![The near floor's squares where affine differs most from a divide every pixel, with each column's CIE76 colour distance from that divide](../../images/render/bistro-street-divide.png)

CIE76 is the distance between two colours in CIELAB.

## Related

- [LTEX](../../assets/formats/LTEX.md): the texture's bytes, its levels and
  palette, and a textured frame against its source
- [Mesh-Rendering.md](../Mesh-Rendering.md): the raster this fill is part of
- [Render-Settings.md](../Render-Settings.md): `texture.divide_px`,
  `texture.affine_bend` and `texture.affine_level`
