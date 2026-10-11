"""Editable prompt library for original-image -> AI reference preprocessing.

Edit the matching dictionary entry; routing/provider code stays in the other
modules. Rules are composed in order and exported by image_preprocessing_debug.
Portrait/unknown retain their legacy composer; non-portraits use only matching
subject/features. Shared helpers and STYLE_PROFILES also serve text generation.
Do not claim measured thickness or invent invisible parts in a prompt.
"""

# 打印尺度模板：由 policy 中已验证的 print 参数填入；仅非人像路径启用。
PRINT_SCALE_TEMPLATE = (
    " Intended print width is {width_mm:g} mm; nozzle {nozzle_mm:g} mm, "
    "line width {line_width_mm:g} mm, minimum printable feature {minimum_feature_mm:g} mm. "
    "At that scale, give existing load-bearing rods, rims and joints readable finite cross-sections and positive-volume "
    "overlap. Preserve important holes and gaps; do not close them to meet a thickness target. Do not add support scaffolding "
    "or alter the subject inventory. These are design intentions, not measured dimensions or a printability guarantee; "
    "the resulting 3D mesh still requires thickness, connectivity and slicing checks."
)

# 风格：人像/未知及文字生图共用；非人像的四种短风格覆盖见下方。
STYLE_PROFILES = {'sculpture': 'Change only the visible material treatment into one monochrome museum-quality plaster, '
              'clay, stone, or matte resin sculpture. Preserve the source subject one-for-one: the same '
              'identity, face, age, expression, body proportions, pose, silhouette, crop, clothing, '
              'accessories, objects, count, placement, and visible details. Do not redesign, beautify, '
              'exaggerate, add, remove, reveal, or reconstruct anything. Use gentle carved planes and '
              'broad connected forms only where needed for a printable single-material result. Keep every '
              'source-visible opening, handle, wheel, limb, tier, and base; monochrome means one material, '
              'not fewer components.',
 'realistic': 'Create a multi-color realistic collectible while changing as little as possible beyond '
              'material and color treatment. Preserve the source subject one-for-one: the same '
              'recognizable identity, face, age, expression, anatomy, proportions, pose, silhouette, crop, '
              'clothing, accessories, objects, count, placement, and visible details. Use believable '
              'naturally colored materials and restrained realistic modeling; do not stylize facial '
              'proportions, invent detail, genericize manufactured parts, or alter the composition. When '
              'the subject is a real person, use the shape language of a highly faithful polychrome '
              'portrait sculpture or faithful 3D scan: carry identity in the actual face silhouette and '
              'sculpted anatomical planes while keeping source-faithful large material colors. Prioritize '
              "likeness over idealized attractiveness: retain the person's natural adult facial asymmetry "
              'and landmark proportions instead of applying a beauty-filter, toy, game-avatar, or generic '
              'commercial-character face. Mildly groom skin and hair surfaces only; never enlarge the eyes '
              'or irises, lift both brows into a stock expression, narrow the nose, widen the smile, taper '
              'the jaw into a V, or shorten the lower face. Do not pursue photographic beauty lighting or '
              'painted skin detail at the expense of recognizable three-dimensional facial geometry.',
 'portrait_sketch': 'Restyle the same real person as an identity-first portrait sketch sculpture. Preserve '
                    'the exact recognizable identity, adult age, expression, facial silhouette, landmark '
                    'spacing, asymmetry, hairstyle, pose, crop, clothing, and accessories. Use restrained '
                    'exaggeration only to clarify an already-present brow, cheek, smile fold, jaw '
                    'transition, or gesture; never replace the person with a generic caricature, idealized '
                    'celebrity, doll, anime face, or stock street-artist template. Translate tone into '
                    'expressive material or relief forms with continuous sculptural modeling. Prefer a few '
                    'confident, printer-width contour grooves and connected planes over drawn texture. Use '
                    'only semantically truthful masses, and do not force every selected color to appear '
                    'when that would create a tiny or false region.',
 'cartoon': 'Restyle the same subject as a friendly cute cartoon collectible, especially for portraits '
            'that look harsh when rendered realistically. Preserve recognizable identity, age, expression, '
            'hairstyle, pose, clothing, accessories, visible objects, subject count, crop, and '
            'composition. Use rounded connected forms, clean large shapes, and modest playful '
            'simplification. Do not replace the face with a generic doll or anime face, do not enlarge '
            'eyes excessively, and do not add or remove elements. Make the result cute through expression, '
            'clean curves, and material treatment rather than changing identity, age, anatomy, or the '
            "subject's distinctive proportions.",
 'low_poly': 'Restyle the same subject as a deliberate low-poly printable model built from broad, clean '
             'planar facets. Preserve the recognizable silhouette, viewpoint, component count, pose, and '
             'identity-defining proportions, but replace fragile surface detail, fur, foliage, fabric '
             'texture, and shallow ornament with a small number of sturdy geometric planes. Keep every '
             'load-bearing connection visibly fused and avoid random triangulation noise or razor-thin '
             'spikes.',
 'relief': 'Convert the source into a printable shallow bas-relief mounted on one simple solid plaque. '
           'Preserve the source-facing silhouette and recognizable internal contours while expressing '
           'depth with a few broad raised levels. Do not reconstruct an unseen back side, create '
           'undercuts, detach foreground elements, or add a decorative frame unless it is already '
           'requested.',
 'ink_relief': 'Convert the complete visible composition into one printmaking- and woodcut-inspired ink '
               'relief on a simple solid backing plaque. Preserve subject identity, viewpoint, count, '
               'silhouette, and recognizable internal contours. Organize positive and negative space into '
               'two to four broad value masses, and turn only the most important brush or cut marks into '
               'connected, printer-width embossed or engraved strokes. Use shallow stepped depth and solid '
               'opaque regions; forbid translucent washes, wet-on-wet gradients, gray mist, micro-halftone '
               'dots, stippling, hairline hatching, and floating ink flecks. Use only semantically '
               'truthful masses, and do not force every selected color to appear when that would create a '
               'tiny or false region.',
 'diorama': 'Restyle the complete visible composition as one compact printable miniature diorama. Preserve '
            'the main subjects, their relative placement, viewpoint, and scene identity, but merge the '
            'ground and supporting elements into one stable base. Simplify distant detail into layered '
            'masses, keep subject count unchanged, and avoid floating props, loose foliage, thin rails, or '
            'deep hidden cavities.'}

