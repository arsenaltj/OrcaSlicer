"""Fixed ENV-only child: prints selected presence/empty/hash, never values."""
import hashlib
import json
import os

KEYS=('CODEX_ENV_TEST_MISSING','CODEX_ENV_TEST_EMPTY','CODEX_ENV_TEST_TEXT','CODEX_ENV_TEST_UNICODE',
      'GIT_CONFIG_COUNT','GIT_CONFIG_KEY_0','GIT_CONFIG_VALUE_0')
print(json.dumps(dict(environment={key:dict(present=key in os.environ,
    empty=os.environ.get(key)=='',sha256=hashlib.sha256(os.environ[key].encode()).hexdigest() if key in os.environ else None)
    for key in KEYS},cwdSha256=hashlib.sha256(os.getcwd().encode()).hexdigest(),
    nativeEnvironmentSha256=hashlib.sha256(json.dumps(dict(os.environ),sort_keys=True).encode()).hexdigest()),sort_keys=True))
