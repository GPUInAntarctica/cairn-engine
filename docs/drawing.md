# Drawing assets with an agent

`cairn` exposes the editor's layered raster core as `image.*` commands: layers,
brushes, shapes, transforms, composition, seeded noise, gradients and palettes.
A person draws those with a mouse; an agent has to draw them **without ever seeing
the PNG**. So the surface carries its own eyes — `image.preview` prints the
picture as text and `image.info` prints numbers about it — and the workflow is a
loop:

```
draw  ->  look  ->  fix  ->  confirm
```

This chapter is that loop, worked twice: a sprite (a 32×32 coin with top and
bottom highlights) and a texture (a 32×32 seamless grass tile). Every command
below was run against the build in this repository, and every output shown is the
real one. The test suite replays both recipes (`cli: drawing tutorial` in
[`test/cli.js`](../test/cli.js)), so the chapter cannot drift from the
implementation. See also the [`image.*` reference](cli.md#image) for the full
argument list of each command.

## The two eyes

**`image.preview`** renders the document, or one layer, as a text grid. `--scale`
averages that many pixels into one cell, `--format` picks the representation
(`ascii` luminance ramp, `blocks` Unicode density ramp, `hex` one `#rrggbb` per
cell), `--empty` is the character for a fully transparent cell and `--chars`
replaces the ramp. Start at `--scale 1` for a 32×32 target: one character per
pixel, which is exactly the resolution you are authoring at.

```bash
cairn --project ./game image.preview --scale 1
```

```text
                                
                                
            --------            
          --########--          
        --############--        
       -################-       
      -##################-      
     -####################-     
    -#######%%%%%%%%#######-    
    -####%%%%%%%%%%%%%%####-    
   -####%%%%%%%%%%%%%%%%####-   
   -####%%%%%%%%%%%%%%%%####-   
  -######%%%%%%%%%%%%%%######-  
  -########=%%%%%%%%=########-  
  -########==========########-  
  -#########========#########-  
  -#########========#########-  
  -########==========########-  
  -########=%%%%%%%%=########-  
  -######%%%%%%%%%%%%%%######-  
   -####%%%%%%%%%%%%%%%%####-   
   -####%%%%%%%%%%%%%%%%####-   
    -####%%%%%%%%%%%%%%####-    
    -#######%%%%%%%%#######-    
     -####################-     
      -##################-      
       -################-       
        --############--        
          --########--          
            --------            
                                
                                
```

That is the finished coin, exactly as printed (rows 0–1 and 30–31 are empty). `#`
is the gold body, `%` the brighter highlight, `=` the darker emblem inside it, `-`
the rim, and spaces are transparency. You can see the shape, the highlight band
and the emblem without decoding a single byte.

**`image.info`** is the numeric half: dimensions and layers, then `stats` with
the non-transparent bounding box, coverage (the fraction of the canvas that is
opaque), the top colours (each channel quantised to 4 bits, so `#ffcc44` is a
bucket and not an exact pixel), whether content touches each canvas edge (i.e. is
being clipped), left/right and top/bottom mirror symmetry with mismatch counts,
and, with `--spans`, the content start/end of every row and column.

**`image.pixel --x --y`** samples one coordinate (the composite, or `--layer N`).
Reach for it once you have *seen* something: it turns "the top edge looks wrong"
into a number you can assert against.

## Recipe 1 — a 32×32 coin with top and bottom highlights

Work in a batch. A single `cairn image.*` call is one process and one session, so
a picture built from more than two or three steps belongs in an `exec.run` file
([why](cli.md#recipes)). Write `coin.jsonl`:

```jsonl
{"command":"image.new","args":{"width":32,"height":32,"background":"transparent"}}
{"command":"image.draw.ellipse","args":{"cx":16,"cy":16,"rx":13,"ry":13,"filled":true,"stroke":1,"color":"#f2c14e","strokeColor":"#8a5a1a"}}
{"command":"image.layer.add","args":{"name":"Gloss"}}
{"command":"image.draw.rect","args":{"x":6,"y":7,"width":20,"height":4,"filled":true,"color":"#fff0b3","opacity":0.85}}
{"command":"image.preview","args":{"scale":1}}
{"command":"image.pixel","args":{"x":16,"y":7}}
{"command":"image.info","args":{}}
```

```bash
cairn --project ./game exec.run --file coin.jsonl
```

The disc is one call: `--filled --stroke 1` fills the ellipse *and* outlines it on
the **outside**, so the fill is never eaten by its own rim. The highlight is on
its own layer, so it can be redrawn without touching the body. Then look — rows 6
to 13, which is where the top of the disc is:

```text
      -##################-      
     -%%%%%%%%%%%%%%%%%%%%-     
    -#%%%%%%%%%%%%%%%%%%%%#-    
    -#%%%%%%%%%%%%%%%%%%%%#-    
   -##%%%%%%%%%%%%%%%%%%%%##-   
   -########################-   
  -##########################-  
  -##########################-  
```

Rows 7 to 10 are one solid highlight band running the full width of the coin, and
it has flattened the top of the disc: the curved rim is gone above the middle.
`image.pixel --x 16 --y 7` confirms it — the pixel is `#fde9a4` (the highlight),
not `#f2c14e` (the body). And `image.info` puts a number on what you can see:

```json
{"mirrorTopBottom": false, "mirrorTopBottomMismatch": 20,
 "mirrorLeftRight": true, "mirrorLeftRightMismatch": 0}
```

A single highlight at the top is not symmetric about the horizontal centre line —
20 pixels mismatch. That is the diagnosis, and the fix is two things: the
highlight has to sit *inside* the disc's curve, and it has to be drawn twice, as a
pair. Both are one command, because `--mirror y` reflects a draw call's geometry
about the canvas centre line and paints the reflection in the same call:

```jsonl
{"command":"image.layer.delete","args":{"index":1}}
{"command":"image.layer.add","args":{"name":"Gloss"}}
{"command":"image.draw.ellipse","args":{"cx":16,"cy":11,"rx":8,"ry":3,"filled":true,"color":"#fff0b3","opacity":0.85,"mirror":"y"}}
{"command":"image.pixel","args":{"x":16,"y":7}}
{"command":"image.info","args":{}}
{"command":"image.draw.path","args":{"sides":6,"cx":16,"cy":16,"radius":7,"inner":0.5,"filled":true,"layer":0,"color":"#a8701c"}}
{"command":"image.preview","args":{"scale":1}}
{"command":"image.info","args":{}}
{"command":"image.save","args":{"out":"Assets/Sprites/coin.png"}}
```

Append those steps to the same file and run it: the session state (the document
and its layers) is what a batch carries from step to step, so the deletion, the
re-draw and the emblem all act on the coin that is already painted. The check
now reads:

```json
{"mirrorTopBottom": true, "mirrorTopBottomMismatch": 0,
 "mirrorLeftRight": true, "mirrorLeftRightMismatch": 0}
```

`mirrorLeftRight` was already true and stays true — the diagnosis was correct and
specific. `image.pixel --x 16 --y 7` is back to `#f2c14e`: the top of the disc is
body again, because the highlight ellipse (radius 8×3, centred at row 11) stays
below the rim, and its reflection sits at row 21 to make the bottom highlight.
The last steps add the emblem as a six-point star on the body layer and confirm
the numbers again. Prefer an odd-sided star — five points, the classic coin
emblem — and `mirrorLeftRight` will go false, because a five-pointed star is not
mirror-symmetric. That is a *check*, not a rule: `image.info` reports symmetry so
you can decide whether the asymmetry is one you meant.

`image.save --out Assets/Sprites/coin.png` composites the layers and writes the
PNG (creating `Assets/Sprites/` if it is missing). 32×32 RGBA, 237 bytes, and no
mouse was involved.

## Recipe 2 — a 32×32 seamless grass tile

A tile has one extra requirement: the right edge column has to continue into the
left edge column, as if the image were wallpaper. `image.draw.noise --kind
fractal --tile` wraps each octave's lattice so the field is **exactly periodic**
at the canvas size, and the self-check is a measurement rather than an opinion:
sample a few rows, compare the two edge columns with each other, and compare them
with the gap between neighbouring columns *inside* the image. Seamless means the
first gap is no bigger than the second.

```jsonl
{"command":"image.new","args":{"width":32,"height":32,"background":"#24421b"}}
{"command":"image.draw.noise","args":{"kind":"fractal","seed":7,"cell":8,"octaves":4,"tile":true,"from":"#24421b","to":"#7fae4e"}}
{"command":"image.draw.noise","args":{"kind":"white","seed":11,"from":"#000000","to":"#ffffff","opacity":0.1}}
```

The first noise call is the grass shading, periodic. The second adds fine grain —
and `white` noise is *per-pixel*, so it rewrites every pixel from its own hash and
has no idea the image is supposed to wrap. Measuring the edges shows it:

| row | edge gap | largest in-image neighbour gap |
| --- | --- | --- |
| 3 | 18 | 18 |
| 11 | **26** | 20 |
| 19 | 4 | 7 |
| 27 | 12 | 20 |

The seam is wider than any interior gap — row 11 jumps 26 grey levels across the
wrap. The fix is not "drop the grain": it is to make the grain periodic too, with
a `value` lattice that is small (`--cell 2`) and wrapped (`--tile`). That
*replaces* the white line in the recipe — the corrected batch is the three steps
below, and the tables here and after the palette are that batch and nothing else:

```jsonl
{"command":"image.new","args":{"width":32,"height":32,"background":"#24421b"}}
{"command":"image.draw.noise","args":{"kind":"fractal","seed":7,"cell":8,"octaves":4,"tile":true,"from":"#24421b","to":"#7fae4e"}}
{"command":"image.draw.noise","args":{"kind":"value","seed":11,"cell":2,"tile":true,"from":"#000000","to":"#ffffff","opacity":0.1}}
```

| row | edge gap | largest in-image neighbour gap |
| --- | --- | --- |
| 3 | 6 | 8 |
| 11 | 6 | 5 |
| 19 | 0 | 0 |
| 27 | 0 | 8 |

Every wrap is now smaller than the busiest neighbour pair inside the image, so the
tile is seamless. Finish it: quantise to an 8-colour palette (`--list` reports
what it did — the 8 colours and how many pixels each took, which is the check
that an agent-authored palette is behaving) and save.

```jsonl
{"command":"image.palette","args":{"colors":8,"dither":"ordered","list":true}}
{"command":"image.save","args":{"out":"Assets/Textures/grass.png"}}
```

The palette is deterministic and position-based, so it does not reopen the seam —
the measurement above is the same before and after quantisation. `--dither
ordered` is the right choice for a repeating texture: Floyd–Steinberg diffuses
error along the scanline, which makes the last column depend on the first.

## What writing this chapter found

The very first version of Recipe 1 put the highlight on its own layer, and the
coin came out *dull* — a `#ddca8d` sheen where drawing the same highlight
directly on the body layer gave `#fde9a4`. That was a bug, not a subtlety:
`blendPixel` (the function every brush, fill and shape writes through) blended a
semi-transparent stroke against a transparent pixel's colour as `src * a`, so a
50% white fill on an empty layer stored **grey at 50% alpha**, and the compositor
then darkened it a second time. A 50% white rectangle over an opaque black
backdrop composited to 25% grey instead of 50%. It is fixed in 0.1.21: the
colour is now weighted by the destination's own alpha, which over an opaque
destination is exactly the old `src * a + dst * (1 - a)`, so nothing that worked
changed — and the alpha channel still uses `max()` rather than source-over, so a
repeated stroke or a mirrored draw cannot build up coverage. See the
[CHANGELOG](../CHANGELOG.md) for the entry; the loop is what surfaced it.

## Checking a slice before it ships

A sheet is a claim about a grid — this strip is two 32×32 frames, this character sheet
is 4×2 — and the drawing tools know nothing about that claim. Opening the PNG in a
viewer answers neither "did the grid land where I meant" nor "which cells does this
clip play", and until 0.1.33 the only way to answer either was to put the sprite in a
scene and screenshot it.

`sprite.sheet` answers both from the command line, through the same model the editor's
2D panel draws. A two-frame 64×32 strip, sliced 2×1 and animated at half a second a
frame:

```jsonl
{"command":"image.new","args":{"width":64,"height":32,"background":"transparent"}}
{"command":"image.draw.rect","args":{"x":2,"y":2,"width":28,"height":28,"filled":true,"color":"#d24b4b"}}
{"command":"image.draw.rect","args":{"x":34,"y":2,"width":28,"height":28,"filled":true,"color":"#4b7fd2"}}
{"command":"image.save","args":{"out":"Assets/Textures/blink.png"}}
{"command":"component.set","args":{"id":4,"type":"SpriteRenderer","prop":"texturePath","value":"Assets/Textures/blink.png"}}
{"command":"component.set","args":{"id":4,"type":"SpriteAnimator","prop":"columns","value":2}}
{"command":"component.set","args":{"id":4,"type":"SpriteAnimator","prop":"rows","value":1}}
{"command":"component.set","args":{"id":4,"type":"SpriteAnimator","prop":"durations","value":[0.5,0.5]}}
{"command":"component.set","args":{"id":4,"type":"SpriteAnimator","prop":"clips","value":[{"name":"blink","firstFrame":0,"lastFrame":1}]}}
{"command":"component.set","args":{"id":4,"type":"SpriteAnimator","prop":"clip","value":"blink"}}
{"command":"scene.save","args":{"file":"Assets/Scenes/Main.cscene"}}
```

```bash
cairn --project ./game --scene Assets/Scenes/Main.cscene exec.run --file slice.jsonl
cairn --project ./game --scene Assets/Scenes/Main.cscene sprite.sheet --id 4 --clip blink --frame 1 --output /tmp/blink.png
```

The second command is the check: it reports the grid as numbers — `region`, `cell`,
`count`, and every cell's `frame`, `x`, `y`, `w`, `h` and `seconds` — and writes a
preview PNG with the same marks the panel draws: cyan slicing, magenta for the cells
the clip plays, the `--frame` cell lit. The reply says `cell: {"width":32,"height":32}`,
`count: 2`, `columns: 2`, `rows: 1` and two cells of `0.5 s`; the preview then shows
the boundary at x=32 and the lit right-hand cell. Pixels out of that preview read with
`image.pixel` are what a test or a batch can assert on, which is how the CLI suite pins
slicing without a scene render.

Two things the command cannot check for you: whether the drawing inside a cell is
centred or on its own pixel grid (that is still a look at the sheet), and whether the
grid is the one you *want* — `columns` and `rows` are equal slices, so an artist's
sheet with uneven cells has to be cut with `image.crop` first, or given a `uvRect` per
animation.

## Gotchas

- **`image.draw.line` needs `--brush 2` or more.** `strokeLine` stamps discs of radius
  `width / 2` and `stampDisc` skips any pixel whose centre is farther than that radius
  from the stamp centre, so the default 1 px brush covers a pixel only when a stamp
  lands exactly on its centre: a horizontal or vertical line between integer
  coordinates paints *nothing*, and a 45° line comes out dotted and misses its last
  pixel. Draw 1 px geometry with `image.draw.rect` (`--height 1`) instead. The coverage
  model is recorded as a candidate in [docs/roadmap.md](roadmap.md).

- **Coordinates are pixels, `(0,0)` is top-left.** `image.pixel` reports exactly
  the `x, y` you asked for; `--cx/--cy` of a shape is in the same space.
- **`--mirror` reflects about the canvas centre line**, mapping `x → width - x`
  (and `y → height - y`), then draws with the reflected arguments. It is what
  makes a top highlight and its bottom twin come from one call. `both` reflects
  both axes; `none` (the default) reflects nothing.
- **Layer 0 is the bottom.** `image.layer.add` puts a new, empty layer on top and
  makes it active, so a draw with no `--layer` lands there. `image.preview` and
  `image.pixel` read the *composite* unless you pass `--layer`; `image.save`
  composites unless you pass `--layer`.
- **Determinism is a feature.** `image.draw.noise` is a pure function of its
  `--seed` and arguments — no time, no `Math.random` — so the same batch paints
  byte-identical pixels and two revisions of a generator can be diffed. Any
  randomness you want, you seed.
- **`--tile` only applies to `value` and `fractal`.** `white` noise is per-pixel
  by definition; wrap it with a `value` lattice if the tile has to be seamless
  (Recipe 2).
- **Transparency is a colour you choose.** Start from `--background transparent`
  for a sprite, a solid colour for a texture, and check `touchesEdge` in
  `image.info` to catch content running off the canvas.
- **One process is one session.** State (the document, its layers, `--layer`
  defaults) lives in the CLI session, so anything longer than a couple of steps
  goes into an `exec.run` batch: the steps share the session, the output stays
  one JSON document per line, and the whole picture is reproducible from the
  file.