# 非人像风格：仅覆盖这些风格；其余回落 STYLE_PROFILES。
NONPORTRAIT_STYLE_PROFILES = {'realistic': 'Create a faithful realistic collectible. Preserve exact recognizable proportions, '
              'viewpoint, silhouette, component count and natural material groups. Do not genericize '
              'manufactured parts.',
 'cartoon': 'Create a friendly cartoon collectible using broad rounded connected forms while preserving '
            'recognizable subject identity, component count, pose and silhouette. Do not substitute a '
            'generic toy.',
 'sculpture': 'Use one monochrome matte sculptural material with continuous tonal modeling. Preserve the '
              'exact subject, visible parts, proportions, crop and pose. Use gentle broad connected planes '
              'without redesigning the subject.',
 'portrait_sketch': 'Translate the exact source subject into a sculptural sketch with broad connected '
                    'planes and shallow contour grooves. Preserve its proportions, part count, viewpoint '
                    'and negative spaces without inventing detail.'}

# 通用：原图权威、构图/视角、背景、灯光、结果自检。
COMMON_RULES = {'solid_background': 'Use one uniform opaque solid-color studio background, preferably neutral mid-gray. '
                     'Choose a different uniform tone if needed to clearly separate the background from '
                     'every subject region, including white clothing, hair and the base; never recolor the '
                     'subject to create contrast. Do not request transparency or draw a transparency '
                     'checkerboard, checker pattern, grid, tiles, background texture, gradient, scenery, '
                     'floor shadow or halo. Replace any such backdrop in the source image. Preserve the '
                     'complete subject and base with clear empty margins on all sides. ',
 'legacy_geometry_lighting': 'This is the geometry reference. Preserve continuous tonal modeling and soft '
                             'broad diffuse lighting so silhouette, facial landmarks, joints, folds, and '
                             'sculptural planes remain legible. Preserve broad natural material groups '
                             'together with their gradients, texture and fine color detail. Do not bake '
                             'lighting or cast shadows into geometry. ',
 'legacy_source_inventory': 'Treat the source as a closed visual inventory. Preserve its exact viewpoint '
                            '(front, three-quarter, side, or rear), facing direction, left-right '
                            'arrangement, silhouette, component count, negative spaces, and all '
                            'identity-defining asymmetry. Never mirror the subject or substitute a more '
                            'typical example of its category. ',
 'legacy_reference_authority': 'Transform the supplied reference into a polished designer-ready style '
                               'preview for later image-to-3D. The supplied source image is the authority '
                               "for the primary subject's identity and recognizable structure. ",
 'lighting_and_groups': 'Preserve continuous tonal modeling with soft broad diffuse lighting. Do not bake '
                        'cast shadows, reflections or highlights into geometry. Use every explicitly '
                        'requested subject and preserve their relative placement; do not merge separate '
                        'bodies for connectivity.',
 'source_inventory': '\n'
                     'Honor explicitly requested changes; preserve all other source structure under the '
                     'following rules. Treat the source as a closed visual inventory: preserve requested '
                     'subject count, proportions, visible component count, left-right order, asymmetry and '
                     'key negative spaces. Preserve its exact viewpoint and pose; do not rotate a '
                     'side/rear image into a front or three-quarter view. Do not reconstruct unseen parts '
                     'or extend a cropped object. Isolate the requested subject or requested group, center '
                     'it with empty margins, and remove only unrelated scenery. ',
 'reference_authority': 'Prepare the supplied image as a faithful image-to-3D geometry reference. The '
                        'original is the authority for the requested subject. User direction: ',
 'source_checklist': 'Before returning, compare source and result: correct omitted/duplicated parts, '
                     'closed key openings, changed proportions, mirrored layout and detached visible '
                     'connections.',
 'material_risk': 'Reduce isolated glare and misleading reflections while retaining the requested material '
                  'identity. Reflections and transparency are not geometric holes; do not invent hidden '
                  'parts from them.'}

