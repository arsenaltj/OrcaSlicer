"""Pinned CGAL predicate: release dead edge_set before the second workspace."""
from pathlib import Path
import hashlib
import json
import sys

source, target = map(Path, sys.argv[1:])
data = source.read_bytes()
if hashlib.sha256(data).hexdigest() != '223bb7a1b6bbde75141a75cf5f05efbfc697e132c7941ba8a36c1985a91f224d':
    raise SystemExit('Pinned CGAL soup header SHA mismatch')
text = data.decode().replace('\r\n', '\n')
start_anchor = 'template<typename PolygonRange>\nbool is_polygon_soup_a_polygon_mesh('
if text.count(start_anchor) != 1:
    raise SystemExit('CGAL soup function must have one exact anchor')
start = text.index(start_anchor)
end = text.index('\n}\n', start) + 2
original = text[start:end]
if hashlib.sha256(original.encode()).hexdigest() != '0852efdd0fd4fdba51af441f7ccdfff0b3820b90df08f28a0ecc82343e23abe9':
    raise SystemExit('Pinned complete CGAL soup function SHA mismatch')
anchor = '  //check manifoldness'
type_anchor = '  typedef typename boost::range_value<Polygon>::type                V_ID;'
if original.count(anchor) != 1 or original.count(type_anchor) != 1 or original.count('edge_set') != 2:
    raise SystemExit('CGAL dead-set lifetime anchors changed')
adapted = original.replace(anchor, '  edge_set.clear();\n\n' + anchor)
adapted = adapted.replace(type_anchor, type_anchor + '\n  static_assert(std::is_integral<V_ID>::value && std::is_trivially_destructible<V_ID>::value, "Private lifetime adapter requires trivial integral vertex IDs");')
# Restore the two declared edits and prove all original checks/branches/returns.
restored = adapted.replace('  edge_set.clear();\n\n', '').replace('\n  static_assert(std::is_integral<V_ID>::value && std::is_trivially_destructible<V_ID>::value, "Private lifetime adapter requires trivial integral vertex IDs");', '')
if restored != original:
    raise SystemExit('CGAL adapter must retain the complete original function')
license_text = text[:text.index('#ifndef')]
header = license_text + '#pragma once\n#include <type_traits>\n#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>\n'
header += '// Private compile-local adapter; original upstream symbol is unchanged.\n// Only a dead set is cleared; source/check/return identity guarded by the extractor.\nnamespace CGAL { namespace Polygon_mesh_processing { namespace orca_model_bounded {\n'
header += adapted + '\n} } }\n'
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(header, encoding='utf-8')
print(json.dumps({'sourceSha256': hashlib.sha256(data).hexdigest(), 'functionSha256NormalizedLf': hashlib.sha256(original.encode()).hexdigest(), 'adapterSha256': hashlib.sha256(header.encode()).hexdigest(), 'originalFunctionRestoredExactly': True, 'upstreamSymbolReplaced': False}))
