# AIML Model Cards: LSM-Tree Key-Value Engine

## Tier A: Learned Compaction Scheduler & Workload Drift Detector (vs fixed leveled policy)
- **Baseline**: Non-AI Static Baseline
- **Target**: Statistically significant win on stated evaluation metric.
- **Status**: Specified & Scaffolded (**TO BE MEASURED**).
- **Graceful Fallback**: Hardcoded static threshold rule engine.

## Tier B: Natural language to KV Get/Scan query engine and tuning advisor
- **LLM Prompt Guard**: Strict schema validation. The LLM is NEVER on correctness or transactional paths.