# 人像：身份锁、脸部/手臂/衣物几何、源图裁剪、底座、表面/肤色、禁贴脸。
PORTRAIT_RULES = {'display_base': 'Portrait base policy: every generated portrait must be free of a separate display base. '
                 'Do not preserve, recreate, or invent a source-visible base, seat, floor contact, '
                 'support, plinth, disc, pedestal, or shared display platform. Preserve actual '
                 "source-visible anatomy, including a person's pelvis, legs, or feet, as part of the "
                 'subject itself. Always keep the generated portrait base-free and finish the visible '
                 'lower torso or clothing with a clean printable boundary. Do not use a shadow-only '
                 'contact or a floating fragment to imply a base. Orca may add a selectable round, oval, '
                 'or rectangular display base after generation, once the user has completed geometry and '
                 'colour editing. ',
 'identity_geometry': 'Real-person identity geometry rule: preserve adult age, source-visible feminine or '
                      'masculine presentation, face width and length, hair part and sweep, hairline, '
                      'visible ears, eye spacing and shape, eyebrow arc, nose bridge/width/tip, mouth '
                      'width, smile asymmetry, cheek volume, jaw contour, and chin length. Match the '
                      'source landmark ratios rather than a memorized attractive face: inter-eye distance, '
                      'visible eye opening, brow-to-eye distance, nose width and projection, nose-to-mouth '
                      'distance, mouth width, upper-to-lower lip balance, cheek width, and the lower '
                      'facial third must stay source-faithful. Preserve small left-right differences in '
                      'eyelids, brows, smile corners, cheeks, and jaw. Encode the eyelids, nose, '
                      'cheekbones, smile folds, mouth corners, and jaw transition as restrained modelable '
                      'relief and silhouette, not only as gradients, highlights, makeup, or thin painted '
                      'lines. Keep teeth as one shallow readable smile band rather than many tiny separate '
                      'teeth. Never turn an adult into a big-eyed childlike, game-avatar, beauty-filtered, '
                      'or generic doll face. Do not make both eyes wider or rounder than the source, and '
                      'do not replace natural facial asymmetry with perfect symmetry. Preserve the exact '
                      'source-visible crossed-arm order, exposed wrist and hand count, jacket lapels, and '
                      'inner neckline; group fingers into sturdy forms, fuse wrists to sleeves and '
                      'forearms, and never mirror or invent a second hand. A watch may be simplified into '
                      'one solid fitted band. Preserve garment coverage exactly around every clothed arm: '
                      'if a jacket or sleeve covers an elbow, upper arm, forearm, or wrist in the source, '
                      'continue that same garment as one closed tube around the underside, side, and '
                      'occluded back. Never invent a bare elbow, upper arm, or forearm behind a crossed '
                      'arm, and never use skin color as an inferred shadow on clothing. Skin is allowed '
                      'only on source-visible face, ears, neck, hands, and exposed wrist areas. With '
                      'crossed arms, preserve every clearly visible hand, but do not turn a thin partially '
                      'occluded skin sliver into a stripe across the jacket: either show it as one '
                      'compact, anatomically bounded hand/wrist region or keep that ambiguous sliver fully '
                      'tucked beneath the existing sleeve without changing the arm order. Do not age the '
                      'person up, change their presentation, broaden or narrow the face, or replace an '
                      'asymmetric hairstyle with a generic centered cap of hair. ',
 'identity_lock': 'IDENTITY-FIRST PORTRAIT LOCK — highest priority when the source contains a real person: '
                  'make the smallest possible face edit. Treat the source head and face as a locked '
                  'geometric reference, not inspiration for a newly drawn attractive person. Keep the face '
                  'bounding box relative to the shoulders, head angle, eye centers and eyelid openings, '
                  'brow heights, nose tip and nostril width, mouth corners, tooth exposure, cheek outline, '
                  'jaw corners, and chin endpoint aligned to the source at the same scale. Do not '
                  'substitute a generic professional portrait, narrow or symmetrize the face, enlarge both '
                  'eyes, or widen the smile. Preserve identity through modelable facial planes and relief, '
                  'not a photographic face overlay. Render the face in the same coherent sculptural '
                  'material and lighting as the head and body; surface simplification must not move, '
                  'resize, or reshape identity landmarks. Before returning, compare the source and result '
                  'face at equal size and correct any landmark drift. ',
 'no_photo_overlay': 'Avoid dithering and tiny color speckles. Do not return the unchanged source as a '
                     "whole. Preserve a person's identity in the generated shape, not by pasting original "
                     'photo pixels onto the face. Do not copy source-background shadows, gray halos, or '
                     'photographic texture around the head into the generated reference.',
 'surface': ' For a real-person subject, refine the surface conservatively while keeping the locked facial '
            'geometry. Reduce capture noise, isolated specular glare and transient skin blemishes; retain '
            'age cues, characteristic creases, moles, freckles and deliberate makeup. Do not smooth away '
            'the eyelid rim, nostril boundary, lip edge or the individual shape of the mouth. Keep lips, '
            'eyebrows, irises, sclera, hairline and clothing boundaries distinct at the intended '
            'reference-image scale without enlarging or outlining them. Use neutral white-balanced, broad '
            'diffuse illumination: enough gentle shading to read facial planes, with no colored rim light, '
            'hard cast shadow across the face, oily highlight or beauty-filter whitening. Keep the '
            "person's natural skin hue consistent across face, ears, neck and visible hands, including "
            'shaded areas; preserve natural local variation rather than making skin one flat swatch. '
            'Lighting darkness must not be interpreted as a different skin material, painted dirt, a deep '
            "wrinkle or a geometric hole. For multiple people, retain each person's own facial proportions "
            "and skin tone independently; never average their faces or swap their features. A user's "
            'explicitly requested lighting or intentional color remains controlling. ',
 'source_crop': 'Apply these source-dependent framing rules: if the complete person, animal, or object is '
                'visible, preserve the complete head-to-toe or whole-object form and its existing pose. If '
                'a person is cropped before the knees or only the upper body is visible, create a '
                'deliberately finished bust or half-body collectible: preserve only the visible head, '
                'torso, arms, and clothing, end the lower torso with a clean printable boundary and do not '
                'invent a pelvis, legs, or feet. If the source is a multi-subject scenic photograph and '
                'the user did not explicitly request a pair, group, set, or exact subject count, isolate '
                'exactly one requested or dominant subject and omit all secondary subjects and background '
                'scenery. Never duplicate a face, limb, tower, statue, accessory, or architectural '
                'element. Determine this source crop and visible anatomical extent before applying style '
                'or palette. Changing palette mode, palette colors, or print constraints must not change '
                'full-body versus bust framing or reveal anatomy outside the source crop. '}

