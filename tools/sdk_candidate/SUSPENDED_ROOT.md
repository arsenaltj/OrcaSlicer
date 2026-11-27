# Suspended SDK root profile (disabled by default)

`SDK_SUSPENDED_ROOT_V1` is a separate lifecycle, with effective CreateProcessW
flags `0x80404`. The legacy path remains `0x80400`. Select the new path only with
`suspended_preflight.py`, matching new plan/GO schemas, exact lifecycle contract,
fresh root GO and full file closure. The template deliberately sets enabled=false.
The only permitted commands remain the original marker and readobj/where fixture;
where is data. No package, SDK loading, Host, Native or arbitrary child qualification
is granted. argv, environment v2, cwd, 60-second command budget, admission/tail
resource gates, raw rows and every child image/member rule remain in force.

The local `suspended_launch.py` copies the pinned kernel launch main path exactly,
adding CREATE_SUSPENDED, full original-PI proof before Resume and thread custody.
The three support files stay byte-for-byte unchanged. `suspended_root.py` records
original hProcess/hThread/PID/TID, direct GetProcessTimes/GetProcessId/member true
and flags-zero full image events before a single ResumeThread. Root references use
PRE_RESUME_DIRECT_SAME_CONTINUOUS_GENERATION; they do not represent new queries.
Later identity/member/image contradictions and close/reuse permanently invalidate
references. Final qualification also requires durable full trace and normal closure.

If never resumed, only a known successful Create with a complete continuous original
PI authorizes one TerminateProcess(original hProcess,74), followed by actual WAIT,
exit74 and Job double zero. Failure stays failed independently of cleanup. There is
no PID reopen, foreign control, Job termination or permission change. Any attempted
Resume with count other than exactly one, unknown result, or failed outcome journal
enters CLEANUP_BLOCKED without abort or retry. Partial/unknown Create and unsuccessful
abort also block. The live creating process retains its original handles and waits
without process APIs or automatic recovery. Ctrl-C cannot unwind this hold. The
future GO must accept the creating process custody and give a root response window;
expiry is not permission to discard the handles. Root must explicitly decide any
subsequent recovery. There is no guarantee for an API that never returns.

Pure development tests use inert pinned kernel layouts and fake bindings only.
All actual preflight/Windows API/SDK operations still require a fresh scoped GO.
Original INPUT pre3/post3 and unknown image5/31 causes remain historical facts;
this profile does not rewrite them. An independent failure is the same ID post4 and
stops for root, with no automatic new repair cycle.

New closure includes suspended_preflight.py, suspended_root.py, suspended_launch.py
and the original owner_preflight.py, in addition to the original owner, API context,
adapter/contracts/companion/static proof, three support files, marker and four exact
executables. A frozen source inventory records all local tests/docs/template too;
these are distinct from the actual executable closure. Never call the old15 complete
for this profile. Loaded module origins are checked against these exact paths.
