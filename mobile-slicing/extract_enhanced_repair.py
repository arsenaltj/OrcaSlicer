"""Compile the complete original enhanced repair entry without desktop strict boolean entry."""
import hashlib,json,pathlib,sys
source=pathlib.Path(sys.argv[1]);output=pathlib.Path(sys.argv[2])
text=source.read_text(encoding='utf8').replace('\r\n','\n')
expected='c2b2e2e670daa0330103a3c6b1d774e15b3b43df79a2d1b2eb3b78fc94534e8e'
if hashlib.sha256(text.encode()).hexdigest()!=expected:
    raise SystemExit('MeshRepair source changed: review exact enhanced implementation boundary')
anchor='namespace {\n\nusing StrictClock'
if text.count(anchor)!=1:
    raise SystemExit('Ambiguous enhanced/strict repair boundary')
body=text[:text.index(anchor)]+'\n} // namespace Slic3r\n'
output.parent.mkdir(parents=True,exist_ok=True)
output.write_text(body,encoding='utf8',newline='\n')
output.with_suffix('.provenance.json').write_text(json.dumps(dict(source=str(source),sourceSha256NormalizedLf=expected,
    completeEnhancedBodySha256=hashlib.sha256(body.encode()).hexdigest(),strictEntryCompiled=False,stubDefinitions=False),indent=2),encoding='utf8')