# 题材：独立调优 animal / architecture / hard_surface / organic / flat_graphic / scene / effects。
SUBJECT_RULES = {'animal': 'Preserve species/breed, ear and limb counts, muzzle proportions, tail pose and exact coat, '
           'feather or shell markings. Do not invent patches. Integrate fragile tips into existing anatomy '
           'without changing pose.',
 'architecture': 'Preserve tier counts, doors, window openings, arches, columns, symmetry and '
                 'source-visible functional foundations. Keep key negative spaces open; simplify dense '
                 'lattice into fewer sturdy members without filling arches or moving supports.',
 'hard_surface': 'Preserve exact count and placement of wheels, handles, lenses, buttons, openings and '
                 'attachments. Do not invent typical product features or replace manufactured parts with '
                 'generic shapes.',
 'organic': 'Preserve recognizable silhouette and major organic masses. Simplify surface microtexture '
            'rather than deleting main branches, petals, layers or components.',
 'flat_graphic': "Preserve the requested graphic's exact silhouette, internal negative spaces and "
                 'meaningful strokes. Use broad shallow connected relief, not an invented volumetric '
                 'backside.',
 'scene': 'Preserve only requested scene subjects, their count, spacing, left-right order and relative '
          'scale. Do not add plausible scenery or merge separate bodies merely to obtain connectivity.',
 'effects': "Preserve the requested effect's recognizable outline. For relief use connected shallow "
            'masses; for a volumetric sculpture use solid connected shapes, not floating particles or '
            'transparent membranes.'}

