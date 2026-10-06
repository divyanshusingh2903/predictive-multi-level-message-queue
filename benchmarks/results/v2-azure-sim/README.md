# v2 Azure Functions trace simulation (`v2-azure-sim-1`)

Pre-registered in [docs/v2-preregistration.md](../../../docs/v2-preregistration.md#addendum-azure-functions-trace-simulation-issue-25).

- `simulation.json`: output of `harbinger_trace_sim <trace> benchmarks/configs/v2-azure-sim.json simulation.json`.
- `report.md`, `summary.json`: output of `python -m benchmarks.sim_report benchmarks/configs/v2-azure-sim.json simulation.json`.
- `exploratory-no-aging.json`: **not pre-registered**; the same simulation with aging disabled, used only to diagnose
  why every policy matched FIFO (see the Phase 2 report).

The trace (`AzureFunctionsInvocationTraceForTwoWeeksJan2021.txt`, Zhang et al., SOSP 2021, CC-BY) is not committed;
its hashes are in the config. Download from https://github.com/Azure/AzurePublicDataset (`data/…Jan2021.rar`).
The whole simulation takes under a minute.
