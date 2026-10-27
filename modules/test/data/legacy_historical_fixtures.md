# Legacy historical parser fixtures

These fixtures are synthetic, redistributable, and contain no user model data. `legacy_historical_decoder_test.cc` is an independent hand encoder for the historical primitive wire and emits the fixture bytes in memory. The production parser never calls the fixture encoder.

The fixtures begin at the decompressed v3 plaintext boundary. Envelope transforms are covered independently by `legacy_v3_dynamic_vector.ysm`; combining the two boundaries is owned by the production importer callback.

## Version matrix

| Inner versions | Historical source layout | Expected structure |
| --- | --- | --- |
| 1 | pre-info legacy suite; optional RGBA texture; legacy name migration | player with `main`/`arm`, migrated `skin` texture, derived metadata defaults |
| 2 | optional suite info | player with `main`/`arm`, explicit current properties |
| 3 | PBR texture set | named `skin` texture with empty PBR map |
| 4 | author-avatar map | empty avatar map accepted |
| 5 | flattened shuffled quads plus global cube count | each stored quad becomes one one-face cube; count retained |
| 6–7 | restored cube grouping; optional export | player with grouped cube wire and no export |
| 8–9 | `render_layers_first` property | explicit false value retained |
| 10 | controller vector/hash and animation loss optionals | empty controller closure and optional fields accepted |
| 11 | sounds and sound keyframes | empty sound closure accepted; detailed sound projection is covered downstream |
| 12 | extra animation buttons/classifications | empty new collections accepted |
| 13 | `all_cutout` | explicit false value retained |
| 14 | `disable_preview_rotation` | explicit false value retained |
| 15 | structured controller blend transition | new reader gate selected |
| 16 | user functions | empty function/hash closure accepted |
| 17 | `gui_no_lighting` | explicit false value retained |
| 18 | languages | empty language/hash closure accepted |
| 19 | current container family | current common/player/info field order selected |
| 20 | GUI image map | empty map accepted |
| 21 | legacy vehicle map | empty map accepted |
| 22 | replacement controller optional | current target reader gate selected |
| 23–24 | encoded image format/frame metadata | RGBA format and one frame validated |
| 25 | v3 full-float layout | 32-bit float `1.25` accepted; half-float mutation rejected |
| 26 | post-half-float removal wire | full-float reader remains selected |
| 27 | ordered replacement vectors and `origin_ver` | vector layout and origin value accepted |
| 28 | controller sound effects | latest controller reader selected |
| 29–31 | unchanged v3 logical wire | exact preceding gates retained without guessing |
| 32 | `merge_multiline_expr` | explicit false value retained |

Every positive case uses hash `000102030405060708090a0b0c0d0e0f`; the expected ModelId is bytes `00` through `0f` followed by sixteen zero bytes. Uppercase hash input must produce the same identity.

The v32 focused fixture also records positive infinity as the historical
animation-length sentinel produced when the old JSON importer converted
`max(double)` through the binary32 wire. NaN and negative infinity remain
invalid, and all fields whose current schema requires finite values use the
finite reader.

## Negative mutations

The focused test changes one parser invariant at a time: versions 0/33, half-float v25, truncated/trailing plaintext, wrong-length/non-hex hash, dangling/cyclic parent, non-finite schema field, unknown enum, invalid optional tag, duplicate map key, noncanonical varint, and collection limit + 1. A fixed-seed mutation smoke run adds 512 arbitrary inputs and one bit mutation at every byte position of a valid v32 fixture. Expected failures use only numeric `StatusCode` identity; diagnostic text is not an oracle.

Concurrent tests share the submitted dynamic envelope vector and a synthetic
v32 plaintext fixture. Each worker owns its decoder and conversion state.