# 结构：机械 / 分枝 / 薄片 / 镂空 / 柔性 / 精密；只在对应题材匹配时添加。
FEATURE_RULES = {'mechanical': 'Source-visible axles, pistons, hinges and linkages must extend inside their original '
               'receiving housings with positive-volume overlap at both ends. Where a bucket/blade joint '
               'is visible, fuse it to its original arm; shallow grooves represent joint gaps, not '
               'disconnected parts. Do not add mechanisms absent from the source.',
 'branching': 'Use overlapping broad leaf/crown clusters joined through sturdy continuous stems to the '
              'original trunk or body. Preserve major branch topology and essential open spaces; never '
              'create isolated leaf pads or a shapeless solid crown.',
 'thin_surface': 'Give canopies, wings, fans and leaves visible finite thickness. Embed ribs into the '
                 'shell/rim and fuse hubs and shafts into the original body; use shallow panel grooves, '
                 'not detached panels or wire-like struts.',
 'hollow': 'Preserve through-holes, chain loops, vase openings and lattice negative spaces. Thicken '
           'existing rims locally; never fill holes, merge chain links into a blob or invent hidden '
           'internal walls.',
 'soft_surface': 'Keep identity-defining folds, straps and layered silhouettes; simplify fabric/food '
                 'microtexture. Join existing straps or decorations at their original contacts without '
                 'inventing fasteners.',
 'precision': 'This is a visual reference, not a dimensionally qualified assembly part. Preserve visible '
              'tooth/thread/clip layout; do not invent tolerances, fits or unseen mechanisms.'}

# 结构适用题材：添加规则时同步核对这张映射；关键词在 policy 模块。
FEATURE_SUBJECTS = {'mechanical': {'hard_surface'},
 'branching': {'organic'},
 'thin_surface': {'organic', 'animal', 'hard_surface'},
 'hollow': {'hard_surface', 'architecture'},
 'soft_surface': {'organic', 'hard_surface'},
 'precision': {'hard_surface'}}

# 主体字标：preserve / remove / auto；与背景水印分开。
TEXT_RULES = {'preserve': "Preserve the requested subject's own visible lettering, logo and glyph silhouettes, "
             'including their spelling and negative spaces. Do not blank a requested badge or invent '
             'substitute letters. If tiny text cannot be modeled faithfully, retain its source footprint '
             'without fabricating spelling. ',
 'remove': 'The user requests removal of subject lettering/logos: preserve the underlying surface and '
           'remove only those markings without inventing replacement text. ',
 'auto': 'Remove extraneous background text, watermarks and camera UI. Preserve source-visible identifying '
         'subject markings; do not invent spelling or add brand labels. '}

