# 投投 Toutou — hero-mascot research notes

Goal: promote the small cloud spirit 投投 to the hero mascot of 自在投影 / Zizai Cast. Non-humanoid,
vector-native, charm from shape language + expression + motion. The rejected chibi girl failed on
uncanny cues (rigid symmetric stare, smug fang, helmet hair, flat lifeless skin) — every rule below is
partly written to avoid those exact failure modes.

## Sources (what each taught us)

| Source | Take-away |
|---|---|
| Duolingo — Shape language: Duolingo's art style — https://blog.duolingo.com/shape-language-duolingos-art-style/ | Everything built from 3 primitives (rounded rect, circle, rounded triangle); all corners rounded ("pointy is off-brand"); fewest shapes possible; vary shape *sizes* for rhythm; head and body = 1–2 shapes each; repeat shapes to keep a character cohesive; exaggerate to near-caricature. |
| Duolingo illustration guidelines — https://design.duolingo.com/illustration | Composable system: hero character + props (hearts, speech bubbles, geometric bg) recombine freely. Bold, bouncy, bright; generous negative space so silhouettes read on small screens. |
| Creative Review on the Johnson Banks Duolingo rebrand — https://www.creativereview.co.uk/duolingo-rebrand-johnson-banks/ | The whole identity (type, illustration) is derived from the mascot's own rounded form — the mascot *is* the shape language. |
| Cameron McEfee, "The Octocat — a nerdy household name" — https://cameronmcefee.com/work/the-octocat/ ; Simon Oxley — https://en.wikipedia.org/wiki/Simon_Oxley | A simple, ownable silhouette (the Invertocat) is what scales to favicon/sticker; personality is then grown by *costumes and situations* (Octodex), not by adding detail to the base character. |
| Lorenz' Kindchenschema; Glocker et al. 2009, "Baby schema in infant faces induces cuteness perception" — https://pmc.ncbi.nlm.nih.gov/articles/PMC3596402 | Measured cuteness drivers: large round head, high forehead, big eyes placed low, small nose/mouth, round cheeks. Cute = low, wide-set eyes, tiny mouth, big "forehead" area. |
| Animation principles: appeal, squash & stretch, silhouette staging — https://people.wku.edu/joon.sung/edu/anim/3d/theory/good_character.html ; https://courses.cs.washington.edu/courses/cse457/09sp/lectures/animation.pdf | Stage poses in silhouette; squash & stretch conveys weight, softness and personality (soft characters squash a lot, volume preserved); appeal = magnetic presence, not prettiness. |
| LINE FRIENDS (Brown & Cony), Kang Byeongmok — https://en.wikipedia.org/wiki/Line_Friends | Near-blank faces (dots + tiny mouth) succeed because the *body* and props carry the emotion; stickers are built for instant read at chat size. |
| Miffy / Dick Bruna — https://en.wikipedia.org/wiki/Miffy | Radical reduction: heavy uniform line, primary flat colour, two dots and a cross for a face — and still endlessly expressive through posture. Limited palette = recognisability. |
| Mailchimp Freddie rebrand (Collins) — https://www.designweek.co.uk/issues/1-7-october-2018/mailchimp-rebrand-aims-to-unify-bran | Mascot mark simplified "to work at any size"; one signature colour (Cavendish yellow) carries the brand; supporting palette is quiet pastels. |
| Discord Wumpus — https://shapes.inc/fandom/discord/characters | Round body + one signature feature (small horns); shy-but-social personality expressed by posture; 2021 refresh = brighter, more expressive features, not more detail. |

## 12 rules for 投投

1. **Primitives only.** Body = a union of circles (3–5 puffs) + one rounded base; face = ovals/arcs; props = rounded rects. No free-hand blobs, no sharp corners.
2. **One silhouette, testable in black.** Filled solid, 投投 must still read as "a cloud holding a phone" at 48 px and as "a cloud" at 16 px. Asymmetric puff sizes (big–medium–small) make it a *character*, not the ☁ emoji.
3. **Head = body.** No neck, no separate torso: the whole cloud is the face-carrier. Width : height ≈ 1.35 : 1, bottom flattened so it can sit, sleep and squash.
4. **Low, wide eyes.** Eye line in the lower ~40 % of the body height; eye spacing ≥ 2.5 eye-widths; mouth tiny and placed between/just below the eyes. The big empty "forehead" is the cute part.
5. **Eyes must be alive.** Every open eye has a primary highlight (upper, same side in every pose = consistent light) + a smaller secondary one; pupils are vertical ovals, not perfect circles; never a pure flat stare.
6. **Break symmetry on purpose.** Default pose has a 3–6° head tilt, one arm raised, phone held off-centre, uneven puffs. Symmetric front-on poses are only allowed for icons.
7. **Expression = eyes + mouth + body.** Each emotion changes all three: eye shape (dot / ^^ / flat-lidded / wide / half-lid / closed arc), mouth (smile / open / pressed / o / tiny), and body squash & stretch (happy stretches up, sleepy squashes, surprised pops tall). Volume is preserved (sx · sy ≈ 1).
8. **No smug cues.** No fangs, no smirks, no narrowed side-glances on the hero; "determined" is shown with flat upper lids and a pressed mouth, never a grin.
9. **Decide outline once.** Each style direction picks *either* a uniform outline (sticker) *or* no outline (soft / geometric) and never mixes. Outline colour is warm dark brown, never black.
10. **Limited palette.** Body neutral warm-white + one darker tint for shade; face ink; blush; **one** theme accent. The accent is only ever used on "product" parts (phone screen, beam, signature mark), so re-theming = swapping one colour.
11. **Signature trait tied to the product.** 投投 always carries/hugs a mini phone whose screen glows the accent, and "casts" a beam to the big screen. Each direction adds one ownable mark (▶ forehead mark / pixel beam / screen-shaped glint + wisp).
12. **Design for the smallest size first.** Icons get their own hinted geometry: at 16 px drop phone, cheeks and highlights-within-highlights; keep only cloud silhouette + two eye dots, aligned to the pixel grid, high contrast on the tile.

