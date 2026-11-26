"""Lossless PACKAGE state journal; completed histories never get rewritten."""
import hashlib
import json
import os
from pathlib import Path


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True, allow_nan=False)


def reconstruct(path):
    state = None
    with Path(path).open('rb') as stream:
        for seq, line in enumerate(stream):
            if not line.endswith(b'\n'):
                raise ValueError('partial execution journal row')
            row = json.loads(line)
            if (type(row) is not dict or set(row) != {'seq','kind','value','sha256'}
                    or type(row['seq']) is not int or type(row['kind']) is not str):
                raise ValueError('invalid execution journal envelope')
            digest = row.pop('sha256')
            if row['seq'] != seq or hashlib.sha256(canonical(row).encode()).hexdigest() != digest:
                raise ValueError('execution journal sequence/checksum differs')
            value = row['value']
            if row['kind'] == 'initial' and state is None:
                state = value
            elif state is None:
                raise ValueError('execution journal missing initial state')
            elif row['kind'] == 'io':
                state['ioSamples'].extend(value)
            elif row['kind'] == 'command':
                index, command = value
                if index == len(state['commands']):
                    state['commands'].append(command)
                elif 0 <= index < len(state['commands']):
                    state['commands'][index] = command
                else:
                    raise ValueError('execution journal command gap')
            elif row['kind'] == 'fields':
                for key in value['deleted']:
                    del state[key]
                state.update(value['set'])
            else:
                raise ValueError('execution journal invalid record kind')
    if state is None:
        raise ValueError('empty execution journal')
    return state


class ExecutionJournal:
    """Only the current command is mutable; full equivalence is checked at close.

    Raw IO samples and completed commands stay in the original in-memory state;
    they are neither dropped nor reserialized for ordinary IO saves. The APIv8
    command journal independently fsyncs each native event before Resume.
    """
    def __init__(self, evidence):
        self.path = Path(evidence) / 'execution.jsonl'
        self.final = Path(evidence) / 'execution.json'
        self.stream = self.path.open('xb')
        self.seq = 0
        self.failed = False
        self.initialized = False
        self.io_count = 0
        self.current = 0
        self.fields = None
        self.field_keys = set()
        self.command_text = None

    def append(self, kind, value):
        if self.failed:
            raise OSError('execution journal permanently failed')
        try:
            row = dict(seq=self.seq, kind=kind, value=value)
            row['sha256'] = hashlib.sha256(canonical(row).encode()).hexdigest()
            encoded = (canonical(row) + '\n').encode()
            if self.stream.write(encoded) != len(encoded):
                raise OSError('short execution journal write')
            self.stream.flush()
            os.fsync(self.stream.fileno())
            self.seq += 1
        except BaseException:
            self.failed = True
            raise

    def save(self, state):
        if self.failed:
            raise OSError('execution journal permanently failed')
        if not self.initialized:
            initial = {k: v for k, v in state.items() if k not in {'commands', 'ioSamples'}}
            self.append('initial', dict(initial, commands=[], ioSamples=[]))
            self.initialized = True
        fields = {k: v for k, v in state.items() if k not in
                  {'commands', 'ioSamples', 'entry', 'sourceAdmissionSamples'}}
        text = canonical(fields)
        if text != self.fields:
            self.append('fields', dict(set=fields, deleted=sorted(self.field_keys-set(fields))))
            self.fields = text
            self.field_keys = set(fields)
        samples = state['ioSamples']
        if len(samples) < self.io_count:
            raise ValueError('execution IO history shortened')
        if len(samples) > self.io_count:
            self.append('io', samples[self.io_count:])
            self.io_count = len(samples)
        commands = state['commands']
        # Snapshot the last mutable command, then freeze it when the next starts.
        while self.current < len(commands):
            command = commands[self.current]
            text = canonical(command)
            if text != self.command_text:
                self.append('command', [self.current, command])
                self.command_text = text
            if self.current == len(commands) - 1:
                break
            self.current += 1
            self.command_text = None

    def write_final(self, state):
        pending = self.final.with_suffix('.pending.json')
        with pending.open('x', encoding='utf8') as stream:
            json.dump(state, stream, indent=2, allow_nan=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, self.final)

    def command_finished(self, state):
        # run_owned has naturally drained and populated its last cleanup fields.
        # Freeze even the final command, without waiting for another dispatch.
        self.save(state)
        self.current = len(state['commands'])
        self.command_text = None

    def finish(self, state):
        self.save(state)
        self.stream.flush()
        restored = reconstruct(self.path)
        if canonical(restored) != canonical(state):
            raise ValueError('complete execution journal differs from final state')
        self.write_final(state)

    def close(self):
        self.stream.close()