# 功能支撑与风格背板：relief 和 diorama 覆盖默认底座规则。
SUPPORT_RULES = {'relief': 'RELIEF SUPPORT OVERRIDE — highest priority for this style: the one simple solid backing plaque '
           'is mandatory, including for people, animals, products, machines, furniture, and complete '
           'scenes. This overrides both the portrait display-base rule and the non-human base-free rule. '
           'Compress the entire visible composition into a shallow front-facing relief fused across broad '
           'contact areas to that plaque; never return a free-standing figurine, product, or diorama. Keep '
           'a clean visible plaque margin around the raised subject and do not add a second pedestal, '
           'floor, or decorative frame. ',
 'diorama': 'DIORAMA SUPPORT OVERRIDE — highest priority for this style: one shared low terrain or floor '
            'base with a flat underside is mandatory and overrides the non-human base-free rule. Fuse '
            'every requested subject and prop to that one base. Preserve only source-visible or explicitly '
            'requested scene elements. For an isolated subject, use a minimal plain contact platform '
            'without inventing rocks, plants, furniture, buildings, signs, or other decorative scenery. '
            'Never return a base-free product shot or an ordinary display figurine with no scene-level '
            'ground relationship. ',
 'legacy_nonhuman_base': 'Except for an explicitly source-visible support, a non-human standing, seated, '
                         'crouched, lying, wheeled, naturally stable, or cleanly cropped subject must '
                         'remain base-free when the source is base-free. Do not add a disc, plinth, stand, '
                         'platform, presentation base, floor slab, or pedestal to a non-human subject '
                         'unless the user explicitly requests one. ',
 'source_functional': 'Keep a source-visible functional foundation or support as part of the subject. '
                      'Otherwise remain base-free unless the user explicitly requests a base; do not '
                      'invent a pedestal, floor or scaffolding. '}

# 颜色/材质：默认自然色，不做打印量化；哑光代理仅显式开启。
MATERIAL_RULES = {'legacy_preview_colors': 'Use coherent natural colors that fit the subject and selected style. Preserve '
                          'useful tonal modeling with broad, contiguous color regions for shape '
                          'readability. ',
 'matte_proxy': 'EXPLICIT MATTE GEOMETRY MODE: use one neutral matte clay material with broad diffuse '
                'shading, retaining every shape and opening. This overrides natural surface colors only '
                'for this requested geometry reference; it does not change the original color source. ',
 'nonportrait_sculpture': 'The selected monochrome sculpture treatment keeps one material with natural '
                          'tonal modeling. There is no printer palette constraint. ',
 'nonportrait_ink_relief': 'The selected ink-relief treatment uses its intentional broad value masses and '
                           'shallow strokes without a printer palette constraint. ',
 'nonportrait_natural': 'Preserve natural colors, continuous gradients, texture and material detail. There '
                        'is no printer color palette or color-count limit. Do not quantize, posterize or '
                        'flatten colors. ',
 'unrestricted_creation': ' Preserve natural colors, continuous gradients, texture, subtle skin tones and '
                          'material detail. There is no printer color palette or color-count limit. Do not '
                          'quantize, posterize, flatten colors, or impose solid-color regions for '
                          'printing. A color explicitly requested by the user remains intentional. ',
 'sculpture_tonal': 'The selected monochrome sculpture style intentionally keeps its single-material '
                    'appearance; preserve tonal shading within that style.'}

