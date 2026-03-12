How to use CVS logging:

Enable track logging in opentrack (CSV logging option).
Run your tracker+filter scenario.
Open the CSV and compare:
prefilter_* vs postfilter_* for direct fidelity
filter_delta_* for per-axis filter effect magnitude/sign
This gives you a stable, frame-by-frame fidelity trace without changing filter behavior.