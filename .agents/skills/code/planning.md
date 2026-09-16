# Planning Reference

Consult when a requested plan or substantial dependencies need explicit sequencing. Routine implementation does not require a separate plan.

## When to Plan
- Dependencies or unresolved choices materially affect the outcome
- Work spans stages and needs a durable handoff or recovery record
- The user requests a plan

## Step Format
```
Step N: [What]
- Output: [What exists after]
- Test: [How to verify]
```

## Good Steps
- Clear output (file, endpoint, screen)
- Testable independently
- No ambiguity in what "done" means

## Bad Steps
- "Implement the thing" (vague output)
- No test defined
- Depends on undefined prior step

## Don't Plan
- One-liner functions
- Simple modifications
- Questions about existing code