# 兼容人像/未知的旧跨题材规则；仅在明确理解旧路径影响时修改。
LEGACY_RULES = {'difficult_structure': 'Difficult-structure rule: for a vehicle, machine, tool, or articulated product, '
                        'keep every wheel, bucket, blade, lens, mirror, handle, and moving attachment '
                        'connected through visibly overlapping solid pin housings, axles, arms, or thick '
                        'opaque rods. Each structural joint must be a positive-volume union: extend every '
                        'axle, piston rod, hinge pin, arm, and brace visibly inside the receiving housing, '
                        'with generous overlap on both sides. Butt contact, near-touching tips, cast '
                        'shadows, painted lines, and a loose pin beside the machine do not count as a '
                        'connection. Keep a bucket or blade merged to its final arm through one thickened '
                        'joint block; keep every linkage merged back to the main chassis. Preserve '
                        'readable joint gaps as shallow recessed grooves instead of separating an '
                        'attachment into another island. If a realistic linkage cannot remain fused, '
                        'simplify it into one solid load-bearing brace while preserving the outer '
                        'silhouette and function. For a fan, feather screen, wing, sail, umbrella canopy, '
                        'leaf, or other broad thin surface, give the surface visible finite thickness and '
                        'fuse its ribs into a continuous rim, hub, body, or trunk; never use a paper-thin '
                        'single sheet. For an open umbrella, make the central shaft penetrate and fuse '
                        'into the canopy hub and the lowest support, embed every rib along the canopy '
                        'instead of leaving wire-like struts, and use shallow panel grooves rather than '
                        'separated fabric panels. For a fan tail or feather display, fuse the screen to a '
                        'broad body or support mass instead of relying on isolated feather tips or thin '
                        'legs alone. When an explicitly requested group contains two or more separate '
                        'people, characters, or animals as one display model, place every subject on one '
                        'shared low integrated base while preserving exact count, spacing, left-right '
                        'order, pose, and individual silhouettes; do not fuse their bodies together merely '
                        'to obtain connectivity. ',
 'non_realistic_text_cleanup': 'NON-REALISTIC TEXT CLEANUP — high priority: remove every readable word, '
                               'brand, logo, serial number, label, watermark, and pseudo-letter from the '
                               'subject as well as the background. Preserve the panel, badge, or engraving '
                               'footprint only as one blank recessed panel, broad unlettered groove, or '
                               'solid color block. Do not copy source glyphs and do not invent plausible '
                               'substitute spelling. ',
 'isolation_and_groups': 'If a thin visible part would otherwise become disconnected, use the smallest '
                         'integrated material bridge or subtle thickening needed for continuity rather '
                         'than adding a display base. Remove scenery, floor shadows, text, logos, '
                         'watermarks, camera UI, color cards, and unrelated people, plants, props, or '
                         'landmarks. Do not combine separate scene elements into one object. Choose every '
                         'subject named by the user when visible. If the user explicitly requests multiple '
                         'subjects, a pair, a group, or a set, preserve the exact requested count, '
                         'identities, left-right order, relative spacing, poses, and accessories as one '
                         'closed composition; never silently drop, merge, duplicate, or replace a '
                         'requested member. Otherwise choose the visually dominant foreground subject. ',
 'composition_support': ', show a coherent complete silhouette, and use a front or gentle three-quarter '
                        'view. Do not add or preserve a presentation base, support, floor slab, or contact '
                        'surface; a source-visible functional part may remain only when it is part of the '
                        'subject itself. ',
 'composition_intro': 'Recompose the selected primary subject as a clean product-shot reference for '
                      'image-to-3D rather than editing the photograph in place. Center the exact requested '
                      'subject or explicitly requested subject group as one readable composition on ',
 'subject_preservation': "Preserve the chosen subject's recognizable identity, facial expression, "
                         'hairstyle, signature clothing or structural features, and visible pose. Simplify '
                         'fine hair strands, fingers, jewelry, fabric patterns, foliage-like texture, and '
                         'shallow surface noise into a few sturdy, connected, modelable forms. Do not turn '
                         'the chosen person, animal, statue, building, or object into a different subject. '
                         'For a person, preserve the exact head angle and gaze direction, adult or child '
                         'age, face aspect ratio, cheekbone placement, chin length, jaw contour, skin-tone '
                         'relationships, hairline, curls, braids, facial hair, eyewear, headwear, visible '
                         'hands, finger grouping, and hand-to-object contact; do not enlarge the eyes, '
                         'shrink the nose or mouth, narrow the jaw, or replace the face with a generic '
                         'doll face. For an animal, preserve its species or breed cues, ear shape and '
                         'count, muzzle length, eye color, limb count, tail pose, and exact coat, feather, '
                         'shell, or scale markings; do not invent a white muzzle, chest patch, socks, '
                         'blaze, or spots that are absent from the source. For a product, vehicle, '
                         'machine, or prop, preserve the exact number and relative placement of wheels, '
                         'handles, openings, windows, lenses, dials, buttons, straps, tools, rods, and '
                         'antennas; do not merge, duplicate, swap, or genericize them. For architecture or '
                         'a statue, preserve tier and opening counts, gestures, symmetry or deliberate '
                         'asymmetry, and any source-visible base; never invent a pedestal when none '
                         'exists. Keep each meaningful thin support, spoke, cable, rail, branch, or '
                         'antenna connected; if printability requires it, thicken it subtly instead of '
                         'deleting or duplicating it. For a plant, bonsai, coral, antler, feather fan, or '
                         'other branching organic subject, use fewer overlapping solid clusters and '
                         'visibly fuse every cluster through sturdy branches or stems to the trunk, body, '
                         'or base; do not leave contact-only shells or isolated leaf pads. Do not invent '
                         'unseen anatomy; use the explicit bust treatment for cropped people instead. '}
