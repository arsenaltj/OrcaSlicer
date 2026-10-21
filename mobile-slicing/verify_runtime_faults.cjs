// Real-device public-ABI resource and storage fault gate. Requires root adb.
// Does not touch the installed application's projects or database.
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const args = {};
for (let i = 2; i < process.argv.length; i += 2) { assert(process.argv[i].startsWith('--')); args[process.argv[i].slice(2)] = process.argv[i + 1]; }
const required = ['adb', 'serial', 'model', 'package-dir', 'gate', 'out', 'run-id'];
for (const key of required) assert(args[key], '--' + key + ' is required');
assert(/^[a-zA-Z0-9_-]+$/.test(args['run-id']));
assert(path.isAbsolute(args.out) && path.isAbsolute(args.model));
const out = path.join(args.out, args['run-id']);
fs.mkdirSync(out, { recursive: false });
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const sh = text => "'" + String(text).replaceAll("'", "'\\''") + "'";
let counter = 0;
function adb(argv, label, allowFailure = false) {
  const id = String(++counter).padStart(4, '0') + '-' + label;
  const r = cp.spawnSync(args.adb, ['-s', args.serial, ...argv], { maxBuffer: 96 * 1024 * 1024, timeout: 180000 });
  fs.writeFileSync(path.join(out, id + '.stdout'), r.stdout || Buffer.alloc(0));
  fs.writeFileSync(path.join(out, id + '.stderr'), r.stderr || Buffer.alloc(0));
  fs.appendFileSync(path.join(out, 'actions.jsonl'), JSON.stringify({ at: new Date().toISOString(), argv, exit: r.status, signal: r.signal, error: r.error?.message, stdout: id + '.stdout', stderr: id + '.stderr' }) + '\n');
  if (!allowFailure) assert.equal(r.status, 0, label + ': ' + String(r.stderr));
  return r;
}
function shell(script, label, allowFailure) { return adb(['shell', script], label, allowFailure); }
function read(remote) {
  const r = adb(['exec-out', 'cat ' + sh(remote) + '; remote_code=$?; printf \'\\nORCA_QA_READ_EXIT=%03d\\n\' "$remote_code"'], 'read-result');
  const marker = Buffer.from('\nORCA_QA_READ_EXIT=');
  const i = r.stdout.lastIndexOf(marker);
  assert(i >= 0, 'Remote exit trailer missing');
  assert.equal(r.stdout.subarray(i + marker.length).toString().trim(), '000', 'Remote read failed');
  return r.stdout.subarray(0, i);
}
function json(remote) { return JSON.parse(read(remote).toString()); }
function publish(file, value) { const local = path.join(out, path.basename(file)); fs.writeFileSync(local, JSON.stringify(value)); adb(['push', local, file], 'push-request'); }
const remote = '/data/local/tmp/orca-runtime-faults-' + args['run-id'];
const model = remote + '/input' + path.extname(args.model).toLowerCase();
const summary = { runId: args['run-id'], startedAt: new Date().toISOString(), scope: 'Standalone ABI using the pinned app library; not an app UI/SQLite test', input: { source: args.model, sha256: sha(fs.readFileSync(args.model)) }, cases: [], state: 'started' };
const save = () => fs.writeFileSync(path.join(out, 'summary.json'), JSON.stringify(summary, null, 2));
let mounted = false;
const quota = remote + '/quota-output';
try {
  assert.equal(shell('id -u', 'root').stdout.toString().trim(), '0');
  shell('test ! -e ' + sh(remote) + ' && mkdir -p ' + sh(remote + '/lib') + ' ' + sh(remote + '/workspace') + ' ' + sh(quota), 'create-owned-run');
  adb(['push', args.model, model], 'push-original');
  summary.libraries = [];
  for (const name of fs.readdirSync(args['package-dir']).filter(n => n.endsWith('.so'))) {
    const file = path.join(args['package-dir'], name); const digest = sha(fs.readFileSync(file));
    adb(['push', file, remote + '/lib/' + name], 'push-library');
    const device = shell('sha256sum ' + sh(remote + '/lib/' + name), 'library-hash').stdout.toString().split(/\s+/)[0];
    assert.equal(device, digest); summary.libraries.push({ name, sha256: digest });
  }
  adb(['push', args.gate, remote + '/gate'], 'push-gate'); shell('chmod 755 ' + sh(remote + '/gate'), 'executable');
  const base = { schemaVersion: 2, inputPath: model, inputSha256: summary.input.sha256, projectId: args['run-id'], projectRevision: 1, quality: 'standard', maxThreads: 4, projectCommands: [{ type: 'singleMaterial', filamentIndex: 0 }] };
  function execute(name, overrides, expectedCode) {
    const output = name === 'disk-full' ? quota : remote + '/' + name;
    shell('mkdir -p ' + sh(output), 'output-directory');
    const request = { ...base, operation: 'slice', outputDir: output, jobId: name, attemptId: 'a1', ...overrides };
    const requestPath = remote + '/' + name + '.request.json', resultPath = remote + '/' + name + '.result.json';
    publish(requestPath, request);
    const reply = shell('env LD_LIBRARY_PATH=' + sh(remote + '/lib') + ' TMPDIR=' + sh(remote + '/workspace') + ' ' + sh(remote + '/gate') + ' ' + sh(remote + '/workspace') + ' ' + sh(requestPath) + ' ' + sh(resultPath), 'execute-' + name, true);
    const answer = json(resultPath); fs.writeFileSync(path.join(out, name + '.result.json'), JSON.stringify(answer, null, 2));
    if (expectedCode) { assert.notEqual(reply.status, 0); assert.equal(answer.ok, false); assert.equal(answer.error?.code, expectedCode); assert(!answer.result?.gcodePath); }
    else { assert.equal(reply.status, 0); assert.equal(answer.ok, true); const result = answer.result; const bytes = read(result.gcodePath); assert(bytes.length > 1000 && bytes.toString().includes('; HEADER_BLOCK_START') && bytes.toString().includes(';LAYER_CHANGE')); assert.equal(bytes.length, result.gcodeSize); assert.equal(sha(bytes), result.gcodeSha256); fs.writeFileSync(path.join(out, 'recovered.gcode'), bytes); }
    const item = { name, exitCode: reply.status, sourceCommit: answer.sourceCommit, error: answer.error, state: 'passed', output: name + '.result.json' }; summary.cases.push(item); save(); return answer;
  }
  execute('memory-low-threshold', { maxMemoryBytes: 1 }, 'MEMORY_LIMIT');
  execute('recommendation-low-timeout', { operation: 'recommend', maxRecommendationMs: 1 }, 'TIMEOUT');
  shell('mount -t tmpfs -o size=24m tmpfs ' + sh(quota), 'mount-owned-quota'); mounted = true;
  summary.quotaBefore = shell('df -k ' + sh(quota), 'quota-before').stdout.toString();
  const failed = execute('disk-full', {}, 'SLICING_FAILED');
  summary.quotaAfter = shell('df -k ' + sh(quota), 'quota-after').stdout.toString();
  const quotaFields = summary.quotaAfter.trim().split(/\r?\n/).at(-1).trim().split(/\s+/);
  assert.equal(Number(quotaFields[3]), 0, 'Quota is not full; another failure cannot be called ENOSPC');
  assert(/space|disk|write|flush|scene|snapshot/i.test(failed.error.message), 'Disk failure was not a write failure');
  shell('umount ' + sh(quota), 'unmount-owned-quota'); mounted = false;
  execute('ordinary-after-faults', {}, null);
  summary.input.deviceSha256 = shell('sha256sum ' + sh(model), 'original-still-intact').stdout.toString().split(/\s+/)[0]; assert.equal(summary.input.deviceSha256, summary.input.sha256);
  summary.limitations = ['Low thresholds test the same resource-exit mechanism; they do not claim an actual 2GiB/10-minute boundary crossing.', 'A 24MiB tmpfs is restricted to this standalone run output; application SQLite and all existing projects are untouched.'];
  summary.state = 'passed';
} catch (error) { summary.state = 'failed'; summary.error = String(error.stack || error); process.exitCode = 1; }
finally {
  if (mounted) { const r = shell('umount ' + sh(quota), 'finally-unmount-owned-quota', true); if (r.status !== 0) { summary.cleanupError = String(r.stderr); summary.state = 'failed'; process.exitCode = 1; } }
  summary.finishedAt = new Date().toISOString(); save(); console.log(JSON.stringify({ runId: summary.runId, state: summary.state, cases: summary.cases, error: summary.error, cleanupError: summary.cleanupError }, null, 2));
}