## Anti-patterns we explicitly check in critique

- Eyes centred vertically on the body (reads adult / stare).
- Pupils without highlights or with highlights that differ by pose (reads dead / inconsistent).
- Perfect bilateral symmetry in hero poses (reads mask-like).
- Flat body with no shade at all *and* no outline (reads like a sticker cut-out with no form).
- Accent colour leaking onto the body (breaks theming).

## Iteration log (render → critique, 13 rounds, all three directions each round)

1. First pass: cloud read as a soft-serve dollop (top puff too dominant); side nubs read as loose pearls; "determined" and "sleepy" lids read *angry*; silhouette panel broken.
2. Rebuilt the cloud as big–medium–small puffs with clear valleys (passes the black-silhouette test); bigger eyes for A.
3. C was "A without gradients" → re-constructed on an 8 px grid as 2 circles + 1 pill (own silhouette), signature changed from a sprout wisp (read as garlic/onion) to a floating cast-signal mark.
4. B: dark outline vanished on the warm-dark app bg → white die-cut border (sticker); arms were outlined donuts (read as wheels/eyeballs) → larger fill ratio.
5. Die-cut via morphology made squares around sparkles/hearts → round blur-threshold die-cut.
6. Sheet + app mock assembled; mock mascot too small → enlarged and raised.
7. Connecting face rebuilt: flat lids read "bored" → eager gaze toward the beam + star glint + small open smile.
8. Sleepy lids now droop toward the outer corner on a curve (never toward the nose).
9. Tried two hands hugging the phone → stacked nubs read as "8"/snowman; calm poses now show only the phone hand (fewest shapes).
10. Icons: 48 px switched to hinted small geometry (full pose too busy); ground shadows removed from icons.
11. 256 icons re-centred and enlarged in tile.
12. A beam widened into a 3-layer ribbon; C beam dots stop before the screen; C body tone darkened slightly so the screen-shaped glint is visible.
13. Final whole-sheet review at all four theme accents.

## Final art (owner-approved direction: hero = A 「軟綿光暈」, icon = C construction in the A palette)

Sources: `hero.mjs` (character + layers), `icon.mjs` (icon, per-size pixel grids), `export.mjs` (all outputs),
`review.mjs` / `iconreview.mjs` (critique sheets → `work/final_<n>.png`, `work/final_<n>_120x2.png`,
`work/icon_<n>.png`). Reference sheet: `final.png`. Public exports: `assets/public/toutou/` (expressions,
layers, per-theme phones, masters, icons, SVG). App layers: `video/res/toutou/`.

Critique rounds (each with zoomed renders; expressions always also judged at 120 px):

1. Beam read as a thin thread; hand as a grey bean with a dark drop shadow (detached); pale horizontal bands on
   the forehead (crease strokes in the wrong place); lower-lid crescent too strong; connecting star glints read
   as "x" eyes at 120 px; sleepy read grumpy; wave nub floated.
2. Creases moved to the real puff valleys (computed circle intersections); thumb bump made the hand read as a
   little bird (beak); beam became a cone/flame instead of a ribbon and did not arc.
3. Beam leaves the phone along its axis and arcs (control point on the phone axis); the ▶ on the screen next to
   the nub formed a beak → hand moved to the phone's lower-left corner.
4. Hand read as a pearl again (free ellipse) → rebuilt as an arm stub: a mitten tip on the phone corner plus a
   root buried in the body and a soft crease, lit like the body, no specular dot.
5. Arm stub reads as an appendage; beam still pinched by the twist → twist amplitude halved, band brighter.
6. 2× zoom of the 120 px row: happy / blink / surprised / asleep instant; sleepy still "judgy", connecting too
   close to idle.
7. Sleepy = heavy lids low over a sliver of eye + a yawn + one z; connecting = gaze up the beam, bigger
   catchlights, small open eager smile.
8. Occlusion shadow made dark-translucent (it lightened the dark phone), wave nub rooted deeper; layer
   export at 4 px/unit for 3440x1440.
9. In the app (D2D composition): paused cloud at 62 % opacity read as a muddy grey blob → 84 %; hearts removed
   from the sleepy reaction; ground shadow too faint on the dark themes → stronger.
10. In the app at 3440x1440: the stacked-rect phone halo banded → radial gradient halo.

Icon rounds: (1) 256 good, 16–24 too tall (onigiri / ghost), 32 px eyes blurry → (2) wider cloud on each small
grid, pixel-rect eyes → (3) 24 px eyes 2x2 instead of 2x3 → (4) crisp eyes at 40/48, hard-edged sheen removed.
The pink-gradient tile was chosen over a bare cloud: the white cloud disappears on a light taskbar.

Layer model (what the app draws, back to front): ground shadow (D2D, scales with the float) · cloud · rim mask
tinted with the accent · face (idle / blink / happy / connecting / surprised / sleepy / asleep) · phone (D2D:
frame, accent screen, ▶, halo; lying with the screen off when asleep) · hand for the phone pose · beam (D2D
ribbon + square pixels) · hearts / zzz / bubble (D2D).
